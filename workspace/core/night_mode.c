/*
 * night_mode.c — 夜间模式与手动豁免实现
 *
 * 时间处理约定：所有时间戳为单调毫秒（uint32_t，允许回绕），
 * 用无符号差值判断先后，不用绝对值比较。
 */
#include "night_mode.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* 工具                                                               */
/* ------------------------------------------------------------------ */

/** now 是否已到达/超过 deadline（回绕安全） */
static inline bool time_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

/** hour 是否落在 [start, end) 时间窗内（支持跨午夜） */
static bool hour_in_window(int hour, uint8_t start, uint8_t end)
{
    if (hour < 0 || hour > 23) {
        return false;
    }
    const int h = hour;
    const int s = (int)start;
    const int e = (int)end;
    if (s == e) {
        return false;             /* 零长度窗口视为关闭 */
    }
    if (s < e) {
        return (h >= s && h < e);
    }
    /* 跨午夜：例如 22 → 7 */
    return (h >= s || h < e);
}

static uint8_t clamp_cap_u32(uint32_t v)
{
    return (v > 255u) ? (uint8_t)255u : (uint8_t)v;
}

/* ------------------------------------------------------------------ */
/* 配置                                                               */
/* ------------------------------------------------------------------ */

void pet_night_config_defaults(pet_night_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->enabled              = (NIGHT_MODE_ENABLE_DEFAULT != 0);
    cfg->start_hour           = (uint8_t)NIGHT_START_HOUR;
    cfg->end_hour             = (uint8_t)NIGHT_END_HOUR;
    cfg->idle_infer_ms        = NIGHT_IDLE_INFER_MS;
    cfg->override_ms          = MANUAL_OVERRIDE_MS_DEFAULT;
    cfg->cap_quiet            = (uint8_t)NIGHT_CAP_QUIET;
    cfg->cap_light            = (uint8_t)NIGHT_CAP_LIGHT;
    cfg->cap_active           = (uint8_t)NIGHT_CAP_ACTIVE;
    cfg->cap_lost             = (uint8_t)NIGHT_CAP_LOST;
    cfg->active_throttle_ms   = NIGHT_ACTIVE_THROTTLE_MS;
    cfg->suppress_lowbat_hint = (NIGHT_SUPPRESS_LOWBAT_HINT != 0);
}

/* ------------------------------------------------------------------ */
/* 生命周期                                                           */
/* ------------------------------------------------------------------ */

void pet_night_init(pet_night_t *n, const pet_night_config_t *cfg, uint32_t now_ms)
{
    if (n == NULL) {
        return;
    }
    memset(n, 0, sizeof(*n));
    if (cfg != NULL) {
        n->cfg = *cfg;
    } else {
        pet_night_config_defaults(&n->cfg);
    }
    /* 配置钳位，防止 NVS 里存了越界值 */
    if (n->cfg.override_ms < MANUAL_OVERRIDE_MS_MIN) {
        n->cfg.override_ms = MANUAL_OVERRIDE_MS_MIN;
    }
    if (n->cfg.override_ms > MANUAL_OVERRIDE_MS_MAX) {
        n->cfg.override_ms = MANUAL_OVERRIDE_MS_MAX;
    }
    if (n->cfg.active_throttle_ms < NIGHT_ACTIVE_THROTTLE_MIN) {
        n->cfg.active_throttle_ms = NIGHT_ACTIVE_THROTTLE_MIN;
    }
    if (n->cfg.active_throttle_ms > NIGHT_ACTIVE_THROTTLE_MAX) {
        n->cfg.active_throttle_ms = NIGHT_ACTIVE_THROTTLE_MAX;
    }
    if (n->cfg.start_hour > 23u) n->cfg.start_hour = 23u;
    if (n->cfg.end_hour > 23u)   n->cfg.end_hour = 23u;

    n->last_action_ms = now_ms;
    /* 节流计时的初值放到"很久以前"，保证第一次 Active 不被误节流 */
    n->last_active_render_ms = now_ms - n->cfg.active_throttle_ms;
}

/* ------------------------------------------------------------------ */
/* 夜间判定                                                           */
/* ------------------------------------------------------------------ */

bool pet_night_is_night(const pet_night_t *n)
{
    if (n == NULL || !n->cfg.enabled) {
        return false;
    }
    /* 手动模式期间不算夜间（决策 D10：豁免时夜间压制不生效） */
    if (n->manual_mode) {
        return false;
    }
    return (n->window_active || n->idle_inferred || n->forced);
}

/* ------------------------------------------------------------------ */
/* 用户操作                                                           */
/* ------------------------------------------------------------------ */

void pet_night_on_user_action(pet_night_t *n, uint32_t now_ms)
{
    if (n == NULL) {
        return;
    }
    n->last_action_ms = now_ms;
    /* 决策 D9/D10：进入或延长手动模式 */
    if (!n->manual_mode) {
        ++n->override_count;
    }
    n->manual_mode = true;
    n->override_until_ms = now_ms + n->cfg.override_ms;
}

void pet_night_on_state_change(pet_night_t *n, uint32_t now_ms)
{
    if (n == NULL) {
        return;
    }
    /* 只重置空闲推断；**不**进入手动模式（否则狗一动就永久豁免，夜间会一直亮） */
    n->last_action_ms = now_ms;
}

void pet_night_exit_manual(pet_night_t *n, uint32_t now_ms)
{
    if (n == NULL) {
        return;
    }
    n->manual_mode = false;
    n->override_until_ms = 0u;
    n->last_action_ms = now_ms;
}

/* ------------------------------------------------------------------ */
/* tick                                                               */
/* ------------------------------------------------------------------ */

void pet_night_tick(pet_night_t *n, uint32_t now_ms, int hour, bool clock_valid)
{
    if (n == NULL) {
        return;
    }

    /* --- 手动豁免超时回落（决策 D9） ------------------------------- */
    if (n->manual_mode && time_reached(now_ms, n->override_until_ms)) {
        n->manual_mode = false;
        n->override_until_ms = 0u;
    }

    /* --- 时间窗判据 ------------------------------------------------ */
    n->window_active = clock_valid && hour_in_window(hour, n->cfg.start_hour,
                                                           n->cfg.end_hour);

    /* --- 空闲推断（无时钟也能工作） -------------------------------- */
    const uint32_t idle = now_ms - n->last_action_ms;
    n->idle_inferred = (idle >= n->cfg.idle_infer_ms);
}

uint32_t pet_night_override_remaining_ms(const pet_night_t *n, uint32_t now_ms)
{
    if (n == NULL || !n->manual_mode) {
        return 0u;
    }
    if (time_reached(now_ms, n->override_until_ms)) {
        return 0u;
    }
    return n->override_until_ms - now_ms;
}

/* ------------------------------------------------------------------ */
/* 压制与节流                                                          */
/* ------------------------------------------------------------------ */

uint8_t pet_night_cap_for(const pet_night_t *n, pet_lamp_state_t st)
{
    if (n == NULL || !pet_night_is_night(n)) {
        return 255u;   /* 不压制 */
    }
    switch (st) {
    case PET_LAMP_QUIET:  return clamp_cap_u32(n->cfg.cap_quiet);
    case PET_LAMP_LIGHT:  return clamp_cap_u32(n->cfg.cap_light);
    case PET_LAMP_ACTIVE: return clamp_cap_u32(n->cfg.cap_active);
    case PET_LAMP_LOST:   return clamp_cap_u32(n->cfg.cap_lost);
    case PET_LAMP_BOOT:
    default:              return clamp_cap_u32(n->cfg.cap_quiet);
    }
}

bool pet_night_suppress_lowbat(const pet_night_t *n)
{
    if (n == NULL) {
        return false;
    }
    return (pet_night_is_night(n) && n->cfg.suppress_lowbat_hint);
}

bool pet_night_allow_active_render(pet_night_t *n, uint32_t now_ms)
{
    if (n == NULL) {
        return true;
    }
    n->active_throttled = false;
    if (!pet_night_is_night(n)) {
        n->last_active_render_ms = now_ms;
        return true;
    }
    if (time_reached(now_ms, n->last_active_render_ms + n->cfg.active_throttle_ms)) {
        n->last_active_render_ms = now_ms;
        return true;
    }
    n->active_throttled = true;
    ++n->throttled_count;
    return false;
}

const char *pet_night_mode_name(const pet_night_t *n, uint32_t now_ms)
{
    if (n == NULL) {
        return "?";
    }
    if (n->manual_mode) {
        return "Manual";
    }
    if (!n->cfg.enabled) {
        return "Off";
    }
    if (pet_night_is_night(n)) {
        return "Night";
    }
    (void)now_ms;
    return "Day";
}
