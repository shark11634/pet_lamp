/*
 * lamp_esp32.c — core/lamp_driver 的 ESP32 LEDC 适配层
 *
 * 本文件只做三件事：
 *   1. 配置两个 LEDC 定时器（RGB 3 kHz / 白光 10 kHz，与上游一致）
 *      与 4 个通道（红0 绿1 蓝2 白4，同样与上游一致）
 *   2. 拉高两路主开关
 *   3. 把 core 算好的占空比写进 LEDC
 *
 * ⚠️ 这里**没有** Gamma 校正，也没有全通道限流 —— 那些在 core/lamp_duty.c
 *    与 core/lamp_gamma.c，由 test/host_effects_test.c 与集成测试覆盖。
 *    如果在这里"顺手"再加一份，就会重演"两份实现漂移"的老问题。
 *
 * 关于分辨率的实现细节：ESP32 的 LEDC 在 10 kHz 下由驱动自动选择分频系数
 * （10 kHz 需要 8000 分频，超过硬件 1024 上限，驱动会启用小数分频）。
 * 本工程按 14 bit 请求；若某个 IDF 版本在 10 kHz 下只能给 13 bit，
 * 也只是白色通道的量化粒度略有损失，不影响功能。
 */
#include "lamp_esp32.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"

#include "app_config.h"

static const char *TAG = "lamp";

/* 通道顺序必须与 core 的 lamp_color_t（r,g,b,w）一致 */
const uint8_t lamp_esp32_gpio[LAMP_CH_COUNT] = {
    PIN_LED_RED, PIN_LED_GREEN, PIN_LED_BLUE, PIN_LED_WHITE,
};

/** 每个颜色通道对应的 LEDC 通道号（与上游 core.yaml 一致：白=通道4） */
static const ledc_channel_t s_ch[LAMP_CH_COUNT] = {
    (ledc_channel_t)LAMP_LEDC_CH_RED,    /* 0 */
    (ledc_channel_t)LAMP_LEDC_CH_GREEN,  /* 1 */
    (ledc_channel_t)LAMP_LEDC_CH_BLUE,   /* 2 */
    (ledc_channel_t)LAMP_LEDC_CH_WHITE,  /* 4 */
};

/** 每个颜色通道使用的定时器（RGB 共用 0；白光单独用 1） */
static const ledc_timer_t s_timer[LAMP_CH_COUNT] = {
    LEDC_TIMER_0, LEDC_TIMER_0, LEDC_TIMER_0, LEDC_TIMER_1,
};

/**
 * 每个颜色通道的 LEDC 分辨率位数。
 *
 * 白色是 13 bit 而非 14 —— 因为 10 kHz @ 14 bit 在 ESP32 LEDC 上不可达
 * （需要分频 0.49 < 1）。原因与取舍写在 app_config.h 的分辨率×频率表旁。
 */
static const uint8_t s_bits[LAMP_CH_COUNT] = {
    LAMP_LEDC_BITS_RGB, LAMP_LEDC_BITS_RGB, LAMP_LEDC_BITS_RGB,
    LAMP_LEDC_BITS_WHITE,
};

/**
 * 把 core 的占空比（LAMP_PWM_RES_BITS 尺度）折算到某通道的实际位数。
 *
 * 用**右移**而不是自己写一套比例运算：core 的占空比与逻辑亮度是单调关系，
 * 右移保持单调，且不会把非零值抹成零（位宽相差 1 时不会）。
 * 这也刻意与 `core/lamp_gamma.h` 的 `lamp_duty_to_bits()` 区别开 ——
 * 那个函数是为"低位数平台（如 8 bit analogWrite）"设计的通用换算，
 * 这里只是同族位宽的简单折算，且两边都有测试覆盖。
 */
static inline uint32_t scale_duty(uint16_t duty, int ch)
{
    /* 先钳到 core 的满量程，再折算。
     * 顺序很重要：不钳位直接右移的话，0xFFFF >> 1 仍是 32767，
     * 远超 13 bit 上限 —— 集成测试的"越界钳位"用例就是为此设的。 */
    uint32_t d = duty;
    if (d > (uint32_t)LAMP_PWM_MAX_DUTY) {
        d = (uint32_t)LAMP_PWM_MAX_DUTY;
    }

    const int shift = (int)LAMP_PWM_RES_BITS - (int)s_bits[ch];
    if (shift > 0) {
        const uint32_t before = d;
        d >>= shift;
        if (before > 0u && d == 0u) {
            d = 1u;      /* 非零不得被抹成零（低亮呼吸的地板） */
        }
    }

    const uint32_t max = (1u << s_bits[ch]) - 1u;
    return (d > max) ? max : d;
}

static uint16_t s_last_duty[LAMP_CH_COUNT];
static bool     s_ready;

/* ------------------------------------------------------------------ */
/* core 的输出回调                                                      */
/* ------------------------------------------------------------------ */

/**
 * core 在每帧灯效更新后调用这里。
 *
 * duty[i] 已经是最终占空比（含 Gamma / 限流 / 层调制），单位
 * 0..LAMP_PWM_MAX_DUTY，与 LEDC 的 LAMP_PWM_RES_BITS 满量程一致，
 * 因此**原样写入即可，不需要任何换算**。
 */
static void lamp_esp32_write(const uint16_t *duty, void *user)
{
    (void)user;
    if (!s_ready || duty == NULL) {
        return;
    }
    for (int i = 0; i < LAMP_CH_COUNT; ++i) {
        const uint32_t d = scale_duty(duty[i], i);
        s_last_duty[i] = (uint16_t)d;
        if (ledc_set_duty(LAMP_LEDC_SPEED_MODE, s_ch[i], d) != ESP_OK) {
            ESP_LOGD(TAG, "ledc_set_duty 失败 ch=%d", i);
            continue;
        }
        (void)ledc_update_duty(LAMP_LEDC_SPEED_MODE, s_ch[i]);
    }
}

/* ------------------------------------------------------------------ */
/* 初始化                                                              */
/* ------------------------------------------------------------------ */

esp_err_t lamp_esp32_power_enable(bool enable)
{
    const uint32_t level = enable ? 1u : 0u;
    /* gpio_set_level 的返回类型在 ESP-IDF 各版本为 void，不检查返回值
     * 以保持跨版本可编译。 */
    gpio_set_level(PIN_LED_SWITCH_1, level);
    gpio_set_level(PIN_LED_SWITCH_2, level);
    return ESP_OK;
}

/** 配置一个 LEDC 定时器 */
static esp_err_t timer_setup(ledc_timer_t timer, uint32_t freq_hz, uint8_t bits,
                             const char *name)
{
    ledc_timer_config_t tcfg = {
        .speed_mode      = LAMP_LEDC_SPEED_MODE,
        .timer_num       = timer,
        .duty_resolution = (ledc_timer_bit_t)bits,
        .freq_hz         = freq_hz,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    const esp_err_t err = ledc_timer_config(&tcfg);
    if (err != ESP_OK) {
        /* 最常见的原因是分辨率×频率组合不可达（分频 < 1）。
         * app_config.h 里已有编译期检查，这里再兜一层运行时日志。 */
        ESP_LOGE(TAG, "LEDC 定时器 %s 配置失败（请求 %u Hz / %u bit）: %s",
                 name, (unsigned)freq_hz, (unsigned)bits, esp_err_to_name(err));
        return err;
    }
    /* 回读实际生效的载波与分辨率 —— 整数分频下与请求值会有偏差，
     * 记一条日志便于实测核对（例如 3 kHz 实际约 2.44 kHz）。 */
    ESP_LOGI(TAG, "LEDC %s: 请求 %u Hz / %u bit，实际载波 %u Hz",
             name, (unsigned)freq_hz, (unsigned)bits,
             (unsigned)ledc_get_freq(LAMP_LEDC_SPEED_MODE, timer));
    return ESP_OK;
}

esp_err_t lamp_esp32_init(lamp_driver_t *drv)
{
    if (drv == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(s_last_duty, 0, sizeof(s_last_duty));

    /* ---- 主开关：先断开，避免上电瞬间亮闪 ---- */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_LED_SWITCH_1) | (1ULL << PIN_LED_SWITCH_2),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "主开关 GPIO 配置失败: %s", esp_err_to_name(err));
        return err;
    }
    (void)lamp_esp32_power_enable(false);

    /* ---- 两个定时器：RGB 与白光频率不同（与上游一致） ---- */
    err = timer_setup(LAMP_LEDC_TIMER_RGB, LAMP_LEDC_FREQ_RGB, LAMP_LEDC_BITS_RGB, "RGB");
    if (err != ESP_OK) {
        return err;
    }
    err = timer_setup(LAMP_LEDC_TIMER_WHITE, LAMP_LEDC_FREQ_WHITE, LAMP_LEDC_BITS_WHITE, "WHITE");
    if (err != ESP_OK) {
        return err;
    }

    /* ---- 4 个通道 ---- */
    for (int i = 0; i < LAMP_CH_COUNT; ++i) {
        ledc_channel_config_t ccfg = {
            .gpio_num   = lamp_esp32_gpio[i],
            .speed_mode = LAMP_LEDC_SPEED_MODE,
            .channel    = s_ch[i],
            .intr_type  = LEDC_INTR_DISABLE,
            .timer_sel  = s_timer[i],
            .duty       = 0,
            .hpoint     = 0,
        };
        err = ledc_channel_config(&ccfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "LEDC 通道 %d (GPIO%d, ch%d) 配置失败: %s", i,
                     lamp_esp32_gpio[i], (int)s_ch[i], esp_err_to_name(err));
            return err;
        }
    }

    /* ---- 把 core 驱动绑到本层 ---- */
    lamp_platform_t plat;
    plat.write = lamp_esp32_write;
    plat.user = NULL;
    lamp_driver_init(drv, &plat);

    s_ready = true;
    ESP_LOGI(TAG,
             "灯驱动就绪: R=GPIO%d/ch%d/%uHz  G=GPIO%d/ch%d  B=GPIO%d/ch%d  "
             "W=GPIO%d/ch%d/%uHz(%u bit)，RGB %u bit（core 满量程 %u）",
             PIN_LED_RED, LAMP_LEDC_CH_RED, LAMP_LEDC_FREQ_RGB,
             PIN_LED_GREEN, LAMP_LEDC_CH_GREEN,
             PIN_LED_BLUE, LAMP_LEDC_CH_BLUE,
             PIN_LED_WHITE, LAMP_LEDC_CH_WHITE, LAMP_LEDC_FREQ_WHITE,
             (unsigned)LAMP_LEDC_BITS_WHITE, (unsigned)LAMP_LEDC_BITS_RGB,
             (unsigned)LAMP_PWM_MAX_DUTY);
    return ESP_OK;
}

uint16_t lamp_esp32_last_duty(int channel)
{
    if (channel < 0 || channel >= (int)LAMP_CH_COUNT) {
        return 0u;
    }
    return s_last_duty[channel];
}
