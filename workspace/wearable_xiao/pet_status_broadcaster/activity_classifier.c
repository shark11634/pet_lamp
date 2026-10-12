/*
 * activity_classifier.c — 活动状态分类实现
 */
#include "activity_classifier.h"

#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------ */

void cls_init(activity_classifier_t *c, uint32_t now_ms)
{
    if (c == NULL) {
        return;
    }
    memset(c, 0, sizeof(*c));
    c->state = PET_STATE_QUIET;
    c->candidate = PET_STATE_QUIET;
    c->candidate_since_ms = now_ms;
    c->last_sample_ms = now_ms;
    c->baseline_mg = 1000.0f;   /* 1 g 作为初始重力基线 */
}

void cls_force_state(activity_classifier_t *c, pet_activity_state_t s, uint32_t now_ms)
{
    if (c == NULL) {
        return;
    }
    c->state = s;
    c->candidate = s;
    c->candidate_since_ms = now_ms;
}

/* ------------------------------------------------------------------ */

/** 把 mean/peak/ratio 映射到 0–255 的活动强度分 */
static uint8_t compute_score(float mean_mg, float peak_mg, float ratio)
{
    /* 均值项：0 对应 0，2 * T_ACTIVE 对应满值 */
    float a = mean_mg / (2.0f * CLS_T_ACTIVE_MG);
    /* 峰值项 */
    float b = peak_mg / (2.0f * CLS_T_ACTIVE_PEAK_MG);
    /* 超阈值比例项 */
    float d = ratio;

    if (a > 1.0f) a = 1.0f;
    if (b > 1.0f) b = 1.0f;
    if (d > 1.0f) d = 1.0f;

    /* 加权：均值 50%，峰值 25%，比例 25% */
    float s = 0.50f * a + 0.25f * b + 0.25f * d;
    if (s < 0.0f) s = 0.0f;
    if (s > 1.0f) s = 1.0f;
    return (uint8_t)(s * 255.0f + 0.5f);
}

/** 根据窗口统计量给出"观察到的候选状态" */
static pet_activity_state_t observe_state(const activity_classifier_t *c)
{
    const float mean = c->mean_dyn_mg;
    const float peak = c->peak_dyn_mg;
    const float ratio = c->over_ratio;

    /* Active：任一指标达标即可 */
    if (mean >= CLS_T_ACTIVE_MG ||
        peak >= CLS_T_ACTIVE_PEAK_MG ||
        ratio >= CLS_T_ACTIVE_RATIO) {
        return PET_STATE_ACTIVE;
    }

    /* Quiet：均值低且峰值低 */
    if (mean < CLS_T_QUIET_MG && peak < CLS_T_QUIET_PEAK_MG) {
        return PET_STATE_QUIET;
    }

    /* 其余为 Light */
    return PET_STATE_LIGHT;
}

/** 计算从当前状态切到 candidate 所需的保持时间 */
static uint32_t required_hold_ms(pet_activity_state_t from, pet_activity_state_t to)
{
    if (to == PET_STATE_ACTIVE) {
        /* 升级到 Active 要快：需求 3.8 要求 5 秒内反映 */
        return CLS_ACTIVE_ENTER_MS;
    }
    if (from == PET_STATE_ACTIVE) {
        /* 从 Active 降级用更长退出时间（防抖建议） */
        return CLS_EXIT_FROM_ACTIVE_MS;
    }
    if (to == PET_STATE_QUIET) {
        return CLS_QUIET_ENTER_MS;
    }
    /* 到 Light：Light 是最"中性"的状态，进入要相对快 */
    return CLS_LIGHT_ENTER_MS;
}

pet_activity_state_t cls_update(activity_classifier_t *c,
                                float ax_mg, float ay_mg, float az_mg,
                                uint32_t now_ms)
{
    if (c == NULL) {
        return PET_STATE_QUIET;
    }

    c->last_sample_ms = now_ms;

    /* 1. 向量模长 */
    const float mag = sqrtf(ax_mg * ax_mg + ay_mg * ay_mg + az_mg * az_mg);

    /* 2. 重力基线（低通）——前若干个样本直接建立基线，避免启动瞬态 */
    if (c->baseline_samples < 8u) {
        c->baseline_mg = mag;
        ++c->baseline_samples;
    } else {
        c->baseline_mg += CLS_BASELINE_ALPHA * (mag - c->baseline_mg);
    }

    /* 3. 动态加速度 */
    float dyn = mag - c->baseline_mg;
    if (dyn < 0.0f) {
        dyn = -dyn;
    }

    /* 4. 窗口统计 */
    c->sum_dyn += dyn;
    if (dyn > c->peak_dyn) {
        c->peak_dyn = dyn;
    }
    if (dyn >= CLS_T_SAMPLE_MG) {
        ++c->over_count;
    }
    ++c->count;

    if (c->count < CLS_WINDOW_SAMPLES) {
        return c->state;
    }

    c->mean_dyn_mg = c->sum_dyn / (float)c->count;
    c->peak_dyn_mg = c->peak_dyn;
    c->over_ratio = (float)c->over_count / (float)c->count;
    c->score = compute_score(c->mean_dyn_mg, c->peak_dyn_mg, c->over_ratio);

    /* 重置窗口 */
    c->sum_dyn = 0.0f;
    c->peak_dyn = 0.0f;
    c->over_count = 0;
    c->count = 0;

    /* 5. 状态判定（带保持时间滞回） */
    pet_activity_state_t observed = observe_state(c);

    if (observed == c->state) {
        c->candidate = observed;
        c->candidate_since_ms = now_ms;
        return c->state;
    }

    if (observed != c->candidate) {
        c->candidate = observed;
        c->candidate_since_ms = now_ms;
        return c->state;
    }

    uint32_t hold = required_hold_ms(c->state, c->candidate);
    /* 直接升级到 Active 时允许更快的通道 */
    if (c->candidate == PET_STATE_ACTIVE && hold > CLS_UPGRADE_FAST_MS) {
        hold = CLS_ACTIVE_ENTER_MS;
    }

    if ((uint32_t)(now_ms - c->candidate_since_ms) >= hold) {
        c->state = c->candidate;
    }
    return c->state;
}

int cls_sensor_error(const activity_classifier_t *c, uint32_t now_ms)
{
    if (c == NULL) {
        return 1;
    }
    /* 超过 1 秒没有任何样本 => 采集任务异常 */
    return ((uint32_t)(now_ms - c->last_sample_ms) > 1000u) ? 1 : 0;
}
