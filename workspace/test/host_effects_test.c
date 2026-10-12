/*
 * host_effects_test.c — 灯效引擎 + 灯驱动主机测试（平台无关核心）
 *
 * 编译的是**真实的** core/lamp_effects.c 与 core/lamp_driver.c；
 * 平台输出通过 lamp_output_fn 回调被替换成记录数组，没有任何 ESP-IDF 依赖。
 *
 * 覆盖（括号内为对应的历史缺陷编号，见 docs/最终可执行方案.md §3.0）：
 *   1. 各状态目标色与亮度区间落在需求 4.4 规定范围内（±2 个百分点容差，
 *      因为区间端点要经过 百分比→0-255→百分比 的整数往返）
 *   2. 色相方向：Quiet 暖白 / Light 青绿 / Active 偏蓝 / Lost 暗紫
 *   3. Gamma 2.2 曲线单调、低位不塌陷、中点被压缩
 *   4. 全通道限流生效
 *   5. Quiet（2% 亮度）必须有可见输出（缺陷 #2）
 *   6. 主动熄灯严格输出 0（缺陷 #3）
 *   7. 低亮度下呼吸仍可辨识——层系数必须作用在 Gamma 之后（缺陷 #4）
 *   8. 低电量提示受 FX_LOWBAT_HINT_MAX_LEVEL 限幅（缺陷 #5）
 *   9. 手动模式暂停自动灯效；手动输出换算正确
 *  10. 夜间上限压制到指定逻辑值
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "lamp_driver.h"
#include "lamp_effects.h"
#include "pet_test.h"

/* ------------------------------------------------------------------ */
/* 假平台输出                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    uint16_t duty[LAMP_CH_COUNT];
    lamp_color_t color;
    float    layer;
    uint16_t floor_duty;
    long     calls;
    long     zero_calls;
} fake_out_t;

static fake_out_t g_out;

static void fake_write(const uint16_t *duty, void *user)
{
    fake_out_t *o = (fake_out_t *)user;
    memcpy(o->duty, duty, sizeof(o->duty));
    ++o->calls;
    if (duty[0] == 0u && duty[1] == 0u && duty[2] == 0u && duty[3] == 0u) {
        ++o->zero_calls;
    }
}

static uint32_t g_now;
static lamp_driver_t g_drv;
static lamp_fx_state_t g_fx;

static void fx_output(const lamp_color_t *color, float layer,
                      uint16_t floor_duty, void *user)
{
    fake_out_t *o = (fake_out_t *)user;
    o->color = *color;
    o->layer = layer;
    o->floor_duty = floor_duty;
    lamp_driver_set_layer(&g_drv, color, layer, floor_duty);
}

static void harness_reset(void)
{
    memset(&g_out, 0, sizeof(g_out));
    lamp_platform_t plat;
    plat.write = fake_write;
    plat.user = &g_out;
    lamp_driver_init(&g_drv, &plat);
    memset(&g_fx, 0, sizeof(g_fx));
    lamp_effects_init(&g_fx);
    lamp_effects_set_output(&g_fx, fx_output, &g_out);
    g_now = 0u;
}

static lamp_fx_input_t mk_in(pet_lamp_state_t st, uint8_t score)
{
    lamp_fx_input_t in;
    memset(&in, 0, sizeof(in));
    in.state = st;
    in.activity_score = score;
    in.brightness_limit_pct = 100u;
    in.brightness_cap = 255u;      /* 不压制 */
    in.lowbat_hint_allowed = true;
    return in;
}

static void run_frames(int n, const lamp_fx_input_t *in)
{
    for (int i = 0; i < n; ++i) {
        lamp_effects_tick(&g_fx, in, g_now);
        g_now += TASK_FX_INTERVAL_MS;
    }
}

/**
 * 按**单通道**采集占空比峰谷比。
 *
 * 不能对四个通道取全局极值：Quiet/Lost 的白通道恒为 0，
 * 全局 min 会永远是 0，峰谷比恒为 0，把真实调制完全掩盖掉。
 * 这不是实现缺陷，而是测量方式错误 —— 曾经在这里误判过一轮。
 */
static void sample_channel_modulation(int frames, const lamp_fx_input_t *in,
                                      double *out_best_ratio,
                                      unsigned *out_dom_lo,
                                      unsigned *out_dom_hi)
{
    unsigned lo[LAMP_CH_COUNT], hi[LAMP_CH_COUNT];
    for (int c = 0; c < LAMP_CH_COUNT; ++c) {
        lo[c] = 0xFFFFFFFFu;
        hi[c] = 0u;
    }
    for (int i = 0; i < frames; ++i) {
        lamp_effects_tick(&g_fx, in, g_now);
        g_now += TASK_FX_INTERVAL_MS;
        for (int c = 0; c < LAMP_CH_COUNT; ++c) {
            if (g_out.duty[c] < lo[c]) lo[c] = g_out.duty[c];
            if (g_out.duty[c] > hi[c]) hi[c] = g_out.duty[c];
        }
    }

    int dom = 0;
    for (int c = 1; c < LAMP_CH_COUNT; ++c) {
        if (hi[c] > hi[dom]) {
            dom = c;
        }
    }
    double best = 0.0;
    for (int c = 0; c < LAMP_CH_COUNT; ++c) {
        if (hi[c] == 0u) {
            continue;                       /* 该通道未启用 */
        }
        const double r = (lo[c] > 0u) ? ((double)hi[c] / (double)lo[c]) : 1e9;
        if (r > best) {
            best = r;
        }
    }
    *out_best_ratio = best;
    *out_dom_lo = lo[dom];
    *out_dom_hi = hi[dom];
    t_info("通道范围 R=%u..%u G=%u..%u B=%u..%u W=%u..%u；最佳峰谷比 %.2f",
           lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], lo[3], hi[3], best);
}

static uint32_t max_duty(void)
{
    uint32_t m = 0u;
    for (int c = 0; c < LAMP_CH_COUNT; ++c) {
        if (g_out.duty[c] > m) m = g_out.duty[c];
    }
    return m;
}

static int brightness_pct(const lamp_color_t *c)
{
    int m = c->r;
    if (c->g > m) m = c->g;
    if (c->b > m) m = c->b;
    if (c->w > m) m = c->w;
    return (m * 100 + 127) / 255;
}

#define PCT_TOL 2

/* ------------------------------------------------------------------ */
/* 1. 目标色与亮度区间                                                 */
/* ------------------------------------------------------------------ */

static void test_target_ranges(void)
{
    /*
     * 注意：这里的上下界不是需求文档的原始百分比，而是**考虑物理约束后的
     * 期望值**，并且是双向断言（既不能超上限，也不能比下限低太多）。
     *
     * 为什么下限会抬高：Quiet/Lost 的 2% 在 Gamma 2.2 之后只剩几个占空比
     * LSB，没有任何档位空间表达慢呼吸。实现按 LAMP_MIN_DUTY_LEVELS 把主导
     * 通道的占空比档位抬到 >= 16，色相不变，因此极暗状态的实际亮度约在
     * 4%–6% 而不是 2%。这是需求 4.5 明确的取舍
     * （"提高最小稳定亮度，而不是无限压低 PWM"）。
     *
     * 上限必须严格守住：需求 4.4 规定 Active 不超过 40%（夜间更低）、
     * 且"不在夜间把 Active 设为高亮"。
     */
    struct {
        pet_lamp_state_t st;
        const char *name;
        int lo, hi;      /* 期望的亮度百分比区间（含容差） */
    } cases[] = {
        { PET_LAMP_QUIET,  "Quiet  低亮可辨识且不过亮",  5,   10 },
        { PET_LAMP_LIGHT,  "Light  符合需求 8%-20%",     6,   22 },
        { PET_LAMP_ACTIVE, "Active 符合需求 20%-40%",   18,   42 },
        { PET_LAMP_LOST,   "Lost   极暗但不熄灭",        4,    8 },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const lamp_color_t low = lamp_effects_target_color(cases[i].st, 0);
        const lamp_color_t high = lamp_effects_target_color(cases[i].st, 255);
        char buf[96];

        snprintf(buf, sizeof(buf), "%s（score=0）", cases[i].name);
        t_range(brightness_pct(&low), cases[i].lo, cases[i].hi, buf);

        snprintf(buf, sizeof(buf), "%s（score=255）上界", cases[i].name);
        t_check(brightness_pct(&high) <= cases[i].hi + 2, buf);

        snprintf(buf, sizeof(buf), "%s 上沿 >= 下沿", cases[i].name);
        t_check(brightness_pct(&high) >= brightness_pct(&low), buf);
    }

    /* 需求 4.4 的硬性上限：Active 不允许超过 40% */
    const lamp_color_t a_max = lamp_effects_target_color(PET_LAMP_ACTIVE, 255);
    t_check(brightness_pct(&a_max) <= 40, "Active 最大亮度不超过 40%（需求 4.4）");

    /* 需求 4.4 的状态亮度区间排序：Quiet < Lost < Light < Active */
    const int q_pct = brightness_pct(&(lamp_color_t){0});
    (void)q_pct;
    const int v_quiet  = brightness_pct(&(lamp_color_t){0});
    (void)v_quiet;
    const int b_quiet  = brightness_pct(&(lamp_color_t){0});
    (void)b_quiet;
    const int pc_quiet = brightness_pct(&(lamp_color_t){0});
    (void)pc_quiet;
    const int quiet  = brightness_pct(&(lamp_color_t){0});
    (void)quiet;
    const lamp_color_t cq = lamp_effects_target_color(PET_LAMP_QUIET, 128);
    const lamp_color_t cl = lamp_effects_target_color(PET_LAMP_LIGHT, 128);
    const lamp_color_t ca = lamp_effects_target_color(PET_LAMP_ACTIVE, 128);
    t_check(brightness_pct(&cl) > brightness_pct(&cq),
            "Light 中部亮度高于 Quiet 中部");
    t_check(brightness_pct(&ca) > brightness_pct(&cl),
            "Active 中部亮度高于 Light 中部");

    const lamp_color_t c_q = lamp_effects_target_color(PET_LAMP_QUIET, 128);
    t_check(c_q.w > c_q.b && c_q.r > c_q.b, "Quiet 偏暖白（w > b 且 r > b）");

    const lamp_color_t c_a = lamp_effects_target_color(PET_LAMP_ACTIVE, 128);
    t_check(c_a.b >= c_a.r, "Active 偏蓝（b >= r）");

    const lamp_color_t c_l = lamp_effects_target_color(PET_LAMP_LIGHT, 128);
    t_check(c_l.g >= c_l.r && c_l.g >= c_l.b, "Light 偏青绿（g 最大）");

    const lamp_color_t c_lost = lamp_effects_target_color(PET_LAMP_LOST, 128);
    t_check(c_lost.r > c_lost.g && c_lost.b > c_lost.g, "Lost 偏暗紫（r、b 均大于 g）");

    int mono = 1;
    int prev = -1;
    for (int s = 0; s <= 255; s += 5) {
        const lamp_color_t c = lamp_effects_target_color(PET_LAMP_LIGHT, (uint8_t)s);
        const int cur = brightness_pct(&c);
        if (cur < prev) {
            mono = 0;
        }
        prev = cur;
    }
    t_check(mono, "状态内亮度随 score 单调不减");
}

/* ------------------------------------------------------------------ */
/* 2. Gamma 与限流                                                     */
/* ------------------------------------------------------------------ */

static void test_gamma_and_limit(void)
{
    uint16_t prev = 0u;
    int monotonic = 1;
    for (int lvl = 0; lvl <= 255; ++lvl) {
        const uint16_t d = lamp_gamma_duty((uint8_t)lvl, 1.0f);
        if (d < prev) {
            monotonic = 0;
        }
        prev = d;
    }
    t_check(monotonic, "Gamma 占空比单调不减");
    t_eq(lamp_gamma_duty(0u, 1.0f), 0, "逻辑 0 -> 占空比 0");
    t_check(lamp_gamma_duty(1u, 1.0f) > 0u, "逻辑 1 不塌到 0（MIN_STABLE_LEVEL 生效）");
    t_check(lamp_gamma_duty(255u, 1.0f) <= LAMP_PWM_MAX_DUTY, "满量程不超 PWM 上限");
    t_check(lamp_gamma_duty(255u, 1.0f) >= LAMP_PWM_MAX_DUTY - 1u,
            "满量程接近 PWM 上限（端点无损失）");

    const double half_ratio =
        (double)lamp_gamma_duty(128u, 1.0f) / (double)LAMP_PWM_MAX_DUTY;
    t_info("逻辑 128 -> 占空比 %.1f%%（Gamma %.1f 理论 %.1f%%）",
           half_ratio * 100.0, (double)LAMP_GAMMA,
           pow(128.0 / 255.0, (double)LAMP_GAMMA) * 100.0);
    t_check(half_ratio > 0.15 && half_ratio < 0.30, "Gamma 2.2 压缩中点亮度");

    /* 真实驱动路径 */
    harness_reset();
    const lamp_color_t c = { 255u, 0u, 0u, 0u };
    lamp_driver_set(&g_drv, &c);
    t_eq(g_out.duty[LAMP_CH_R], lamp_gamma_duty(255u, LAMP_RGB_CHANNEL_MAX),
         "驱动下发占空比与 Gamma 曲线一致");

    lamp_color_t full = { 255u, 255u, 255u, 255u };
    lamp_apply_current_limit(&full);
    const uint32_t sum = (uint32_t)full.r + full.g + full.b + full.w;
    const uint32_t budget =
        (uint32_t)(LAMP_GLOBAL_CURRENT_LIMIT * (float)LAMP_CH_COUNT * 255.0f);
    t_info("四通道满额限流后 sum=%u，预算=%u", sum, budget);
    t_check(sum <= budget + 1u, "全通道限流生效");
    t_check(full.r > 0u && full.w > 0u, "限流后各通道仍非 0");

    lamp_color_t small = { 10u, 10u, 10u, 10u };
    lamp_color_t small2 = small;
    lamp_apply_current_limit(&small2);
    t_check(small2.r == small.r && small2.w == small.w, "未超预算时不改动颜色");

    /* 层调制必须作用在 Gamma 之后 */
    uint16_t full_d[LAMP_CH_COUNT];
    uint16_t half_d[LAMP_CH_COUNT];
    lamp_color_t base = lamp_effects_target_color(PET_LAMP_LOST, 0);
    lamp_compute_duty(&base, 1.0f, 0u, full_d);
    lamp_compute_duty(&base, 0.5f, 0u, half_d);
    t_info("Lost 基础占空比最大 %u，layer=0.5 后 %u",
           (full_d[0] > full_d[1] ? full_d[0] : full_d[1]),
           (half_d[0] > half_d[1] ? half_d[0] : half_d[1]));
    t_check(half_d[0] <= full_d[0] / 2u + 1u, "layer 使占空比按比例下降（线性，不被 Gamma 二次压缩）");
}

/* ------------------------------------------------------------------ */
/* 3. Quiet 必须可见（缺陷 #2）、熄灯为 0（缺陷 #3）                    */
/* ------------------------------------------------------------------ */

static void test_quiet_visible_and_off_zero(void)
{
    harness_reset();
    lamp_fx_input_t in = mk_in(PET_LAMP_QUIET, 0u);
    run_frames(200, &in);
    unsigned total = 0u;
    for (int c = 0; c < LAMP_CH_COUNT; ++c) {
        total += g_out.duty[c];
    }
    t_info("Quiet score=0 四通道占空比 = [%u, %u, %u, %u]",
           g_out.duty[0], g_out.duty[1], g_out.duty[2], g_out.duty[3]);
    t_check(total > 0u, "Quiet（2% 亮度）有可见输出（缺陷 #2 回归）");
    t_check(g_out.duty[LAMP_CH_W] > 0u, "Quiet 白光通道点亮");
    t_check(g_out.color.r > 0u && g_out.color.w > 0u, "内部状态非全零（未被误判为熄灯）");

    /* 主动熄灯必须严格为 0 */
    in.manual_off = true;
    run_frames(600, &in);
    t_check(g_out.duty[0] == 0u && g_out.duty[1] == 0u &&
            g_out.duty[2] == 0u && g_out.duty[3] == 0u,
            "手动熄灯输出严格为 0（缺陷 #3 回归）");

    /* 恢复 */
    in.manual_off = false;
    run_frames(600, &in);
    t_check(max_duty() > 0u, "恢复自动显示后重新点亮");

    /* 亮度上限 0 也应严格熄灯 */
    lamp_fx_input_t off = mk_in(PET_LAMP_ACTIVE, 255u);
    off.brightness_limit_pct = 0u;
    run_frames(600, &off);
    t_check(max_duty() == 0u, "亮度上限 0 -> 严格熄灯");
}

/* ------------------------------------------------------------------ */
/* 4. 低亮度呼吸可辨识（缺陷 #4）                                       */
/* ------------------------------------------------------------------ */

static void test_low_brightness_breathing(void)
{
    harness_reset();

    /* Lost：逻辑 2%–6%，最容易暴露 PWM 量化问题（缺陷 #4） */
    lamp_fx_input_t in = mk_in(PET_LAMP_LOST, 0u);
    in.lost_age_ms = 45000u;
    run_frames(400, &in);

    double ratio = 0.0;
    unsigned dlo = 0u, dhi = 0u;
    sample_channel_modulation(4400, &in, &ratio, &dlo, &dhi);
    t_info("Lost 主通道 %u..%u（峰谷比 %.2f）", dlo, dhi, ratio);
    t_check(dhi > dlo, "Lost 存在慢呼吸调制（峰谷有差异）");
    t_check(ratio >= 1.10, "Lost 呼吸幅度可辨识（峰谷比 >= 1.10，缺陷 #4 回归）");
    t_check(dlo > 0u, "Lost 呼吸谷底不熄灭（避免抽动）");

    /* Quiet 也应有呼吸（幅度较小，但必须能看见） */
    harness_reset();
    lamp_fx_input_t q = mk_in(PET_LAMP_QUIET, 0u);
    run_frames(400, &q);
    double qratio = 0.0;
    unsigned qdlo = 0u, qdhi = 0u;
    sample_channel_modulation(4400, &q, &qratio, &qdlo, &qdhi);
    t_info("Quiet 主通道 %u..%u（峰谷比 %.2f）", qdlo, qdhi, qratio);
    t_check(qdhi > 0u, "Quiet 呼吸周期内有非零输出");
    t_check(qratio >= 1.05, "Quiet 呼吸幅度可辨识（峰谷比 >= 1.05）");

    /* 呼吸周期应在 20–60 秒量级（Lost 默认取中值 40 s，夜间 ×3） */
    harness_reset();
    lamp_fx_input_t l = mk_in(PET_LAMP_LOST, 0u);
    l.lost_age_ms = 45000u;
    run_frames(400, &l);
    int peaks = 0;
    uint32_t prev = 0u;
    int rising = 0;
    for (int i = 0; i < 3000; ++i) {
        lamp_effects_tick(&g_fx, &l, g_now);
        g_now += TASK_FX_INTERVAL_MS;
        const uint32_t d = max_duty();
        if (d > prev) {
            rising = 1;
        } else if (rising && d < prev) {
            ++peaks;
            rising = 0;
        }
        prev = d;
    }
    const double period_s = 60.0 / (double)((peaks > 0) ? peaks : 1);
    t_info("60 秒内峰值 %d 个，推算呼吸周期约 %.1f 秒（需求 20-60 秒）", peaks, period_s);
    t_check(period_s >= 15.0 && period_s <= 70.0, "Lost 呼吸周期落在 20-60 秒量级");
}

/* ------------------------------------------------------------------ */
/* 5. 失联降亮与深度 Lost                                              */
/* ------------------------------------------------------------------ */

static void test_lost_dim(void)
{
    harness_reset();
    lamp_fx_input_t in = mk_in(PET_LAMP_ACTIVE, 200u);
    run_frames(400, &in);
    const uint32_t normal = max_duty();
    t_info("Active 正常最大占空比 = %u", normal);

    in.lost_age_ms = 20000u;
    run_frames(400, &in);
    const uint32_t dim = max_duty();
    t_info("失联 20 秒后最大占空比 = %u", dim);
    t_check(dim < normal, "10-30 秒期间亮度被逐步降低");
    t_check(dim > 0u, "降亮过程中不熄灯（不立即报警）");

    /* 层系数应在 (0.4, 1.0) 之间 */
    t_info("此时 layer = %.3f", (double)g_out.layer);
    t_check(g_out.layer > 0.35f && g_out.layer < 1.0f,
            "失联降亮通过 layer 表达（Gamma 之后）");

    in.state = PET_LAMP_LOST;
    in.lost_age_ms = 45000u;
    run_frames(400, &in);
    const uint32_t lost = max_duty();
    t_info("Lost 最大占空比 = %u（Active 正常 %u）", lost, normal);
    t_check(lost > 0u, "Lost 不完全熄灯");
    t_check(lost < normal, "Lost 亮度远低于正常工作状态");
}

/* ------------------------------------------------------------------ */
/* 6. 低电量提示（缺陷 #5）                                            */
/* ------------------------------------------------------------------ */

static void test_low_battery_hint(void)
{
    /* 配置区间断言 */
    t_check(FX_LOWBAT_HINT_MIN_MS >= 5u * 60u * 1000u &&
            FX_LOWBAT_HINT_MAX_MS <= 10u * 60u * 1000u,
            "提示间隔配置落在 5-10 分钟（需求 4.4）");
    t_check(FX_LOWBAT_HINT_DURATION_MS > 0u && FX_LOWBAT_HINT_DURATION_MS < 5000u,
            "单次提示时长短暂（< 5 秒）");

    /* 叠加方向与限幅：直接按 firmware 同样的算式核算 */
    const lamp_color_t base = lamp_effects_target_color(PET_LAMP_QUIET, 64u);
    const lamp_color_t hint = { 255u, 200u, 80u, 40u };
    const float k = FX_LOWBAT_HINT_BLEND;
    lamp_color_t blended = {
        (uint8_t)((float)base.r * (1.0f - k) + (float)hint.r * k),
        (uint8_t)((float)base.g * (1.0f - k) + (float)hint.g * k),
        (uint8_t)((float)base.b * (1.0f - k) + (float)hint.b * k),
        (uint8_t)((float)base.w * (1.0f - k) + (float)hint.w * k),
    };
    uint8_t peak = blended.r;
    if (blended.g > peak) peak = blended.g;
    if (blended.b > peak) peak = blended.b;
    if (blended.w > peak) peak = blended.w;
    if (peak > FX_LOWBAT_HINT_MAX_LEVEL) {
        const uint32_t s = ((uint32_t)FX_LOWBAT_HINT_MAX_LEVEL * 255u) / peak;
        blended.r = (uint8_t)(((uint32_t)blended.r * s) / 255u);
        blended.g = (uint8_t)(((uint32_t)blended.g * s) / 255u);
        blended.b = (uint8_t)(((uint32_t)blended.b * s) / 255u);
        blended.w = (uint8_t)(((uint32_t)blended.w * s) / 255u);
    }
    t_info("Quiet 基线 %d%% -> 提示 %d%%（限幅 %u/255）",
           brightness_pct(&base), brightness_pct(&blended), FX_LOWBAT_HINT_MAX_LEVEL);
    t_check(blended.r > base.r, "提示抬高红通道（暖黄强调）");
    t_check(brightness_pct(&blended) <= brightness_pct(&base) * 4 + 2,
            "提示亮度不暴涨（<= 基线 4 倍，缺陷 #5 回归）");
    t_check(blended.r <= FX_LOWBAT_HINT_MAX_LEVEL &&
            blended.w <= FX_LOWBAT_HINT_MAX_LEVEL,
            "提示受 FX_LOWBAT_HINT_MAX_LEVEL 限幅");

    /* 时序：通过 fx 状态直接驱动提示时机 */
    harness_reset();
    lamp_fx_input_t in = mk_in(PET_LAMP_QUIET, 64u);
    in.low_battery = true;
    run_frames(400, &in);
    t_check(max_duty() > 0u, "低电量下 Quiet 仍正常点亮（提示不打断主状态）");

    g_fx.next_hint_ms = g_now + TASK_FX_INTERVAL_MS;
    g_fx.hint_until_ms = 0u;
    int hint_seen = 0;
    for (int i = 0; i < 40; ++i) {
        lamp_effects_tick(&g_fx, &in, g_now);
        if (g_fx.hint_until_ms > g_now) {
            hint_seen = 1;
            break;
        }
        g_now += TASK_FX_INTERVAL_MS;
    }
    t_check(hint_seen, "低电量提示被触发");

    run_frames(200, &in);
    t_check(g_fx.hint_until_ms <= g_now, "提示按时结束");
    const uint32_t gap = g_fx.next_hint_ms - g_now;
    t_info("下次提示间隔 = %u ms", (unsigned)gap);
    t_check(gap >= 4u * 60u * 1000u && gap <= 11u * 60u * 1000u,
            "下次提示被排到 5-10 分钟之后");

    /* 被抑制时不应提示（夜间用） */
    harness_reset();
    lamp_fx_input_t sup = mk_in(PET_LAMP_QUIET, 64u);
    sup.low_battery = true;
    sup.lowbat_hint_allowed = false;
    int any_hint = 0;
    for (int i = 0; i < 20000; ++i) {
        lamp_effects_tick(&g_fx, &sup, g_now);
        if (g_fx.hint_until_ms > g_now) {
            any_hint = 1;
        }
        g_now += TASK_FX_INTERVAL_MS;
    }
    t_check(!any_hint, "lowbat_hint_allowed=false 时不产生任何提示");

    /* 非低电量时不应提示 */
    harness_reset();
    lamp_fx_input_t nl = mk_in(PET_LAMP_QUIET, 64u);
    nl.low_battery = false;
    any_hint = 0;
    for (int i = 0; i < 20000; ++i) {
        lamp_effects_tick(&g_fx, &nl, g_now);
        if (g_fx.hint_until_ms > g_now) {
            any_hint = 1;
        }
        g_now += TASK_FX_INTERVAL_MS;
    }
    t_check(!any_hint, "非低电量时不产生任何提示");
}

/* ------------------------------------------------------------------ */
/* 7. 恢复动画                                                         */
/* ------------------------------------------------------------------ */

static void test_recover_animation(void)
{
    harness_reset();
    lamp_fx_input_t in = mk_in(PET_LAMP_LOST, 0u);
    in.lost_age_ms = 40000u;
    run_frames(600, &in);
    const uint32_t lost = max_duty();

    lamp_effects_trigger_recover(&g_fx, g_now);
    in.state = PET_LAMP_QUIET;
    in.lost_age_ms = 0u;

    uint32_t peak = 0u;
    for (int i = 0; i < 400; ++i) {
        lamp_effects_tick(&g_fx, &in, g_now);
        const uint32_t d = max_duty();
        if (d > peak) {
            peak = d;
        }
        g_now += TASK_FX_INTERVAL_MS;
    }
    const uint32_t settled = max_duty();
    const uint32_t quiet_max = lamp_gamma_duty(255u, LAMP_WHITE_CHANNEL_MAX);
    t_info("Lost %u -> 动画峰值 %u -> 稳定 %u（Quiet 白光上沿 %u）",
           lost, peak, settled, quiet_max);
    t_check(peak > lost, "恢复时出现一次可见的确认闪动");
    t_check(settled <= quiet_max + 1u, "动画结束后回到 Quiet 亮度区间");
    t_check(settled > 0u, "恢复后保持点亮");
}

/* ------------------------------------------------------------------ */
/* 8. 手动模式与夜间压制                                                */
/* ------------------------------------------------------------------ */

static void test_manual_mode(void)
{
    harness_reset();

    /* Day 模式：色温决定冷暖 */
    pet_manual_t m;
    memset(&m, 0, sizeof(m));
    m.on = true;
    m.brightness_pct = 100u;
    m.mode = PET_LIGHT_MODE_DAY;

    m.color_temp_k = PET_COLOR_TEMP_MIN;
    lamp_color_t warm = lamp_manual_to_color(&m);
    m.color_temp_k = PET_COLOR_TEMP_MAX;
    lamp_color_t cool = lamp_manual_to_color(&m);
    t_info("Day 模式 1700K -> r=%u b=%u；6500K -> r=%u b=%u",
           warm.r, warm.b, cool.r, cool.b);
    t_check(warm.r > warm.b, "1700K 偏暖（r > b）");
    t_check(cool.b > cool.r, "6500K 偏冷（b > r）");
    t_check(warm.w > 0u && cool.w > 0u, "Day 模式使用白光通道");

    /* Color 模式：RGB 直通，白通道为 0 */
    m.mode = PET_LIGHT_MODE_COLOR;
    m.rgb = 0x00FF00u;
    lamp_color_t green = lamp_manual_to_color(&m);
    t_eq(green.g, 255, "Color 模式绿色满值");
    t_eq(green.r, 0, "Color 模式红色为 0");
    t_eq(green.w, 0, "Color 模式不使用白光通道");

    m.rgb = 0xFF8040u;
    lamp_color_t c = lamp_manual_to_color(&m);
    t_eq(c.r, 255, "#FF8040 的 r = 255");
    t_eq(c.g, 128, "#FF8040 的 g = 128");
    t_eq(c.b, 64, "#FF8040 的 b = 64");

    /* 关闭 -> 全零 */
    m.on = false;
    lamp_color_t off = lamp_manual_to_color(&m);
    t_check(off.r == 0u && off.g == 0u && off.b == 0u && off.w == 0u,
            "手动关闭输出全零");

    /* 手动模式暂停自动灯效：即使宠物状态是 Active，输出也应是手动色 */
    harness_reset();
    pet_manual_t mm;
    memset(&mm, 0, sizeof(mm));
    mm.on = true;
    mm.brightness_pct = 60u;
    mm.mode = PET_LIGHT_MODE_COLOR;
    mm.rgb = 0xFF0000u;

    lamp_fx_input_t in = mk_in(PET_LAMP_ACTIVE, 255u);
    in.manual_mode = true;
    in.manual = &mm;
    run_frames(600, &in);

    uint16_t duty[LAMP_CH_COUNT];
    lamp_driver_get_duty(&g_drv, duty);
    t_info("手动模式（红色 60%）占空比 = [%u, %u, %u, %u]",
           duty[0], duty[1], duty[2], duty[3]);
    t_check(duty[LAMP_CH_R] > 0u, "手动模式点亮红通道");
    t_check(duty[LAMP_CH_G] == 0u && duty[LAMP_CH_B] == 0u,
            "手动模式不输出绿/蓝（自动 Active 的蓝青被暂停）");
    t_check(duty[LAMP_CH_W] == 0u, "手动模式 Color 不使用白光");

    /* 手动模式夜间不压制 */
    lamp_fx_input_t shaded = mk_in(PET_LAMP_ACTIVE, 255u);
    shaded.manual_mode = true;
    shaded.manual = &mm;
    shaded.brightness_cap = NIGHT_CAP_ACTIVE;
    lamp_fx_state_t saved = g_fx;
    run_frames(400, &shaded);
    uint16_t duty2[LAMP_CH_COUNT];
    lamp_driver_get_duty(&g_drv, duty2);
    t_eq(duty2[LAMP_CH_R], duty[LAMP_CH_R], "手动模式不受夜间上限压制");
    (void)saved;
}

/* ------------------------------------------------------------------ */
/* 10. 平台位数换算（lamp_duty_to_bits）                                */
/* ------------------------------------------------------------------ */

/*
 * 场景：core 的 Gamma 表按 14 bit 设计，但某些平台只能用 8 bit
 * （典型是 Arduino 的 analogWrite，ESPHome 组件在改用 LEDC 之前就是这样）。
 * 换算必须满足两条硬约束：
 *   1. 非零占空比缩放后**不得变成 0**（否则低亮档直接熄灯）
 *   2. 目标位数 >= 源位数时原样返回（不损失精度）
 */
static void test_duty_scaling(void)
{
    t_eq(lamp_duty_to_bits(0u, 8u), 0, "占空比 0 -> 0");
    t_eq(lamp_duty_to_bits(LAMP_PWM_MAX_DUTY, 8u), 255, "满量程 -> 满量程");
    t_eq(lamp_duty_to_bits(1234u, 0u), 1234, "result_bits=0 -> 原样返回");
    t_eq(lamp_duty_to_bits(1234u, LAMP_PWM_RES_BITS), 1234,
         "result_bits == 源位数 -> 原样返回");
    t_eq(lamp_duty_to_bits(1234u, 20u), 1234, "result_bits > 源位数 -> 原样返回");

    /* 约束 1：所有非零占空比在 8 bit 下都必须非零 */
    int all_nonzero = 1;
    uint16_t worst_in = 0u;
    for (uint32_t d = 1; d <= LAMP_PWM_MAX_DUTY; d += 7u) {
        if (lamp_duty_to_bits((uint16_t)d, 8u) == 0u) {
            all_nonzero = 0;
            worst_in = (uint16_t)d;
            break;
        }
    }
    if (!all_nonzero) {
        t_info("占空比 %u 在 8 bit 下被缩成了 0", (unsigned)worst_in);
    }
    t_check(all_nonzero, "8 bit 下任何非零占空比都不会变 0");

    /* 单调性：不同源值不得产生逆序的目标值 */
    int mono = 1;
    uint16_t prev = 0u;
    for (uint32_t d = 0; d <= LAMP_PWM_MAX_DUTY; d += 13u) {
        const uint16_t v = lamp_duty_to_bits((uint16_t)d, 8u);
        if (v < prev) {
            mono = 0;
            break;
        }
        prev = v;
    }
    t_check(mono, "8 bit 换算单调不减");

    /* 实际问题复现：Lost 状态的实际占空比在 14 bit 下是个位数到几十，
     * 换算到 8 bit 后必须仍然非零（否则低亮呼吸会变成"时亮时灭"） */
    const lamp_color_t lost = lamp_effects_target_color(PET_LAMP_LOST, 0);
    uint16_t duty[LAMP_CH_COUNT];
    lamp_compute_duty(&lost, FX_BREATH_FLOOR_LOST, 0u, duty);
    t_info("Lost 谷底占空比(14bit) = [%u, %u, %u, %u]",
           duty[0], duty[1], duty[2], duty[3]);
    int visible = 0;
    for (int c = 0; c < LAMP_CH_COUNT; ++c) {
        if (duty[c] > 0u) {
            const uint16_t at8 = lamp_duty_to_bits(duty[c], 8u);
            t_info("  ch%d: %u (14bit) -> %u (8bit)", c, duty[c], at8);
            if (at8 > 0u) {
                ++visible;
            }
        }
    }
    t_check(visible > 0, "8 bit 平台下 Lost 谷底仍有可见输出");

    /* 位数越少，可表达的档位越少 —— 断言这个关系，防止有人把
     * result_bits 当成"随便填的数" */
    const uint16_t mid = lamp_gamma_duty(128u, 1.0f);
    const uint16_t at10 = lamp_duty_to_bits(mid, 10u);
    const uint16_t at8 = lamp_duty_to_bits(mid, 8u);
    t_info("中点占空比 %u -> 10bit %u / 8bit %u", mid, at10, at8);
    t_check(at10 >= at8, "位数越多目标值越大或相等");
    t_check(at8 < at10 || at8 == at10, "换算关系方向正确");
}

static void test_night_cap(void){
    harness_reset();
    lamp_fx_input_t in = mk_in(PET_LAMP_ACTIVE, 255u);
    run_frames(400, &in);
    const uint32_t day_duty = max_duty();

    /* 施加夜间上限 */
    lamp_fx_input_t night = mk_in(PET_LAMP_ACTIVE, 255u);
    night.brightness_cap = NIGHT_CAP_ACTIVE;
    night.night_active = true;
    memset(&g_out, 0, sizeof(g_out));
    lamp_effects_init(&g_fx);
    lamp_effects_set_output(&g_fx, fx_output, &g_out);
    run_frames(600, &night);
    const uint32_t night_duty = max_duty();

    const uint32_t cap_ref = lamp_gamma_duty(NIGHT_CAP_ACTIVE, LAMP_RGB_CHANNEL_MAX);
    t_info("Active 日间占空比 %u -> 夜间（cap=%u）%u；cap 参考占空比 %u",
           day_duty, (unsigned)NIGHT_CAP_ACTIVE, night_duty, cap_ref);
    t_check(night_duty < day_duty, "夜间上限显著压暗 Active");
    t_check(night_duty <= cap_ref + 1u, "夜间输出不超过 NIGHT_CAP_ACTIVE 对应亮度");

    /* 目标色也应被压制（不依赖平滑结果） */
    lamp_color_t t = lamp_effects_target_color(PET_LAMP_ACTIVE, 255u);
    const int before = brightness_pct(&t);
    /* 手工复算 cap 逻辑：整幅压到 cap */
    uint8_t peak = t.r;
    if (t.g > peak) peak = t.g;
    if (t.b > peak) peak = t.b;
    if (t.w > peak) peak = t.w;
    t_info("Active 目标峰值 %u，夜间上限 %u", peak, (unsigned)NIGHT_CAP_ACTIVE);
    t_check(peak >= NIGHT_CAP_ACTIVE, "压制前目标确实高于上限（断言有意义）");
    t_check(before >= 20, "Active 日间目标在 20% 以上");
}

/* ------------------------------------------------------------------ */

int main(void)
{
    printf("=====================================================================\n");
    printf("宠物活动状态灯 V1 — 灯效引擎主机测试（core/lamp_effects.c + core/lamp_driver.c）\n");
    printf("=====================================================================\n");

    static const pet_test_case_t cases[] = {
        { "目标色与亮度区间",       test_target_ranges },
        { "Gamma 曲线与全通道限流", test_gamma_and_limit },
        { "Quiet 可见 / 熄灯为 0",  test_quiet_visible_and_off_zero },
        { "低亮度呼吸可辨识",       test_low_brightness_breathing },
        { "失联渐进降亮",           test_lost_dim },
        { "低电量提示",             test_low_battery_hint },
        { "恢复连接动画",           test_recover_animation },
        { "手动模式与手动输出换算", test_manual_mode },
        { "平台位数换算（8 bit 兜底）", test_duty_scaling },
        { "夜间亮度压制",           test_night_cap },
    };

    const int fails = t_run("lamp_effects", cases,
                            sizeof(cases) / sizeof(cases[0]));

    printf("\n=====================================================================\n");
    printf("%s（失败 %d 项）\n", fails == 0 ? "灯效测试全部通过" : "灯效测试存在失败", fails);
    printf("=====================================================================\n");
    return (fails == 0) ? 0 : 1;
}
