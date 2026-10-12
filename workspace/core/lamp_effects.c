/*
 * lamp_effects.c — 灯效引擎实现（平台无关）
 *
 * 顺序与阈值严格对应需求 4.4 / 4.5，关键取舍见 lamp_effects.h 顶部说明。
 */
#include "lamp_effects.h"

#include "lamp_gamma.h"

#include <math.h>
#include <string.h>

/* 2π。不用 M_PI：ESP-IDF 需 _USE_MATH_DEFINES，主机 -std=c99 下也不可见。 */
#define TWO_PI 6.28318530717958647692f

/* ------------------------------------------------------------------ */
/* 状态颜色表（需求 4.4）                                              */
/* ------------------------------------------------------------------ */

/** Quiet：2400–2700K 暖白 —— 以白光通道为主，RGB 只做色温偏移 */
#define RGB_QUIET_R   255u
#define RGB_QUIET_G   214u
#define RGB_QUIET_B   170u
#define RGB_QUIET_W   255u

/** Light：低饱和青绿 / 黄绿 */
#define RGB_LIGHT_R    90u
#define RGB_LIGHT_G   235u
#define RGB_LIGHT_B   150u
#define RGB_LIGHT_W    20u

/** Active：蓝青 / 天蓝 */
#define RGB_ACTIVE_R   30u
#define RGB_ACTIVE_G  180u
#define RGB_ACTIVE_B  255u
#define RGB_ACTIVE_W    0u

/** Lost：暗紫 / 暗琥珀 */
#define RGB_LOST_R    120u
#define RGB_LOST_G     60u
#define RGB_LOST_B    170u
#define RGB_LOST_W      0u

/** 低电量提示：低亮暖黄 */
#define RGB_HINT_R    255u
#define RGB_HINT_G    200u
#define RGB_HINT_B     80u
#define RGB_HINT_W     40u

/** 恢复连接动画：暖白确认 */
#define RGB_RECOVER_R 255u
#define RGB_RECOVER_G 236u
#define RGB_RECOVER_B 210u
#define RGB_RECOVER_W 255u

/* ------------------------------------------------------------------ */
/* 工具                                                               */
/* ------------------------------------------------------------------ */

static inline uint8_t pct_of_255(uint32_t pct)
{
    if (pct > 100u) {
        pct = 100u;
    }
    /* 四舍五入到最近整数，并把 100% 精确映射到 255 */
    uint32_t v = (pct * 255u + 50u) / 100u;
    if (v > 255u) {
        v = 255u;
    }
    return (uint8_t)v;
}

static inline uint8_t clamp_u8(float v)
{
    if (v <= 0.0f) {
        return 0u;
    }
    if (v >= 255.0f) {
        return 255u;
    }
    return (uint8_t)(v + 0.5f);
}

static inline int color_is_zero(const lamp_color_t *c)
{
    return (c->r == 0u && c->g == 0u && c->b == 0u && c->w == 0u);
}

/** 四通道最大值（判断"主导通道"用） */
static inline uint32_t dominant_value(const lamp_color_t *c)
{
    uint32_t m = c->r;
    if (c->g > m) m = c->g;
    if (c->b > m) m = c->b;
    if (c->w > m) m = c->w;
    return m;
}

/** 四个独立通道值的最大值（用于基色判定，避免先构造 lamp_color_t） */
static inline uint32_t dominant_base(uint32_t r, uint32_t g, uint32_t b, uint32_t w)
{
    uint32_t m = r;
    if (g > m) m = g;
    if (b > m) m = b;
    if (w > m) m = w;
    return m;
}

/** 按百分比缩放整幅颜色（四舍五入，避免连续截断把亮度压低） */
static void scale_color(lamp_color_t *c, uint8_t s)
{
    c->r = (uint8_t)(((uint32_t)c->r * s + 127u) / 255u);
    c->g = (uint8_t)(((uint32_t)c->g * s + 127u) / 255u);
    c->b = (uint8_t)(((uint32_t)c->b * s + 127u) / 255u);
    c->w = (uint8_t)(((uint32_t)c->w * s + 127u) / 255u);
}

/** 把整幅颜色的最大通道压到 max_level 以内，保持色相 */
static void cap_to_level(lamp_color_t *c, uint8_t max_level)
{
    uint8_t peak = c->r;
    if (c->g > peak) peak = c->g;
    if (c->b > peak) peak = c->b;
    if (c->w > peak) peak = c->w;
    if (peak <= max_level || peak == 0u) {
        return;
    }
    const uint32_t s = ((uint32_t)max_level * 255u) / peak;
    c->r = (uint8_t)(((uint32_t)c->r * s) / 255u);
    c->g = (uint8_t)(((uint32_t)c->g * s) / 255u);
    c->b = (uint8_t)(((uint32_t)c->b * s) / 255u);
    c->w = (uint8_t)(((uint32_t)c->w * s) / 255u);
}

/** 状态亮度区间（百分比） */
static void state_brightness_range(pet_lamp_state_t s, uint32_t *lo, uint32_t *hi)
{
    switch (s) {
    case PET_LAMP_QUIET:
        *lo = FX_QUIET_BRIGHTNESS_MIN;  *hi = FX_QUIET_BRIGHTNESS_PCT;  break;
    case PET_LAMP_LIGHT:
        *lo = FX_LIGHT_BRIGHTNESS_MIN;  *hi = FX_LIGHT_BRIGHTNESS_PCT;  break;
    case PET_LAMP_ACTIVE:
        *lo = FX_ACTIVE_BRIGHTNESS_MIN; *hi = FX_ACTIVE_BRIGHTNESS_PCT; break;
    case PET_LAMP_LOST:
        *lo = FX_LOST_BRIGHTNESS_MIN;   *hi = FX_LOST_BRIGHTNESS_PCT;   break;
    case PET_LAMP_BOOT:
    default:
        *lo = FX_BOOT_BRIGHTNESS_MIN;   *hi = FX_BOOT_BRIGHTNESS_PCT;   break;
    }
}

/** 状态过渡时间（毫秒） */
static uint32_t state_transition_ms(pet_lamp_state_t s)
{
    switch (s) {
    case PET_LAMP_QUIET:  return FX_QUIET_TRANSITION_MS;
    case PET_LAMP_LIGHT:  return FX_LIGHT_TRANSITION_MS;
    case PET_LAMP_ACTIVE: return FX_ACTIVE_TRANSITION_MS;
    case PET_LAMP_LOST:   return FX_LOST_TRANSITION_MS;
    case PET_LAMP_BOOT:   return FX_BOOT_TRANSITION_MS;
    default:              return FX_QUIET_TRANSITION_MS;
    }
}

/** 状态基准色 */
static void base_color(pet_lamp_state_t s,
                       uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *w)
{
    switch (s) {
    case PET_LAMP_QUIET:
        *r = RGB_QUIET_R;  *g = RGB_QUIET_G;  *b = RGB_QUIET_B;  *w = RGB_QUIET_W;  break;
    case PET_LAMP_LIGHT:
        *r = RGB_LIGHT_R;  *g = RGB_LIGHT_G;  *b = RGB_LIGHT_B;  *w = RGB_LIGHT_W;  break;
    case PET_LAMP_ACTIVE:
        *r = RGB_ACTIVE_R; *g = RGB_ACTIVE_G; *b = RGB_ACTIVE_B; *w = RGB_ACTIVE_W; break;
    case PET_LAMP_LOST:
        *r = RGB_LOST_R;   *g = RGB_LOST_G;   *b = RGB_LOST_B;   *w = RGB_LOST_W;   break;
    case PET_LAMP_BOOT:
    default:
        *r = RGB_QUIET_R;  *g = RGB_QUIET_G;  *b = RGB_QUIET_B;  *w = RGB_QUIET_W;  break;
    }
}

/** 一阶平滑。
 *
 *  - 逐通道用**局部临时量**计算后写回。不要用 `(const uint8_t*)cur` 与
 *    `(uint8_t*)cur` 两个指针别名读写同一缓冲区：编译器会据 const 假设
 *    判定它们不重叠，从而把循环优化成"每帧都不变"（实测 current 永远停在 1）。
 *  - 用**整数误差**做小误差吸附：浮点截断会让"目标-当前 = 1"的残差
 *    永远收敛不到目标（低亮度下表现为像素被钉在 1 上，呼吸看不见）。
 *  - 目标非 0 时不允许整通道塌陷到 0：Quiet 的 b 通道只有 3/255，
 *    被舍入成 0 后就与"整幅全 0"无法区分，会被误判成熄灯。
 */
static void smooth_towards(lamp_color_q8_t *cur, const lamp_color_t *tgt, float alpha)
{
    if (alpha <= 0.0f) {
        return;
    }
    if (alpha > 1.0f) {
        alpha = 1.0f;
    }
    const uint32_t t[4] = {
        (uint32_t)tgt->r << LAMP_Q8_SHIFT,
        (uint32_t)tgt->g << LAMP_Q8_SHIFT,
        (uint32_t)tgt->b << LAMP_Q8_SHIFT,
        (uint32_t)tgt->w << LAMP_Q8_SHIFT,
    };
    const uint32_t c[4] = { cur->r, cur->g, cur->b, cur->w };
    uint32_t out[4];

    for (int i = 0; i < 4; ++i) {
        if (t[i] == c[i]) {
            out[i] = c[i];
            continue;
        }
        const int32_t err = (int32_t)t[i] - (int32_t)c[i];
        int32_t step = (int32_t)((float)err * alpha);
        if (step == 0) {
            /* 每帧增量不足 1/256 色阶时至少走一格，避免低亮度下永久停滞 */
            step = (err > 0) ? 1 : -1;
        }
        int32_t v = (int32_t)c[i] + step;
        if (err > 0 && v > (int32_t)t[i]) {
            v = (int32_t)t[i];      /* 不越过目标 */
        }
        if (err < 0 && v < (int32_t)t[i]) {
            v = (int32_t)t[i];
        }
        if (v < 0) {
            v = 0;
        }
        if (v > (int32_t)LAMP_Q8_MAX) {
            v = (int32_t)LAMP_Q8_MAX;
        }
        out[i] = (uint32_t)v;
    }

    cur->r = out[0];
    cur->g = out[1];
    cur->b = out[2];
    cur->w = out[3];
}

/* ------------------------------------------------------------------ */
/* 公开 API                                                           */
/* ------------------------------------------------------------------ */

lamp_color_t lamp_effects_target_color(pet_lamp_state_t state, uint8_t score)
{
    lamp_color_t out = { 0u, 0u, 0u, 0u };

    uint8_t br, bg, bb, bw;
    base_color(state, &br, &bg, &bb, &bw);

    uint32_t lo, hi;
    state_brightness_range(state, &lo, &hi);

    const uint32_t span = (hi > lo) ? (hi - lo) : 0u;
    const uint32_t pct = lo + (span * (uint32_t)score) / 255u;

    /*
     * 缩放因子：先用百分比算出基准档位，再检查主导通道在 Gamma 之后
     * 是否有足够的占空比档位来表达呼吸；不足时**整体**放大（保色相）。
     * 详见 LAMP_MIN_DUTY_LEVELS 的说明。
     *
     * ⚠️ 主导通道必须从 **基色**（br/bg/bb/bw）判定，不能从 `out` 判定 ——
     * 这正是本函数曾经的真实缺陷：判定写在给 out 赋值之前，读到全 0，
     * 于是循化不执行、缩放因子永远停在基准值，低亮呼吸被量化吃掉。
     */
    const uint8_t dom = (uint8_t)dominant_base(br, bg, bb, bw);
    uint32_t s = (uint32_t)pct_of_255(pct);
    if (s == 0u) {
        s = 1u;
    }
    if (dom > 0u) {
        /* 白通道用的是 LAMP_WHITE_CHANNEL_MAX，判定时不能混用 RGB 系数 */
        const int dom_is_white = (bw == dom && bw >= br && bw >= bg && bw >= bb);
        const float dom_max = dom_is_white ? LAMP_WHITE_CHANNEL_MAX
                                           : LAMP_RGB_CHANNEL_MAX;
        while (s < 255u) {
            const uint8_t lvl = (uint8_t)(((uint32_t)dom * s + 127u) / 255u);
            if (lamp_gamma_duty(lvl, dom_max) >= LAMP_MIN_DUTY_LEVELS) {
                break;
            }
            ++s;
        }
    }

    out.r = (uint8_t)(((uint32_t)br * s + 127u) / 255u);
    out.g = (uint8_t)(((uint32_t)bg * s + 127u) / 255u);
    out.b = (uint8_t)(((uint32_t)bb * s + 127u) / 255u);
    out.w = (uint8_t)(((uint32_t)bw * s + 127u) / 255u);
    return out;
}

void lamp_effects_set_output(lamp_fx_state_t *fx, lamp_output_fn fn, void *user)
{
    if (fx == NULL) {
        return;
    }
    fx->out_fn = fn;
    fx->out_user = user;
}

void lamp_effects_init(lamp_fx_state_t *fx)
{
    if (fx == NULL) {
        return;
    }
    lamp_output_fn fn = fx->out_fn;
    void *user = fx->out_user;
    memset(fx, 0, sizeof(*fx));
    fx->out_fn = fn;
    fx->out_user = user;
    fx->target = lamp_effects_target_color(PET_LAMP_BOOT, 0);
    fx->next_hint_ms = FX_LOWBAT_HINT_MIN_MS;
}

void lamp_effects_trigger_recover(lamp_fx_state_t *fx, uint32_t now_ms)
{
    if (fx == NULL) {
        return;
    }
    fx->recovering = true;
    fx->recover_until_ms = now_ms + FX_ACTIVE_TRANSITION_MS + FX_RECOVER_EXTRA_MS;
}

const char *lamp_state_name(pet_lamp_state_t s)
{
    switch (s) {
    case PET_LAMP_BOOT:   return "Boot";
    case PET_LAMP_QUIET:  return "Quiet";
    case PET_LAMP_LIGHT:  return "Light";
    case PET_LAMP_ACTIVE: return "Active";
    case PET_LAMP_LOST:   return "Lost";
    default:              return "?";
    }
}

lamp_color_t lamp_manual_to_color(const pet_manual_t *m)
{
    lamp_color_t out = { 0u, 0u, 0u, 0u };
    if (m == NULL || !m->on) {
        return out;
    }

    uint8_t pct = m->brightness_pct;
    if (pct < PET_BRIGHTNESS_MIN) pct = PET_BRIGHTNESS_MIN;
    if (pct > PET_BRIGHTNESS_MAX) pct = PET_BRIGHTNESS_MAX;
    const uint8_t scale = pct_of_255(pct);

    if (m->mode == PET_LIGHT_MODE_COLOR) {
        const uint32_t rgb = m->rgb;
        out.r = (uint8_t)((rgb >> 16) & 0xFFu);
        out.g = (uint8_t)((rgb >> 8) & 0xFFu);
        out.b = (uint8_t)(rgb & 0xFFu);
        out.w = 0u;
        scale_color(&out, scale);
    } else {
        /*
         * Day 模式：用白光通道 + 少量 RGB 做色温偏移。
         * 1700 K → 偏暖（红多蓝少）；6500 K → 偏冷（红少蓝多）。
         * 这是线性近似，实测可用；若需要更准可在平台层查色温表。
         */
        uint32_t k = m->color_temp_k;
        if (k < PET_COLOR_TEMP_MIN) k = PET_COLOR_TEMP_MIN;
        if (k > PET_COLOR_TEMP_MAX) k = PET_COLOR_TEMP_MAX;
        const float t = (float)(k - PET_COLOR_TEMP_MIN) /
                        (float)(PET_COLOR_TEMP_MAX - PET_COLOR_TEMP_MIN);
        /* t=0（最暖）→ (255,170,90)；t=1（最冷）→ (170,205,255) */
        out.r = clamp_u8(255.0f - 85.0f * t);
        out.g = clamp_u8(170.0f + 35.0f * t);
        out.b = clamp_u8(90.0f + 165.0f * t);
        out.w = 255u;
        scale_color(&out, scale);
    }
    return out;
}

/* ------------------------------------------------------------------ */
/* 输出辅助                                                            */
/* ------------------------------------------------------------------ */

/* 把 Q8 平滑状态量化后交给平台。必须走这个宏，避免任何地方直接把
 * Q8 当成 8-bit 颜色传出去（那会把亮度放大 256 倍）。 */
#define FX_EMIT(fx, layer)                                                   \
    do {                                                                     \
        lamp_color_t _c8;                                                    \
        lamp_q8_to_color(&(fx)->current, &_c8);                              \
        if ((fx)->out_fn != NULL) {                                          \
            (fx)->out_fn(&_c8, (layer), 0u, (fx)->out_user);                 \
        }                                                                    \
    } while (0)

/* ------------------------------------------------------------------ */
/* tick                                                               */
/* ------------------------------------------------------------------ */

void lamp_effects_tick(lamp_fx_state_t *fx, const lamp_fx_input_t *in, uint32_t now_ms)
{
    if (fx == NULL || in == NULL) {
        return;
    }

    const pet_lamp_state_t st = in->state;

    /* ---- 1. 手动模式：暂停自动宠物灯效（决策 D10） ------------------ */
    if (in->manual_mode && in->manual != NULL) {
        const lamp_color_t manual_out = lamp_manual_to_color(in->manual);
        fx->target = manual_out;
        /* 手动模式夜间不压制；但熄灭仍要硬切 */
        if (color_is_zero(&manual_out)) {
            lamp_color_to_q8(&manual_out, &fx->current);
            FX_EMIT(fx, 1.0f);
            return;
        }
        smooth_towards(&fx->current, &fx->target,
                       (float)TASK_FX_INTERVAL_MS /
                       ((float)MANUAL_OFF_FADE_MS / 3.0f + (float)TASK_FX_INTERVAL_MS));
        FX_EMIT(fx, 1.0f);
        return;
    }

    /* ---- 2. 自动模式目标值 ----------------------------------------- */
    lamp_color_t tgt = lamp_effects_target_color(st, in->activity_score);

    /* 滑条：只影响全局亮度上限，不改变活动状态颜色规则（需求 7.3） */
    if (in->brightness_limit_pct < 100u) {
        scale_color(&tgt, pct_of_255(in->brightness_limit_pct));
    }

    /* 夜间压制（决策 D8/D14；需求 4.4「不在夜间把 Active 设为高亮」） */
    if (in->brightness_cap < 255u) {
        cap_to_level(&tgt, in->brightness_cap);
    }

    fx->target = tgt;

    /* ---- 3. 失联渐进降亮（需求 4.3：10–30 s 逐步降低当前状态亮度） ----
     *
     * 只作用于**已按状态表设定目标亮度**的那些状态（Quiet/Light/Active）。
     * Lost 的目标亮度本身就已经很低（2%–6%），若再乘 LOST_DIM_END_FACTOR
     * 就是双重压暗：实测把占空比压到个位数 LSB，呼吸波形被量化地板吃掉
     * （峰谷比 1.00）。这是一个真实缺陷，务必不要"顺手"把它加回来。
     */
    float dim_factor = 1.0f;
    if (st != PET_LAMP_LOST && st != PET_LAMP_BOOT &&
        in->lost_age_ms > LOST_DIM_START_MS) {
        if (in->lost_age_ms >= LOST_DIM_END_MS) {
            dim_factor = LOST_DIM_END_FACTOR;
        } else {
            const float t = (float)(in->lost_age_ms - LOST_DIM_START_MS) /
                            (float)(LOST_DIM_END_MS - LOST_DIM_START_MS);
            dim_factor = 1.0f - (1.0f - LOST_DIM_END_FACTOR) * t;
        }
    }

    /* ---- 4. 呼吸 / 浮动（禁止 3 Hz 以上闪烁） ----------------------
     *
     * 这里把正弦映射成 [floor, 1] 的**单极**层系数，而不是 1±amp：
     *   - 谷底不会到 0，所以低亮度下不会出现"一格熄灭"的抽动
     *   - 峰谷比可辨识（Lost 用 floor=0.55，对比明显）
     * 层系数最终在 Gamma **之后**相乘（见 §7），避免被量化吃掉。
     */
    float breath = 0.0f;
    switch (st) {
    case PET_LAMP_QUIET: {
        const float floor_k = FX_BREATH_FLOOR_DEFAULT;
        const float s = sinf((float)now_ms * TWO_PI / (float)FX_QUIET_BREATH_MS);
        breath = (floor_k + (1.0f - floor_k) * (0.5f + 0.5f * s)) - 1.0f;
        break;
    }
    case PET_LAMP_LIGHT: {
        const float floor_k = FX_BREATH_FLOOR_DEFAULT;
        const float s = sinf((float)now_ms * TWO_PI / (float)FX_LIGHT_FLOAT_MS);
        breath = (floor_k + (1.0f - floor_k) * (0.5f + 0.5f * s)) - 1.0f;
        break;
    }
    case PET_LAMP_ACTIVE: {
        const float floor_k = FX_BREATH_FLOOR_DEFAULT;
        const float s = sinf((float)now_ms * TWO_PI / (float)FX_ACTIVE_FLOAT_MS);
        breath = (floor_k + (1.0f - floor_k) * (0.5f + 0.5f * s)) - 1.0f;
        break;
    }
    case PET_LAMP_LOST: {
        uint32_t period = (FX_LOST_BREATH_MIN_MS + FX_LOST_BREATH_MAX_MS) / 2u;
        if (in->night_active) {
            period *= NIGHT_LOST_BREATH_SCALE;   /* 夜间呼吸更慢、更轻 */
        }
        const float floor_k = FX_BREATH_FLOOR_LOST;
        const float s = sinf((float)now_ms * TWO_PI / (float)period);
        breath = (floor_k + (1.0f - floor_k) * (0.5f + 0.5f * s)) - 1.0f;
        break;
    }
    case PET_LAMP_BOOT:
    default:
        break;
    }

    /* ---- 5. 恢复连接确认动画 --------------------------------------- */
    bool recovering = false;
    lamp_color_t recover_color = tgt;
    if (fx->recovering) {
        if ((int32_t)(now_ms - fx->recover_until_ms) >= 0) {
            fx->recovering = false;
        } else {
            recovering = true;
            const uint32_t total = FX_ACTIVE_TRANSITION_MS + FX_RECOVER_EXTRA_MS;
            float t = 1.0f - (float)(fx->recover_until_ms - now_ms) / (float)total;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            /* 前 35% 冲到峰值，再回落 */
            float k = (t < 0.35f) ? (t / 0.35f) : (1.0f - (t - 0.35f) / 0.65f);
            if (k < 0.0f) k = 0.0f;
            const uint8_t lvl = clamp_u8(FX_RECOVER_PEAK * k * 255.0f);
            lamp_color_t rec = { RGB_RECOVER_R, RGB_RECOVER_G,
                                 RGB_RECOVER_B, RGB_RECOVER_W };
            scale_color(&rec, lvl);
            recover_color = rec;
        }
    }

    /* ---- 6. 低电量低频提示（每 5–10 分钟一次，低亮暖黄） ------------ */
    bool hinting = false;
    if (in->low_battery && in->lowbat_hint_allowed) {
        if ((int32_t)(now_ms - fx->next_hint_ms) >= 0 &&
            (int32_t)(now_ms - fx->hint_until_ms) >= 0) {
            fx->hint_until_ms = now_ms + FX_LOWBAT_HINT_DURATION_MS;
            fx->next_hint_ms = now_ms +
                (FX_LOWBAT_HINT_MIN_MS + FX_LOWBAT_HINT_MAX_MS) / 2u;
        }
        hinting = ((int32_t)(now_ms - fx->hint_until_ms) < 0);
    } else {
        fx->next_hint_ms = now_ms +
            (FX_LOWBAT_HINT_MIN_MS + FX_LOWBAT_HINT_MAX_MS) / 2u;
        fx->hint_until_ms = 0u;
    }

    /* ---- 7. 层系数：呼吸 × 失联降亮 --------------------------------- */
    float layer = dim_factor;
    if (breath != 0.0f) {
        layer *= (1.0f + breath);
    }
    if (layer < 0.0f) layer = 0.0f;
    if (layer > 1.0f) layer = 1.0f;

    /* ---- 8. 熄灯硬切（只对"确实是关灯"的两种情形） ----------------- */
    const int commanded_off = (in->manual_off || in->brightness_limit_pct == 0u);
    if (commanded_off) {
        fx->target = (lamp_color_t){ 0u, 0u, 0u, 0u };
        lamp_color_to_q8(&fx->target, &fx->current);
        FX_EMIT(fx, 1.0f);
        return;
    }

    /* ---- 9. 恢复动画优先于常规目标 ---------------------------------- */
    if (recovering) {
        fx->target = recover_color;
        smooth_towards(&fx->current, &fx->target,
                       (float)TASK_FX_INTERVAL_MS /
                       ((float)FX_ACTIVE_TRANSITION_MS / 3.0f + (float)TASK_FX_INTERVAL_MS));
        FX_EMIT(fx, layer);
        return;
    }

    /* ---- 10. 低电量提示叠加 ---------------------------------------- */
    if (hinting) {
        lamp_color_t hint = { RGB_HINT_R, RGB_HINT_G, RGB_HINT_B, RGB_HINT_W };
        const float k = FX_LOWBAT_HINT_BLEND;
        lamp_color_t blended = {
            clamp_u8((float)tgt.r * (1.0f - k) + (float)hint.r * k),
            clamp_u8((float)tgt.g * (1.0f - k) + (float)hint.g * k),
            clamp_u8((float)tgt.b * (1.0f - k) + (float)hint.b * k),
            clamp_u8((float)tgt.w * (1.0f - k) + (float)hint.w * k),
        };
        /* 需求 4.4 要求"低亮度"提示；夜间压制仍要生效但不低于提示所需 */
        cap_to_level(&blended, FX_LOWBAT_HINT_MAX_LEVEL);
        if (in->brightness_cap < 255u) {
            cap_to_level(&blended, in->brightness_cap);
        }
        tgt = blended;
    }

    fx->target = tgt;

    /* ---- 11. 平滑过渡 ---------------------------------------------- */
    if (color_is_zero(&tgt)) {
        lamp_color_to_q8(&tgt, &fx->current);
        FX_EMIT(fx, layer);
        return;
    }

    uint32_t trans_ms = state_transition_ms(st);
    if (trans_ms == 0u) {
        trans_ms = TASK_FX_INTERVAL_MS;
    }
    const float tau = (float)trans_ms / 3.0f;
    const float alpha = (float)TASK_FX_INTERVAL_MS / (tau + (float)TASK_FX_INTERVAL_MS);
    smooth_towards(&fx->current, &tgt, alpha);

    /* ---- 12. 输出 -------------------------------------------------- */
    FX_EMIT(fx, layer);
}
