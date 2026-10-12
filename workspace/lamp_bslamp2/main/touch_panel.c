/*
 * touch_panel.c — bslamp2 触控面板驱动（I²C）
 *
 * 协议出处：esphome-xiaomi_bslamp2 (mmakaay) 的
 *   components/xiaomi_bslamp2/front_panel_hal.h
 *   packages/core.yaml（从机地址 0x2C、SDA=GPIO21、SCL=GPIO19、trigger=GPIO16）
 * 本工程把它移植为 C，纯解析部分在 panel_protocol.c（有单元测试）。
 *
 * 本文件只做三件事：
 *   1. I²C 总线与面板中断（GPIO16 下降沿）的初始化与自检
 *   2. "write READY_FOR_EV → read 7 字节 → panel_parse_event" 的事务
 *   3. 把协议事件翻译成 touch_panel_event_t 交给 app_main
 *
 * 注意：这里不写 Flash、不调用 pet_reg_* —— 那些在 app_main 的回调里做
 * （需求 §6.3：不要在中断/高频上下文写 Flash；面板事件虽低频，但保持分层）。
 */
#include "touch_panel.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "app_config.h"
#include "panel_protocol.h"

static const char *TAG = "panel";

#define PANEL_I2C_PORT         I2C_NUM_0
#define PANEL_I2C_FREQ_HZ      100000
#define PANEL_EVENT_QUEUE_LEN  8
#define PANEL_I2C_TIMEOUT_MS   50
#define PANEL_POLL_INTERVAL_MS 20
/** 没有中断时最多隔多少次轮询主动读一次（20 × 20 ms = 400 ms） */
#define PANEL_IDLE_POLLS       20

/**
 * 面板 I²C 从机地址。
 * 出处：上游 packages/core.yaml 的 xiaomi_bslamp2.front_panel.address: 0x2C
 */
#ifndef PANEL_I2C_ADDR
#define PANEL_I2C_ADDR 0x2Cu
#endif

/**
 * 1 = 使用真实 I²C 时序（当前值，协议已移植并单元测试）。
 * 0 = 保守回退：只初始化总线，不产生事件。
 */
#ifndef PANEL_PROTOCOL_CONFIRMED
#define PANEL_PROTOCOL_CONFIRMED 1
#endif

static bool               s_inited;
static bool               s_bus_ok;
static bool               s_panel_found;
static QueueHandle_t      s_evt_queue;
static touch_panel_cb_t   s_cb;
static TaskHandle_t       s_task;

/* 面板每产生一个事件就把 GPIO16 拉低一次（下降沿） */
static volatile uint32_t  s_irq_count;
/* 最近一次写出的指示灯状态，避免重复占用 I²C */
static uint16_t           s_last_leds = 0xFFFFu;

/* ------------------------------------------------------------------ */
/* I²C 事务                                                           */
/* ------------------------------------------------------------------ */

/**
 * 写 7 字节到面板。
 *
 * 面板把整条消息当作命令（没有额外的 register 字节），
 * 因此 READY_FOR_EV 是**裸写**——与上游 I2CDevice::write 的行为一致。
 */
static esp_err_t panel_write(const uint8_t msg[PANEL_MSG_LEN])
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (cmd == NULL) {
        return ESP_ERR_NO_MEM;
    }
    (void)i2c_master_start(cmd);
    (void)i2c_master_write_byte(cmd,
                                (uint8_t)((PANEL_I2C_ADDR << 1) | I2C_MASTER_WRITE),
                                true);
    (void)i2c_master_write(cmd, msg, PANEL_MSG_LEN, true);
    (void)i2c_master_stop(cmd);
    const esp_err_t err = i2c_master_cmd_begin(PANEL_I2C_PORT, cmd,
                                              pdMS_TO_TICKS(PANEL_I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    return err;
}

/** 从面板读 7 字节（最后一个字节回 NACK） */
static esp_err_t panel_read(uint8_t out[PANEL_MSG_LEN])
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (cmd == NULL) {
        return ESP_ERR_NO_MEM;
    }
    (void)i2c_master_start(cmd);
    (void)i2c_master_write_byte(cmd,
                                (uint8_t)((PANEL_I2C_ADDR << 1) | I2C_MASTER_READ),
                                true);
    (void)i2c_master_read(cmd, out, PANEL_MSG_LEN - 1u, I2C_MASTER_ACK);
    (void)i2c_master_read_byte(cmd, &out[PANEL_MSG_LEN - 1u], I2C_MASTER_NACK);
    (void)i2c_master_stop(cmd);
    const esp_err_t err = i2c_master_cmd_begin(PANEL_I2C_PORT, cmd,
                                              pdMS_TO_TICKS(PANEL_I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    return err;
}

/** 探测面板是否在 0x2C 应答 */
static esp_err_t panel_probe(void)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (cmd == NULL) {
        return ESP_ERR_NO_MEM;
    }
    (void)i2c_master_start(cmd);
    (void)i2c_master_write_byte(cmd,
                                (uint8_t)((PANEL_I2C_ADDR << 1) | I2C_MASTER_WRITE),
                                true);
    (void)i2c_master_stop(cmd);
    const esp_err_t err = i2c_master_cmd_begin(PANEL_I2C_PORT, cmd,
                                              pdMS_TO_TICKS(PANEL_I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    return err;
}

/* ------------------------------------------------------------------ */
/* 面板中断                                                           */
/* ------------------------------------------------------------------ */

/**
 * 面板在"有新事件可读"时把 GPIO16 拉低一小段时间。
 * ISR 只做计数（与 BLE 回调同一原则：中断/回调里不做 I/O）。
 */
static void IRAM_ATTR panel_gpio_isr(void *arg)
{
    (void)arg;
    ++s_irq_count;
}

/* ------------------------------------------------------------------ */
/* 总线                                                               */
/* ------------------------------------------------------------------ */

static esp_err_t bus_init(void)
{
    i2c_config_t cfg = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = PIN_PANEL_I2C_SDA,
        .scl_io_num       = PIN_PANEL_I2C_SCL,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = PANEL_I2C_FREQ_HZ,
    };
    esp_err_t err = i2c_param_config(PANEL_I2C_PORT, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_param_config 失败: %s", esp_err_to_name(err));
        return err;
    }
    /* 面板是纯从机，不需要收发缓冲 */
    err = i2c_driver_install(PANEL_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_driver_install 失败: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

static esp_err_t bus_probe_addr(uint8_t addr)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (cmd == NULL) {
        return ESP_ERR_NO_MEM;
    }
    (void)i2c_master_start(cmd);
    (void)i2c_master_write_byte(cmd, (uint8_t)((addr << 1) | I2C_MASTER_WRITE), true);
    (void)i2c_master_stop(cmd);
    const esp_err_t err = i2c_master_cmd_begin(PANEL_I2C_PORT, cmd,
                                              pdMS_TO_TICKS(PANEL_I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    return err;
}

esp_err_t touch_panel_scan_bus(void)
{
    if (!s_bus_ok) {
        ESP_LOGW(TAG, "I²C 总线未就绪，无法扫描");
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t found = 0u;
    ESP_LOGI(TAG, "扫描面板 I²C 总线（SDA=%d SCL=%d）…",
             PIN_PANEL_I2C_SDA, PIN_PANEL_I2C_SCL);

    for (uint8_t addr = 0x08u; addr < 0x78u; ++addr) {
        if (bus_probe_addr(addr) == ESP_OK) {
            ESP_LOGI(TAG, "  发现从机 0x%02X%s", (unsigned)addr,
                     (addr == PANEL_I2C_ADDR) ? "  <- 面板（期望值）" : "");
            ++found;
        }
    }
    if (found == 0u) {
        ESP_LOGW(TAG, "  未发现任何从机 —— 检查排线，或面板由其他总线控制");
    }
    return (found > 0u) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/* ------------------------------------------------------------------ */
/* 事件读取                                                           */
/* ------------------------------------------------------------------ */

/**
 * 读一个面板事件并解析。时序与上游 FrontPanelHAL::loop 一致：
 *   1. write READY_FOR_EV
 *   2. read 7 字节
 *   3. panel_parse_event() 校验
 */
static bool panel_read_event(panel_event_t *out)
{
    if (panel_write(PANEL_READY_FOR_EV) != ESP_OK) {
        ESP_LOGD(TAG, "写 READY_FOR_EV 失败");
        return false;
    }
    uint8_t raw[PANEL_MSG_LEN];
    memset(raw, 0, sizeof(raw));
    if (panel_read(raw) != ESP_OK) {
        ESP_LOGD(TAG, "读事件失败");
        return false;
    }
    panel_parse_event(raw, out);
    if (!out->ok) {
        ESP_LOGW(TAG, "事件解析失败（%s）：[%02X:%02X:%02X:%02X:%02X:%02X:%02X]",
                 panel_parse_err_name(out->error), raw[0], raw[1], raw[2], raw[3],
                 raw[4], raw[5], raw[6]);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* 面板任务                                                           */
/* ------------------------------------------------------------------ */

static void panel_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    uint32_t last_irq = 0u;
    uint8_t idle = 0u;

    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(PANEL_POLL_INTERVAL_MS));

        /*
         * 中断驱动优先：只有中断计数变化才去读 I²C。
         * 若面板中断线没接好（计数不变），退化为周期轮询（约 400 ms 一次），
         * 否则会永远收不到事件。
         */
        const uint32_t irq = s_irq_count;
        if (irq == last_irq) {
            if (++idle < PANEL_IDLE_POLLS) {
                continue;
            }
        }
        idle = 0u;
        last_irq = irq;

        panel_event_t pe;
        memset(&pe, 0, sizeof(pe));
        if (!panel_read_event(&pe)) {
            continue;
        }

        touch_panel_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = TOUCH_EVENT_NONE;

        switch (pe.part) {
            case PANEL_EV_PART_POWER:
                if (panel_is_touch_type(pe.type)) {
                    ev.type = TOUCH_EVENT_POWER_SHORT;
                }
                /* 松开不产生事件：短按/长按的判别由 app_main 按时长判定 */
                break;

            case PANEL_EV_PART_COLOR:
                if (panel_is_touch_type(pe.type)) {
                    ev.type = TOUCH_EVENT_COLOR_SHORT;
                }
                break;

            case PANEL_EV_PART_SLIDER:
                /*
                 * 滑条档位 0..22 → 0–100%。
                 * 只在**松开**时上报最终值：拖动过程中每 20 ms 上报一次既没意义
                 * （用户还没松手），还会让上层反复写 NVS（需求 §6.3）。
                 */
                if (!panel_is_touch_type(pe.type)) {
                    ev.type = TOUCH_EVENT_BRIGHTNESS;
                    ev.value = (uint8_t)((pe.level * 100u +
                                          (PANEL_SLIDER_MAX_LEVEL / 2u)) /
                                         PANEL_SLIDER_MAX_LEVEL);
                }
                break;

            default:
                break;
        }

        if (ev.type == TOUCH_EVENT_NONE) {
            continue;
        }
        ev.time_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        ESP_LOGI(TAG, "面板事件：%s%s", panel_part_name(pe.part),
                 (ev.type == TOUCH_EVENT_BRIGHTNESS) ? "（亮度）" : "");

        if (s_cb != NULL) {
            s_cb(&ev);      /* 面板事件频率很低，直接回调 */
        } else {
            (void)xQueueSend(s_evt_queue, &ev, 0);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 公开接口                                                           */
/* ------------------------------------------------------------------ */

esp_err_t touch_panel_init(touch_panel_cb_t cb)
{
    if (s_inited) {
        return ESP_OK;
    }
    s_cb = cb;

    /* 中断脚：面板 → ESP32，下降沿表示"有新事件" */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_PANEL_INT),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "面板中断脚配置失败: %s", esp_err_to_name(err));
    }

    s_evt_queue = xQueueCreate(PANEL_EVENT_QUEUE_LEN, sizeof(touch_panel_event_t));
    if (s_evt_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_bus_ok = (bus_init() == ESP_OK);
    s_inited = true;

    if (!s_bus_ok) {
        ESP_LOGW(TAG, "I²C 总线不可用：面板功能禁用（核心灯效不受影响）");
        return ESP_ERR_INVALID_STATE;
    }

    /* 启动自检：面板应在 0x2C 应答 */
    s_panel_found = (panel_probe() == ESP_OK);
    if (s_panel_found) {
        ESP_LOGI(TAG, "面板在线：地址 0x%02X，中断 GPIO%d，I²C %d Hz",
                 (unsigned)PANEL_I2C_ADDR, PIN_PANEL_INT, PANEL_I2C_FREQ_HZ);
    } else {
        ESP_LOGW(TAG, "面板在 0x%02X 无应答；扫描总线帮助定位：",
                 (unsigned)PANEL_I2C_ADDR);
        (void)touch_panel_scan_bus();
    }

    /* 注册中断；GPIO ISR 服务可能已被别处安装，这不是错误 */
    (void)gpio_install_isr_service(0);
    if (gpio_isr_handler_add(PIN_PANEL_INT, panel_gpio_isr, NULL) != ESP_OK) {
        ESP_LOGW(TAG, "注册面板中断失败；将退化为周期轮询");
    }

    if (!s_panel_found) {
        ESP_LOGW(TAG, "面板未就绪：不启动事件任务（核心功能不受影响）");
        return ESP_ERR_NOT_FOUND;
    }

#if PANEL_PROTOCOL_CONFIRMED
    xTaskCreate(panel_task, "panel", 3072, NULL, 3, &s_task);
    ESP_LOGI(TAG, "面板事件任务已启动（协议移植自 esphome-xiaomi_bslamp2）");
#else
    ESP_LOGW(TAG, "PANEL_PROTOCOL_CONFIRMED=0：不产生事件（保守模式）");
#endif
    return ESP_OK;
}

bool touch_panel_poll(touch_panel_event_t *out)
{
    if (out == NULL || s_evt_queue == NULL) {
        return false;
    }
    return (xQueueReceive(s_evt_queue, out, 0) == pdTRUE);
}

bool touch_panel_protocol_confirmed(void)
{
    return (PANEL_PROTOCOL_CONFIRMED != 0) && s_panel_found;
}

/* ------------------------------------------------------------------ */
/* 面板指示灯（12 颗）                                                 */
/* ------------------------------------------------------------------ */

esp_err_t touch_panel_set_leds(uint16_t led_bits)
{
    if (!s_bus_ok) {
        return ESP_ERR_INVALID_STATE;
    }
    if (led_bits == s_last_leds) {
        return ESP_OK;      /* 只在变化时写总线 */
    }
    uint8_t msg[PANEL_MSG_LEN];
    panel_encode_leds(led_bits, msg);

    const esp_err_t err = panel_write(msg);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "写面板指示灯失败: %s", esp_err_to_name(err));
        return err;
    }
    s_last_leds = led_bits;
    return ESP_OK;
}

uint16_t touch_panel_slider_leds_for_ratio(float ratio)
{
    return panel_slider_leds(ratio);
}

uint32_t touch_panel_irq_count(void)
{
    return s_irq_count;
}
