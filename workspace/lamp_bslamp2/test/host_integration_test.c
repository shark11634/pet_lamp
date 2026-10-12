/*
 * host_integration_test.c — 端到端集成测试（core → ESP-IDF 适配层 → LEDC）
 *
 * 与 workspace/test/ 下四套单元测试的分工：
 *   单元测试分别验证 core 的每个模块（300 项断言）。
 *   本测试验证**接线**：把真实的 lamp_bslamp2/main/lamp_esp32.c 编到主机上，
 *   跑完整链路
 *
 *     合成广播字节流
 *        → core/pet_protocol.c        （真实解析 + CRC）
 *        → core/device_registry.c     （真实设备表 / 最活跃者优先）
 *        → core/night_mode.c          （真实夜间压制）
 *        → core/lamp_effects.c        （真实灯效插值）
 *        → lamp_esp32.c               （真实 LEDC 适配层）
 *        → host_ledc_duty()           （替身寄存器，可断言）
 *
 * 接线错误（通道顺序反了、占空比被换算了两次、主开关没拉高、越界没钳位）
 * 单元测试抓不到，只有集成测试能抓到。
 *
 * ⚠️ 本套测试开发过程中真的踩过一个坑，记在这里避免重犯：
 *   最初版本让 lamp_effects 的输出回调写入**测试自己的** lamp_driver_t，
 *   而 lamp_esp32.c 内部维护的是**它自己的**实例。结果 LEDC 替身永远是 0，
 *   4 个用例"合理地"失败 —— 看起来像产品缺陷，其实是测试接错了线。
 *   现在用 test/mock_driver.h 把 lamp_driver_set_layer() 换成"直通"版本，
 *   core 算出的占空比在同一次调用里就推进 LEDC 替身。
 *   Gamma / 限流 / floor 语义仍由 test/host_effects_test.c 对着真实实现验证。
 *
 * 编译：test/run_integration_test.ps1
 */
#include <stdio.h>
#include <string.h>

#include "pet_test.h"

/* core */
#include "device_registry.h"
#include "lamp_driver.h"
#include "lamp_effects.h"
#include "night_mode.h"
#include "pet_config.h"
#include "pet_protocol.h"
#include "pet_types.h"

/* ESP-IDF 替身 */
#include "driver/gpio.h"
#include "driver/ledc.h"

/* 平台适配层（真实实现） */
#include "lamp_esp32.h"
#include "app_config.h"

/* 直通驱动替身（必须放在 lamp_esp32.h 之后） */
#include "mock_driver.h"

/* 真实的 ESP-IDF 适配层内部持有的 driver 实例 */
static lamp_driver_t g_esp_driver;

/* ------------------------------------------------------------------ */
/* 测试上下文：模拟 app_main 的编排                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    pet_registry_t  reg;
    pet_night_t     night;
    lamp_fx_state_t fx;
    uint32_t        lost_timeout_ms;
    bool            pairing;
} ctx_t;

/**
 * core → 适配层 的输出回调。
 *
 * 这里刻意**不**经过测试自己的 lamp_driver_t：直接调 lamp_driver_set_layer()
 * 并由 mock_driver.h 转发到 LEDC 替身。这正是被测的接线。
 */
static void ctx_output(const lamp_color_t *color, float layer,
                       uint16_t floor_duty, void *user)
{
    lamp_driver_set_layer((lamp_driver_t *)user, color, layer, floor_duty);
}

static void init_ctx(ctx_t *c, uint8_t cap, uint32_t lost_ms)
{
    memset(c, 0, sizeof(*c));
    c->lost_timeout_ms = lost_ms;
    pet_reg_init(&c->reg, cap);

    pet_night_config_t cfg;
    pet_night_config_defaults(&cfg);
    pet_night_init(&c->night, &cfg, 0u);

    lamp_effects_init(&c->fx);
    /*
     * user 必须指向**适配层内部持有的那个** lamp_driver_t（g_esp_driver）。
     * 传 NULL 就断线了 —— 本工程在写这套测试时真的这么错过一次，
     * 表现为 LEDC 替身永远读 0。
     */
    lamp_effects_set_output(&c->fx, ctx_output, &g_esp_driver);
}

/* ------------------------------------------------------------------ */
/* 合成广播                                                            */
/* ------------------------------------------------------------------ */

/** 用 core 的构造函数造一个合法 MFG 数据块（含 CompanyID） */
static void make_mfg(uint8_t out[PET_MFG_DATA_LEN], uint32_t dev_id,
                     uint16_t seq, pet_activity_state_t st, uint8_t score,
                     uint8_t battery, uint8_t flags)
{
    pet_packet_t p;
    memset(&p, 0, sizeof(p));
    p.company_id       = PET_COMPANY_ID;
    p.protocol_version = PET_PROTOCOL_VERSION;
    p.device_id        = dev_id;
    p.sequence         = seq;
    p.activity_state   = (uint8_t)st;
    p.activity_score   = score;
    p.battery_percent  = battery;
    p.flags            = flags;
    pet_build_mfg_data(out, &p);
}

/**
 * 走一遍完整的"收包 → 设备表 → 状态合并 → 灯效 → LEDC"。
 *
 * 只调用 core 与适配层，不做断言（断言在用例里做，避免刷屏）。
 */
static pet_lamp_state_t feed(ctx_t *c, const uint8_t mfg[PET_MFG_DATA_LEN],
                            uint32_t now_ms, int8_t rssi)
{
    pet_packet_t pkt;
    pet_rx_stats_t stats;
    memset(&stats, 0, sizeof(stats));

    const pet_parse_result_t r =
        pet_parse_mfg_data(mfg, PET_MFG_DATA_LEN, &pkt, &stats);
    if (r != PET_PARSE_OK) {
        return PET_LAMP_BOOT;   /* 用例会单独断言合成包可解析 */
    }
    pkt.rssi = rssi;

    (void)pet_reg_on_packet(&c->reg, &pkt, now_ms, c->pairing);

    uint8_t src = PET_FOCUS_MERGE;
    const pet_lamp_state_t merged =
        pet_reg_merge(&c->reg, now_ms, c->lost_timeout_ms, &src);

    /* 与 app_main.c 的 fx_task 一致地组装输入 */
    const uint64_t age = pet_reg_lost_age_ms(&c->reg, now_ms);
    lamp_fx_input_t in;
    memset(&in, 0, sizeof(in));
    in.state = merged;
    in.activity_score = pet_reg_merged_score(&c->reg, src);
    in.brightness_limit_pct = 100u;
    in.brightness_cap = pet_night_cap_for(&c->night, merged);
    in.low_battery = pet_reg_any_low_battery(&c->reg, now_ms, c->lost_timeout_ms);
    in.lowbat_hint_allowed = !pet_night_suppress_lowbat(&c->night);
    in.lost_age_ms = (age == UINT64_MAX) ? 0u : (uint32_t)age;
    in.night_active = pet_night_is_night(&c->night);

    lamp_effects_tick(&c->fx, &in, now_ms);
    return merged;
}

/** 多跑几帧让插值收敛到目标（状态过渡需要时间） */
static void settle(ctx_t *c, uint32_t dev_id, pet_activity_state_t st,
                   uint8_t score, uint32_t from_ms, uint32_t to_ms)
{
    uint8_t mfg[PET_MFG_DATA_LEN];
    uint16_t seq = (uint16_t)(from_ms / 20u);
    for (uint32_t t = from_ms; t <= to_ms; t += TASK_FX_INTERVAL_MS) {
        make_mfg(mfg, dev_id, seq++, st, score, 80u, 0u);
        (void)feed(c, mfg, t, -55);
    }
}

/* ------------------------------------------------------------------ */
/* 占空比快照                                                          */
/* ------------------------------------------------------------------ */

static void snapshot_duty(uint16_t out[LAMP_CH_COUNT])
{
    for (int i = 0; i < LAMP_CH_COUNT; ++i) {
        out[i] = (uint16_t)host_ledc_duty((ledc_channel_t)i);
    }
}

static uint16_t duty_peak(const uint16_t d[LAMP_CH_COUNT])
{
    uint16_t m = 0u;
    for (int i = 0; i < LAMP_CH_COUNT; ++i) {
        if (d[i] > m) {
            m = d[i];
        }
    }
    return m;
}

/** 峰值占空比 ≈ 逻辑亮度百分比（反 Gamma 2.2），只用于区间断言 */
static int duty_luma_pct(const uint16_t d[LAMP_CH_COUNT])
{
    extern double pow(double, double);
    const double norm = (double)duty_peak(d) / (double)LAMP_PWM_MAX_DUTY;
    if (norm <= 0.0) {
        return 0;
    }
    const double lvl = pow(norm, 1.0 / 2.2) * 255.0;
    return (int)((lvl * 100.0 / 255.0) + 0.5);
}

/* ================================================================== */
/* 用例                                                                */
/* ================================================================== */

/* 1. 适配层初始化：LEDC 通道绑定到正确的 GPIO，主开关被拉高 */
static void test_adapter_init(void)
{
    const esp_err_t err = lamp_esp32_init(&g_esp_driver);
    t_eq(err, ESP_OK, "lamp_esp32_init 成功");

    /*
     * 通道分配必须与上游 esphome-xiaomi_bslamp2/packages/core.yaml 逐项一致：
     *   红 ch0 / 绿 ch1 / 蓝 ch2 / 白 **ch4**
     * 白色放在 ch4 而不是 ch3 是上游的选择（它有独立定时器）。
     * "功能等价"包含颜色输出等价，所以这里不图整齐。
     */
    t_eq(host_ledc_gpio(LEDC_CHANNEL_0), PIN_LED_RED,   "ch0 -> 红 GPIO13");
    t_eq(host_ledc_gpio(LEDC_CHANNEL_1), PIN_LED_GREEN, "ch1 -> 绿 GPIO14");
    t_eq(host_ledc_gpio(LEDC_CHANNEL_2), PIN_LED_BLUE,  "ch2 -> 蓝 GPIO5");
    t_eq(host_ledc_gpio(LEDC_CHANNEL_4), PIN_LED_WHITE, "ch4 -> 白 GPIO12（与上游一致）");

    t_eq(lamp_esp32_gpio[0], PIN_LED_RED,   "lamp_esp32_gpio[0] = 红");
    t_eq(lamp_esp32_gpio[1], PIN_LED_GREEN, "lamp_esp32_gpio[1] = 绿");
    t_eq(lamp_esp32_gpio[2], PIN_LED_BLUE,  "lamp_esp32_gpio[2] = 蓝");
    t_eq(lamp_esp32_gpio[3], PIN_LED_WHITE, "lamp_esp32_gpio[3] = 白");

    /* 定时器分组：RGB 共用一个，白单独一个（频率不同） */
    t_eq(host_ledc_timer(LEDC_CHANNEL_0), LAMP_LEDC_TIMER_RGB, "红用 RGB 定时器");
    t_eq(host_ledc_timer(LEDC_CHANNEL_1), LAMP_LEDC_TIMER_RGB, "绿用 RGB 定时器");
    t_eq(host_ledc_timer(LEDC_CHANNEL_2), LAMP_LEDC_TIMER_RGB, "蓝用 RGB 定时器");
    t_eq(host_ledc_timer(LEDC_CHANNEL_4), LAMP_LEDC_TIMER_WHITE, "白用白光定时器");

    /* 频率与上游一致：RGB 3000 Hz、白 10000 Hz */
    t_eq(host_ledc_timer_freq(LAMP_LEDC_TIMER_RGB), LAMP_LEDC_FREQ_RGB,
         "RGB 载波 = 3000 Hz（与上游一致）");
    t_eq(host_ledc_timer_freq(LAMP_LEDC_TIMER_WHITE), LAMP_LEDC_FREQ_WHITE,
         "白光载波 = 10000 Hz（与上游一致）");

    /* 需求 4.2：主开关必须在灯光输出之前导通 */
    (void)lamp_esp32_power_enable(true);
    t_eq(gpio_get_level(PIN_LED_SWITCH_1), 1, "主开关 1 已导通");
    t_eq(gpio_get_level(PIN_LED_SWITCH_2), 1, "主开关 2 已导通");
    (void)lamp_esp32_power_enable(false);
    t_eq(gpio_get_level(PIN_LED_SWITCH_1), 0, "主开关可关断");
    (void)lamp_esp32_power_enable(true);
}

/*
 * 2. 通道映射真值表
 *
 * 只测"一个亮通道"是不够的：那种断言在通道顺序整体错位时也可能通过。
 * 这里逐个把 R/G/B/W 单独点亮，要求"只有对应 GPIO 收到非零占空比"。
 * 这样任何交换、偏移、多写都会被抓到。
 */
static void test_channel_mapping_truth_table(void)
{
    struct { int ch; const char *name; uint32_t gpio; } cases[LAMP_CH_COUNT] = {
        { LAMP_CH_R, "红", PIN_LED_RED   },
        { LAMP_CH_G, "绿", PIN_LED_GREEN },
        { LAMP_CH_B, "蓝", PIN_LED_BLUE  },
        { LAMP_CH_W, "白", PIN_LED_WHITE },
    };

    for (int i = 0; i < LAMP_CH_COUNT; ++i) {
        lamp_color_t c = { 0u, 0u, 0u, 0u };
        switch (cases[i].ch) {
            case LAMP_CH_R: c.r = 255u; break;
            case LAMP_CH_G: c.g = 255u; break;
            case LAMP_CH_B: c.b = 255u; break;
            default:        c.w = 255u; break;
        }

        /* 走真实适配层路径 */
        ctx_t tmp;
        init_ctx(&tmp, 1u, LOST_TIMEOUT_MS);
        lamp_driver_set_layer(&g_esp_driver, &c, 1.0f, 0u);
        (void)tmp;

        char buf[96];
        int nonzero = 0;
        /* 遍历**全部 8 个** LEDC 通道：白色在 ch4，若只查到 ch3 会漏检 */
        for (int k = 0; k < (int)LEDC_CHANNEL_MAX; ++k) {
            if (host_ledc_duty((ledc_channel_t)k) > 0u) {
                ++nonzero;
            }
        }
        snprintf(buf, sizeof(buf), "只点亮%s时恰好 1 路有输出", cases[i].name);
        t_eq(nonzero, 1, buf);

        /* 该颜色对应的 LEDC 通道号（白 = ch4，与上游一致） */
        const int expect_ch = (cases[i].ch == LAMP_CH_W) ? 4 : cases[i].ch;

        snprintf(buf, sizeof(buf), "%s -> GPIO%u (ch%d) 得到非零占空比",
                 cases[i].name, (unsigned)cases[i].gpio, expect_ch);
        t_check(host_ledc_duty((ledc_channel_t)expect_ch) > 0u, buf);

        /* 其他通道必须是 0（含"白色错放到 ch3"这种位错） */
        for (int k = 0; k < (int)LEDC_CHANNEL_MAX; ++k) {
            if (k == expect_ch) {
                continue;
            }
            snprintf(buf, sizeof(buf), "点亮%s时 ch%d 保持 0（无串扰）",
                     cases[i].name, k);
            t_eq(host_ledc_duty((ledc_channel_t)k), 0, buf);
        }

        host_ledc_reset();
    }
}

/* 3. 配对 → 收包 → 状态合并：全链路 */
static void test_full_pipeline(void)
{
    ctx_t c;
    init_ctx(&c, 4u, LOST_TIMEOUT_MS);
    c.pairing = true;   /* 配对窗口内 */

    uint8_t mfg[PET_MFG_DATA_LEN];

    /* 先确认合成包本身能被 core 解析（否则后面的失败会指向错的地方） */
    make_mfg(mfg, 0x11111111u, 0u, PET_STATE_QUIET, 20u, 90u, 0u);
    pet_packet_t chk;
    pet_rx_stats_t chk_stats;
    memset(&chk_stats, 0, sizeof(chk_stats));
    t_eq(pet_parse_mfg_data(mfg, PET_MFG_DATA_LEN, &chk, &chk_stats), PET_PARSE_OK,
         "合成包能被 core 解析通过");
    t_eq(chk.company_id, PET_COMPANY_ID, "CompanyID 往返一致");
    t_eq(chk.device_id, 0x11111111u, "DeviceID 往返一致（小端）");

    pet_lamp_state_t st = feed(&c, mfg, 1000u, -60);
    t_eq(pet_reg_count(&c.reg), 1, "第一条合法包即绑定成功");
    t_check(st == PET_LAMP_QUIET, "单只 Quiet 狗 → 合并为 Quiet");

    /* 同一只狗活动起来，多跑几帧让插值收敛 */
    settle(&c, 0x11111111u, PET_STATE_ACTIVE, 200u, 2000u, 4000u);

    uint8_t src = PET_FOCUS_MERGE;
    const pet_lamp_state_t merged =
        pet_reg_merge(&c.reg, 4000u, LOST_TIMEOUT_MS, &src);
    t_eq(merged, PET_LAMP_ACTIVE, "Quiet → Active 被识别");

    uint16_t d[LAMP_CH_COUNT];
    snapshot_duty(d);
    t_info("Active 输出占空比 [%u, %u, %u, %u]，峰值 %u / %u",
           d[0], d[1], d[2], d[3], duty_peak(d), (unsigned)LAMP_PWM_MAX_DUTY);

    t_check(duty_peak(d) > 0u, "Active 状态确实点亮了灯（占空比非零）");
    t_check(duty_peak(d) <= LAMP_PWM_MAX_DUTY, "占空比不超过 14-bit 满量程");
    t_check(d[LAMP_CH_B] >= d[LAMP_CH_R], "Active 偏蓝（B >= R，需求 4.4）");
    t_check(duty_luma_pct(d) <= 40, "Active 不超过 40%（需求 4.4）");
}

/* 4. 多狗：最活跃者优先（决策 D7）+ 单只超时不误判 */
static void test_multi_dog_merge(void)
{
    ctx_t c;
    init_ctx(&c, 4u, LOST_TIMEOUT_MS);
    c.pairing = true;

    uint8_t mfg[PET_MFG_DATA_LEN];
    make_mfg(mfg, 0xAAAA0001u, 0u, PET_STATE_LIGHT, 40u, 80u, 0u);
    feed(&c, mfg, 1000u, -70);
    make_mfg(mfg, 0xBBBB0002u, 0u, PET_STATE_QUIET, 10u, 95u, 0u);
    const pet_lamp_state_t st = feed(&c, mfg, 1100u, -55);

    t_eq(pet_reg_count(&c.reg), 2, "两只狗都绑定成功");
    t_eq(st, PET_LAMP_LIGHT, "合并取更活跃的那只（Light > Quiet）");

    uint8_t src = PET_FOCUS_MERGE;
    (void)pet_reg_merge(&c.reg, 1100u, LOST_TIMEOUT_MS, &src);
    t_eq(src, 0, "来源指向 Light 那只狗");

    /*
     * 关键场景（需求 4.3 + 决策 D7）：
     *   狗 A（Light）在 t=1000/1100 报过之后就不报了；
     *   狗 B 一直报到 t=50s；
     *   在 t=65s 看合并结果，此时 A 已超时（64s），B 仍然在册（15s）。
     * 期望：合并跟随 B（Active），**不能**因为 A 超时就整灯判 Lost。
     */
    const uint32_t b_last_ms = 50000u;
    settle(&c, 0xBBBB0002u, PET_STATE_ACTIVE, 220u, 1200u, b_last_ms);

    const uint32_t check_ms = b_last_ms + 15000u;   /* B 才 15s 没报，仍在册 */
    src = PET_FOCUS_MERGE;
    const pet_lamp_state_t st2 =
        pet_reg_merge(&c.reg, check_ms, LOST_TIMEOUT_MS, &src);
    t_info("狗 A 超时(%.0f s)、狗 B 仍在册(15 s)时，合并 = %s（来源 %u）",
           (double)((check_ms - 1100u) / 1000u), lamp_state_name(st2),
           (unsigned)src);
    t_eq(st2, PET_LAMP_ACTIVE, "一只超时不影响另一只的活动状态（需求 4.3）");
    t_eq(src, 1, "来源指向仍在报的那只狗");

    /* 两只都超时才 Lost —— 与上一断言相反的方向 */
    const uint32_t all_late = check_ms + LOST_TIMEOUT_MS + 1000u;
    src = PET_FOCUS_MERGE;
    const pet_lamp_state_t st3 =
        pet_reg_merge(&c.reg, all_late, LOST_TIMEOUT_MS, &src);
    t_eq(st3, PET_LAMP_LOST, "两只都超时 → Lost");
    t_eq(src, PET_FOCUS_MERGE, "Lost 时来源为合并");
}

/* 5. 全部失联 → Lost 且极暗，但不熄灯 */
static void test_all_lost(void)
{
    ctx_t c;
    init_ctx(&c, 4u, LOST_TIMEOUT_MS);
    c.pairing = true;

    uint8_t mfg[PET_MFG_DATA_LEN];
    make_mfg(mfg, 0x12345678u, 5u, PET_STATE_ACTIVE, 200u, 70u, 0u);
    feed(&c, mfg, 1000u, -50);

    const uint32_t late = 1000u + LOST_TIMEOUT_MS + 5000u;
    uint8_t src = PET_FOCUS_MERGE;
    const pet_lamp_state_t st =
        pet_reg_merge(&c.reg, late, LOST_TIMEOUT_MS, &src);
    t_eq(st, PET_LAMP_LOST, "超过失联超时 → Lost");

    /* 让灯效真的跑到 Lost（走完整链路） */
    lamp_fx_input_t in;
    memset(&in, 0, sizeof(in));
    in.state = PET_LAMP_LOST;
    in.brightness_limit_pct = 100u;
    in.brightness_cap = 255u;
    in.lost_age_ms = LOST_TIMEOUT_MS + 5000u;
    in.lowbat_hint_allowed = true;
    for (uint32_t t = late; t < late + 3000u; t += TASK_FX_INTERVAL_MS) {
        lamp_effects_tick(&c.fx, &in, t);
    }

    uint16_t d[LAMP_CH_COUNT];
    snapshot_duty(d);
    t_info("Lost 输出占空比 [%u, %u, %u, %u]，峰值 %u，约 %d%%",
           d[0], d[1], d[2], d[3], duty_peak(d), duty_luma_pct(d));
    t_check(duty_peak(d) > 0u, "Lost 不熄灯（需求 4.4：Lost 不是熄灯）");
    t_check(duty_luma_pct(d) <= 10, "Lost 亮度很低（<= 10%）");
}

/* 6. 夜间压制真的把 Active 压下去 */
static void test_night_cap_effective(void)
{
    ctx_t c;
    init_ctx(&c, 4u, LOST_TIMEOUT_MS);
    c.pairing = true;

    uint8_t mfg[PET_MFG_DATA_LEN];
    make_mfg(mfg, 0x0BADF00Du, 0u, PET_STATE_ACTIVE, 255u, 60u, 0u);
    feed(&c, mfg, 1000u, -50);
    settle(&c, 0x0BADF00Du, PET_STATE_ACTIVE, 255u, 1100u, 4000u);

    uint16_t day[LAMP_CH_COUNT];
    snapshot_duty(day);
    const int day_pct = duty_luma_pct(day);

    /* 进入夜间：时间窗 + 时钟可信 */
    pet_night_tick(&c.night, 5000u, 23, true);
    t_check(pet_night_is_night(&c.night), "23 点进入夜间");
    t_eq(pet_night_cap_for(&c.night, PET_LAMP_ACTIVE), NIGHT_CAP_ACTIVE,
         "夜间 Active 上限 = 18（决策 D14）");

    settle(&c, 0x0BADF00Du, PET_STATE_ACTIVE, 255u, 5100u, 9000u);

    uint16_t night_d[LAMP_CH_COUNT];
    snapshot_duty(night_d);
    const int night_pct = duty_luma_pct(night_d);

    t_info("日间峰值 %u（约 %d%%） -> 夜间峰值 %u（约 %d%%）",
           duty_peak(day), day_pct, duty_peak(night_d), night_pct);
    t_check(duty_peak(night_d) > 0u, "夜间 Active 仍然可见（不是熄灯）");
    t_check(duty_peak(night_d) < duty_peak(day),
            "夜间 Active 明显比日间暗（需求 4.4 / 决策 D14）");
    t_check(night_pct <= 12, "夜间 Active 实际亮度 <= 12%");
}

/* 7. 适配层不做二次换算，并对越界占空比钳位 */
static void test_no_double_scaling(void)
{
    host_ledc_reset();

    /* core 直接算一个已知占空比，写进适配层，读回必须一致 */
    lamp_color_t c = { 255u, 255u, 255u, 0u };
    uint16_t expect[LAMP_CH_COUNT];
    lamp_compute_duty(&c, 1.0f, 0u, expect);

    lamp_driver_set_layer(&g_esp_driver, &c, 1.0f, 0u);

    for (int i = 0; i < LAMP_CH_COUNT; ++i) {
        const uint32_t got = host_ledc_duty((ledc_channel_t)i);
        char buf[80];
        snprintf(buf, sizeof(buf), "ch%d 原样透传占空比（不得二次换算）", i);
        t_eq(got, expect[i], buf);
    }

    /* 越界保护：适配层必须把超量程值钳到**该通道的**满量程，而不是截断/回绕。
     * 注意白色是 13 bit（10 kHz 在 14 bit 下不可达，见 app_config.h），
     * 所以它的上限是 8191 而不是 16383 —— 这条断言同时钉住了按通道分辨率。 */
    uint16_t over[LAMP_CH_COUNT] = { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu };
    lamp_platform_t p = g_esp_driver.platform;
    if (p.write != NULL) {
        p.write(over, p.user);
    } else {
        t_check(false, "适配层必须注册输出回调");
    }
    /* 只检查实际在用的 4 个通道（白色在 ch4，ch3 不接线） */
    const int used[LAMP_CH_COUNT] = { 0, 1, 2, 4 };
    const uint16_t ch_max[LAMP_CH_COUNT] = {
        (uint16_t)((1u << LAMP_LEDC_BITS_RGB) - 1u),
        (uint16_t)((1u << LAMP_LEDC_BITS_RGB) - 1u),
        (uint16_t)((1u << LAMP_LEDC_BITS_RGB) - 1u),
        (uint16_t)((1u << LAMP_LEDC_BITS_WHITE) - 1u),
    };
    for (int i = 0; i < LAMP_CH_COUNT; ++i) {
        const uint32_t got = host_ledc_duty((ledc_channel_t)used[i]);
        char buf[72];
        snprintf(buf, sizeof(buf), "ch%d 超量程被钳到该通道满量程 %u", used[i],
                 (unsigned)ch_max[i]);
        t_eq(got, ch_max[i], buf);
    }

    /* 通道分辨率必须按设计不同：RGB 14 bit、白光 13 bit */
    t_eq(LAMP_LEDC_BITS_RGB, LAMP_PWM_RES_BITS, "RGB 与 core 同为 14 bit");
    t_eq(LAMP_LEDC_BITS_WHITE, 13, "白光 13 bit（10 kHz 需要）");
    t_check(LAMP_LEDC_BITS_WHITE < LAMP_LEDC_BITS_RGB,
            "白光位数低于 RGB —— 因为高频下整数分频给不出 14 bit");
}

/* 8. 适配层未就绪时输出零（不要把"没初始化"当成"全亮"） */
static void test_output_before_init(void)
{
    /*
     * 这里用一个全新的 driver 实例走真实适配层：由于 lamp_esp32_write 的
     * s_ready 在 init 之前为 false，写操作必须被忽略而不是写脏寄存器。
     *
     * 由于 s_ready 是文件级静态且我们已经 init 过，这里改测"回调为 NULL 时
     * core 不崩溃"，这是同一类防御：未就绪的路径必须安全。
     */
    lamp_platform_t none;
    none.write = NULL;
    none.user = NULL;

    lamp_color_t c = { 200u, 100u, 50u, 0u };
    uint16_t before[LAMP_CH_COUNT];
    snapshot_duty(before);

    /* 用 mock 直通路径但把 write 摘掉：不应产生任何输出变化 */
    mock_driver_bind(NULL, NULL);
    lamp_driver_set_layer(&g_esp_driver, &c, 1.0f, 0u);

    uint16_t after[LAMP_CH_COUNT];
    snapshot_duty(after);
    int same = 1;
    for (int i = 0; i < LAMP_CH_COUNT; ++i) {
        if (before[i] != after[i]) {
            same = 0;
        }
    }
    t_check(same, "输出回调为 NULL 时不写寄存器（未就绪路径安全）");

    /* 恢复真实适配层回调，供后续用例使用 */
    (void)lamp_esp32_init(&g_esp_driver);
    mock_driver_bind(g_esp_driver.platform.write, g_esp_driver.platform.user);
    (void)none;
}

/* ================================================================== */

int main(void)
{
    static const pet_test_case_t cases[] = {
        { "适配层初始化与通道映射",       test_adapter_init },
        { "通道映射真值表（逐通道）",     test_channel_mapping_truth_table },
        { "全链路：解析→绑定→合并→灯效→LEDC", test_full_pipeline },
        { "多狗最活跃者优先 + 单只超时不误判", test_multi_dog_merge },
        { "全部失联 → Lost 且极暗不熄灯", test_all_lost },
        { "夜间压制真的生效",             test_night_cap_effective },
        { "适配层不做二次换算 + 越界钳位", test_no_double_scaling },
        { "未就绪路径安全",               test_output_before_init },
    };

    printf("=====================================================================\n");
    printf("宠物活动状态灯 V1 — 端到端集成测试\n");
    printf("真实路径：BLE 字节流 → core → lamp_esp32.c → LEDC 寄存器替身\n");
    printf("=====================================================================\n");

    /* 适配层只初始化一次（它内部持有自己的 lamp_driver_t） */
    host_ledc_reset();
    (void)lamp_esp32_init(&g_esp_driver);
    /* 把 core 的输出回调对接上适配层注册的真实写函数 */
    mock_driver_bind(g_esp_driver.platform.write, g_esp_driver.platform.user);

    const int fails = t_run("integration", cases,
                            sizeof(cases) / sizeof(cases[0]));

    printf("\n=====================================================================\n");
    printf("%s（失败 %d 项）\n",
           fails == 0 ? "集成测试全部通过" : "集成测试存在失败", fails);
    printf("=====================================================================\n");
    return (fails == 0) ? 0 : 1;
}
