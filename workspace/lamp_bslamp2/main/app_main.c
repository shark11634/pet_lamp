/*
 * app_main.c — 灯端主程序（方案 B：纯 ESP-IDF）
 *
 * 本文件是**编排层**，不含业务数学。业务全在 core/：
 *   core/pet_protocol.c   广播解析 / CRC / 字段校验
 *   core/device_registry.c 多狗设备表 / 每狗独立计时 / 最活跃者优先
 *   core/night_mode.c     夜间模式 / 空闲推断 / 手动豁免 / Active 节流
 *   core/lamp_effects.c   状态目标色 / 过渡 / 呼吸 / 提示 / 恢复动画
 *   core/lamp_driver.c    Gamma 2.2 / 最小稳定亮度 / 全通道限流 / 层调制
 *
 * 三档任务节奏（需求 §6.3）：
 *   ble_rx_task   事件驱动  — 出队原始广播 → 协议解析 → 设备表
 *   state_task    1 Hz     — 夜间 tick、合并、失联、NVS（仅变化时写）
 *   fx_task       50 Hz    — 灯效插值 + 输出到 LEDC
 *
 * 为什么三个任务而不是一个大循环：灯效必须 20–100 Hz 稳定更新，而协议解析
 * 与 NVS 写入的耗时不可预测；混在一起会出现"写一次 Flash 灯闪一下"。
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"

#include "app_config.h"
#include "ble_scanner.h"
#include "diagnostics.h"
#include "lamp_esp32.h"
#include "panel_protocol.h"
#include "storage.h"
#include "touch_panel.h"

/* core */
#include "device_registry.h"
#include "lamp_driver.h"
#include "lamp_effects.h"
#include "night_mode.h"
#include "pet_protocol.h"
#include "pet_types.h"

static const char *TAG = "app";

/* ------------------------------------------------------------------ */
/* 全局状态（只有 state_task 与 fx_task 会写，且各写各的字段）          */
/* ------------------------------------------------------------------ */

static pet_registry_t   s_reg;
static pet_night_t      s_night;
static lamp_driver_t    s_driver;
static lamp_fx_state_t  s_fx;
static pet_nvs_settings_t s_settings;

/** 合并后的显示状态；由 state_task 写，fx_task 读 */
static volatile pet_lamp_state_t s_merged = PET_LAMP_BOOT;
static volatile uint8_t s_merged_src = PET_FOCUS_MERGE;
static volatile uint8_t s_merged_score = 0u;

static bool     s_pairing;
static uint32_t s_pairing_until_ms;
static uint32_t s_boot_ms;

/* ------------------------------------------------------------------ */
/* 工具                                                               */
/* ------------------------------------------------------------------ */

/** 单调毫秒时钟（不受系统时间同步影响） */
static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000LL);
}

/**
 * 当前本地小时；时间未同步时返回 false，让 night_mode 走空闲推断兜底。
 * 本参考实现不接 SNTP（那是 ESPHome 线路的事），因此这里一般返回 false ——
 * 这恰好验证了"没有时钟也能夜间免打扰"这条设计目标。
 */
static bool clock_hour(int *hour)
{
    (void)hour;
    return false;
}

/** 需要持久化设置时置位，由 state_task 统一写（避免在收包/灯效路径写 Flash） */
static volatile bool s_settings_dirty;

static void settings_save_later(void)
{
    s_settings_dirty = true;
}

static void persist_settings(void)
{
    if (!s_settings_dirty) {
        return;
    }
    s_settings_dirty = false;
    storage_settings_from_night(&s_settings, &s_night.cfg);
    s_settings.manual_off = 0u;   /* 参考实现暂不接管原厂手动灯光，见 README */
    (void)storage_save_settings(&s_settings);
}

/* ------------------------------------------------------------------ */
/* 灯效输出：core → LEDC                                              */
/* ------------------------------------------------------------------ */

static void fx_output(const lamp_color_t *color, float layer,
                      uint16_t floor_duty, void *user)
{
    (void)user;
    /* core 已经把 Gamma / 限流 / 层调制算完，这里只写硬件 */
    lamp_driver_set_layer(&s_driver, color, layer, floor_duty);
}

/* ------------------------------------------------------------------ */
/* 任务 1：BLE 收包                                                   */
/* ------------------------------------------------------------------ */

static void ble_rx_task(void *arg)
{
    (void)arg;
    ble_raw_adv_t raw;

    for (;;) {
        if (!ble_scanner_wait(&raw, 1000u)) {
            continue;   /* 超时：继续等，不忙转 */
        }

        pet_packet_t pkt;
        pet_rx_stats_t rx_stats;
        memset(&rx_stats, 0, sizeof(rx_stats));

        /* 六层校验的前五层在 core 里（CompanyID 已由 AD 层筛过） */
        const pet_parse_result_t r =
            pet_parse_mfg_data(raw.mfg, raw.mfg_len, &pkt, &rx_stats);
        /* 解析失败也计数：这些包到不了设备表，只能在解析层统计 */
        diagnostics_add_rx_stats(&rx_stats);
        if (r != PET_PARSE_OK) {
            /* 别家的 0xFFFF 广播可能不少，用 DEBUG 而不是 WARN */
            ESP_LOGD(TAG, "丢弃广播: %s", pet_parse_error_name(r));
            continue;
        }
        pkt.rssi = raw.rssi;

        const bool pair_ok = s_pairing;
        const pet_reg_result_t res =
            pet_reg_on_packet(&s_reg, &pkt, raw.rx_time_ms, pair_ok);

        switch (res) {
            case PET_RX_ACCEPTED:
                if (pair_ok && pet_reg_is_bound(&s_reg, pkt.device_id)) {
                    /* 新设备入表：立刻持久化，并暖白闪一下确认（需求 §7.2） */
                    (void)storage_save_bindings(&s_reg);
                    (void)storage_save_names(&s_reg);
                    lamp_effects_trigger_recover(&s_fx, raw.rx_time_ms);
                    ESP_LOGI(TAG, "已绑定 0x%08X（当前 %u 只，容量 %u）",
                             (unsigned)pkt.device_id,
                             (unsigned)pet_reg_count(&s_reg),
                             (unsigned)pet_reg_cap(&s_reg));
                    /* 面板指示灯刷新要在配对任务之外做 I²C，这里安全 */
                    panel_refresh_leds();
                }
                break;
            case PET_RX_ERR_UNBOUND:
                ESP_LOGD(TAG, "未绑定 0x%08X 被忽略", (unsigned)pkt.device_id);
                break;
            case PET_RX_ERR_STALE:
                break;
            case PET_RX_ERR_NO_SLOT:
                ESP_LOGW(TAG, "设备表已满（容量 %u），忽略 0x%08X",
                         (unsigned)pet_reg_cap(&s_reg), (unsigned)pkt.device_id);
                break;
            default:
                break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* 任务 2：状态（1 Hz）                                                */
/* ------------------------------------------------------------------ */

static void state_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(TASK_STATE_INTERVAL_MS));

        const uint32_t now = now_ms();

        /* 配对窗口到期 */
        if (s_pairing && (int32_t)(now - s_pairing_until_ms) >= 0) {
            s_pairing = false;
            ESP_LOGI(TAG, "配对窗口结束，共绑定 %u 只",
                     (unsigned)pet_reg_count(&s_reg));
            diagnostics_report_pairing_end(pet_reg_count(&s_reg));
        }

        /* 夜间模式（时间窗 + 空闲推断 + 豁免超时回落 + Active 节流） */
        int hour = 0;
        const bool have_clock = clock_hour(&hour);
        pet_night_tick(&s_night, now, hour, have_clock);

        /* 各设备状态驻留时长 */
        pet_reg_tick(&s_reg, now);

        /* 合并：最活跃者优先（决策 D7）；全部超时才算 Lost */
        uint8_t src = PET_FOCUS_MERGE;
        const pet_lamp_state_t st =
            pet_reg_merge(&s_reg, now, s_settings.lost_timeout_ms, &src);

        if (st != s_merged) {
            ESP_LOGI(TAG, "合并状态 %s -> %s", lamp_state_name(s_merged),
                     lamp_state_name(st));
            if (st == PET_LAMP_LOST) {
                /* Lost 是"状态暂不可用"，不是报警（需求 §4.4 避免事项） */
                ESP_LOGW(TAG, "全部设备失联（超时 %u ms）",
                         (unsigned)s_settings.lost_timeout_ms);
            }
        }
        s_merged = st;
        s_merged_src = src;
        s_merged_score = pet_reg_merged_score(&s_reg, src);

        /* 设置变更统一在这里落盘（不在收包/灯效路径写 Flash） */
        persist_settings();

        /* 诊断日志：默认 30 秒一次（需求 §8.5 的 10–60 秒） */
        diagnostics_maybe_report(now, &s_reg, &s_night, s_settings.lost_timeout_ms);
    }
}

/* ------------------------------------------------------------------ */
/* 任务 3：灯效（50 Hz）                                              */
/* ------------------------------------------------------------------ */

static void fx_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(TASK_FX_INTERVAL_MS));

        const uint32_t now = now_ms();
        const pet_lamp_state_t st = s_merged;

        /* 宠物状态显著变化时重置空闲推断，但**不**进入手动模式
         * （否则狗一动就永久豁免，夜里会一直亮） */
        if (st != PET_LAMP_LOST) {
            pet_night_on_state_change(&s_night, now);
        }

        /* 夜间 Active 节流：狗翻身不会让灯反复亮灭 */
        bool render = true;
        if (st == PET_LAMP_ACTIVE && !s_night.manual_mode) {
            render = pet_night_allow_active_render(&s_night, now);
        }
        if (!render) {
            continue;   /* 保持上一帧输出 */
        }

        const uint64_t age = pet_reg_lost_age_ms(&s_reg, now);

        lamp_fx_input_t in;
        memset(&in, 0, sizeof(in));
        in.state = st;
        in.activity_score = s_merged_score;
        in.brightness_limit_pct = 100u;   /* 面板滑条接入后由触摸事件改 */
        in.brightness_cap = pet_night_cap_for(&s_night, st);
        in.manual_off = false;            /* 参考实现暂不接管原厂手动灯光 */
        in.manual_mode = false;
        in.low_battery =
            pet_reg_any_low_battery(&s_reg, now, s_settings.lost_timeout_ms);
        in.lowbat_hint_allowed = !pet_night_suppress_lowbat(&s_night);
        in.lost_age_ms = (age == UINT64_MAX) ? 0u : age;
        in.night_active = pet_night_is_night(&s_night);
        in.manual = NULL;

        lamp_effects_tick(&s_fx, &in, now);
    }
}

/* ------------------------------------------------------------------ */
/* 面板指示灯：12 颗用于显示"当前焦点是第几只狗"（决策 D15 / 需求 §4.2） */
/* ------------------------------------------------------------------ */

/**
 * 面板有 12 颗灯：电源灯（bit14）、色彩灯（bit12）、滑条 10 颗（bit9..0）。
 *
 * 用途分配（不改动硬件含义，只是复用它显示宠物状态）：
 *   · 电源灯：开 = 手动模式生效中（已豁免夜间压制）；闪烁不便实现，用常亮表示
 *   · 色彩灯：开 = 当前处于"只看某只狗"而非合并显示
 *   · 滑条灯：前 N 颗亮 = 当前焦点狗序号（N = index+1）；
 *             合并显示时按"在册狗数量"点亮，便于一眼看出有几只在活动
 */
static void panel_refresh_leds(void)
{
    const uint8_t count = pet_reg_count(&s_reg);
    if (count == 0u) {
        /* 没有绑定任何设备：只留电源灯表示"我在跑" */
        (void)touch_panel_set_leds(PANEL_LED_POWER);
        return;
    }

    uint16_t leds = PANEL_LED_POWER;

    const uint8_t focus = pet_reg_focus(&s_reg);
    if (focus == PET_FOCUS_MERGE) {
        /* 合并显示：按在册狗数点亮滑条灯（最多 10 颗） */
        uint8_t n = count;
        if (n > 10u) {
            n = 10u;
        }
        leds |= touch_panel_slider_leds_for_ratio((float)n / 10.0f);
    } else {
        /* 单只：点亮色彩灯（表示非合并）并点亮 focus+1 颗滑条灯 */
        leds |= PANEL_LED_COLOR;
        uint8_t n = (uint8_t)(focus + 1u);
        if (n > 10u) {
            n = 10u;
        }
        leds |= touch_panel_slider_leds_for_ratio((float)n / 10.0f);
    }

    /* 手动豁免生效中：额外点亮电源灯（这里已含，语义在日志里说明） */
    if (s_night.manual_mode) {
        leds |= PANEL_LED_POWER;
    }

    (void)touch_panel_set_leds(leds);
}

/* ------------------------------------------------------------------ */
/* 触摸面板事件（需求 §7.3：原厂用户可见功能等价）                      */
/* ------------------------------------------------------------------ */

static void on_panel_event(const touch_panel_event_t *ev)
{
    if (ev == NULL) {
        return;
    }
    const uint32_t now = now_ms();

    switch (ev->type) {
        case TOUCH_EVENT_POWER_SHORT:
            /* 短按：临时熄灯 / 恢复自动显示（需求 §7.3）
             * 实现为"切换手动模式"：手动模式期间暂停自动灯效与夜间压制，
             * 无操作满豁免时长后自动回落（决策 D10）。 */
            if (s_night.manual_mode) {
                pet_night_exit_manual(&s_night, now);
                ESP_LOGI(TAG, "面板：短按 -> 恢复自动显示");
            } else {
                pet_night_on_user_action(&s_night, now);
                ESP_LOGI(TAG, "面板：短按 -> 进入手动模式（%u 分钟后自动恢复）",
                         (unsigned)(s_night.cfg.override_ms / 60000u));
            }
            panel_refresh_leds();
            break;

        case TOUCH_EVENT_POWER_LONG:
            /* 长按 5 秒：进入配对模式，60 秒窗口（需求 §7.2） */
            ESP_LOGI(TAG, "面板：长按电源键 -> 配对模式");
            pet_reg_clear(&s_reg);
            (void)storage_clear_bindings();
            s_pairing = true;
            s_pairing_until_ms = now + PAIR_MODE_DURATION_MS;
            diagnostics_report_pairing_start(PAIR_MODE_DURATION_MS);
            panel_refresh_leds();
            break;

        case TOUCH_EVENT_COLOR_SHORT:
            /* 多狗时：切换"合并 / 只看某只"（决策 D15）
             * 单狗时：保持原厂色相循环行为（本参考实现不干预色相，
             *         留给 app_main 之外的灯光层，此处不动作） */
            if (pet_reg_count(&s_reg) > 1u) {
                const uint8_t cur = pet_reg_focus(&s_reg);
                uint8_t next;
                if (cur == PET_FOCUS_MERGE) {
                    next = 0u;
                } else if ((uint8_t)(cur + 1u) >= pet_reg_count(&s_reg)) {
                    next = PET_FOCUS_MERGE;
                } else {
                    next = (uint8_t)(cur + 1u);
                }
                (void)pet_reg_set_focus(&s_reg, next);
                (void)storage_save_bindings(&s_reg);
                ESP_LOGI(TAG, "面板：切换显示焦点 -> %s",
                         (next == PET_FOCUS_MERGE) ? "合并" : "单只");
                panel_refresh_leds();
            } else {
                ESP_LOGD(TAG, "面板：色彩键短按（单狗，色相行为交由灯光层）");
            }
            break;

        case TOUCH_EVENT_COLOR_LONG:
            /* 多狗时的色相切换入口（决策 D15：单狗时色相仍在短按） */
            ESP_LOGI(TAG, "面板：色彩键长按");
            break;

        case TOUCH_EVENT_BRIGHTNESS:
            /* 滑条只改全局亮度上限，不改变状态颜色规则（需求 §7.3）
             * 同时算作一次用户操作，刷新手动豁免计时 */
            ESP_LOGI(TAG, "面板：亮度滑条 -> %u%%", (unsigned)ev->value);
            pet_night_on_user_action(&s_night, now);
            settings_save_later();
            break;

        default:
            break;
    }
}

/* ------------------------------------------------------------------ */
/* 初始化                                                             */
/* ------------------------------------------------------------------ */

static void load_state(void)
{
    /* 1) NVS：设置先载入，因为夜间模式初始化要用它 */
    if (storage_init() != ESP_OK) {
        ESP_LOGW(TAG, "NVS 不可用，全部设置走默认值（断电会丢）");
    }
    if (storage_load_settings(&s_settings) != ESP_OK) {
        storage_settings_defaults(&s_settings);
    }

    /* 2) 设备表。容量最终由 NVS 决定（决策 D6：默认 8，可配 1–16），
     *    这里先用默认值建表，storage_load_bindings() 会按记录调整容量。
     *    注意：pet_reg_set_cap() 缩小容量不会踢掉已有设备，所以这个顺序安全。 */
    pet_reg_init(&s_reg, PET_DEV_MAX_DEFAULT);
    (void)storage_load_bindings(&s_reg);
    (void)storage_load_names(&s_reg);

    /* 3) 夜间模式：先用默认值构造，再套用已载入的设置 */
    pet_night_config_t cfg;
    pet_night_config_defaults(&cfg);
    cfg.enabled = s_settings.night_enabled != 0u;
    cfg.start_hour = s_settings.night_start;
    cfg.end_hour = s_settings.night_end;
    cfg.cap_quiet = s_settings.cap_quiet;
    cfg.cap_light = s_settings.cap_light;
    cfg.cap_active = s_settings.cap_active;
    cfg.cap_lost = s_settings.cap_lost;
    cfg.suppress_lowbat_hint = s_settings.suppress_lowbat != 0u;
    cfg.override_ms = s_settings.override_ms;
    cfg.idle_infer_ms = s_settings.night_idle_ms;
    cfg.active_throttle_ms = s_settings.night_throttle_ms;
    pet_night_init(&s_night, &cfg, now_ms());

    /* 4) 灯效引擎绑定输出回调（core → LEDC） */
    lamp_effects_init(&s_fx);
    lamp_effects_set_output(&s_fx, fx_output, NULL);
}

void app_main(void)
{
    s_boot_ms = now_ms();
    ESP_LOGI(TAG, "=== %s 启动（boot %u ms）===", FIRMWARE_VERSION,
             (unsigned)s_boot_ms);

    /* 灯驱动最先行：主开关与 LEDC 先就绪，后续任何日志都不影响上电状态 */
    esp_err_t err = lamp_esp32_init(&s_driver);
    if (err != ESP_OK) {
        /* 灯驱动失败仍然继续跑：这样串口还能看到诊断，
         * 而不是一块"什么都不说的砖"（需求 §8.5 可观测性） */
        ESP_LOGE(TAG, "灯驱动初始化失败: %s（继续运行以便诊断）",
                 esp_err_to_name(err));
    }

    load_state();

    /* 上电自检灯效：让用户看见固件活着（暖白恢复动画） */
    lamp_effects_trigger_recover(&s_fx, now_ms());

    if (touch_panel_init(on_panel_event) != ESP_OK) {
        ESP_LOGW(TAG, "触摸面板未就绪：原厂面板功能不可用（核心灯效不受影响）");
    } else {
        panel_refresh_leds();
    }

    if (ble_scanner_start() != ESP_OK) {
        ESP_LOGE(TAG, "BLE 扫描启动失败：宠物状态功能不可用");
    }

    diagnostics_init(&s_reg, &s_night, &s_settings);

    xTaskCreate(ble_rx_task, "ble_rx", TASK_STACK_BLE, NULL, TASK_PRIO_BLE, NULL);
    xTaskCreate(state_task, "state", TASK_STACK_STATE, NULL, TASK_PRIO_STATE, NULL);
    xTaskCreate(fx_task, "fx", TASK_STACK_FX, NULL, TASK_PRIO_FX, NULL);

    ESP_LOGI(TAG, "就绪：容量 %u，失联超时 %u ms，豁免 %u ms，夜间 %s %02u:00-%02u:00",
             (unsigned)pet_reg_cap(&s_reg), (unsigned)s_settings.lost_timeout_ms,
             (unsigned)s_settings.override_ms,
             s_night.cfg.enabled ? "开" : "关",
             (unsigned)s_night.cfg.start_hour, (unsigned)s_night.cfg.end_hour);
}
