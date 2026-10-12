/*
 * activity_classifier.h — 活动状态分类（Quiet / Light / Active）
 * 需求 3.4 节：运动状态定义、推荐计算量、建议初始分类规则、防抖建议。
 *
 * 算法（与需求一一对应）：
 *   1. 对每个加速度样本计算向量模长        |a| = sqrt(ax²+ay²+az²)
 *   2. 用低通估计重力基线                  baseline += alpha * (|a| - baseline)
 *   3. 动态加速度                          dyn = | |a| - baseline |
 *   4. 每 2–5 秒窗口（默认 3 秒 @100 ms 采样）计算：
 *        - 平均动态加速度 mean
 *        - 峰值动态加速度 peak
 *        - 超过活动阈值的样本比例 ratio
 *   5. 规则（带滞回 + 最短保持时间，避免频繁跳变）：
 *        Quiet : mean < T_quiet 且 peak 低        连续 10–20 s 才进入
 *        Light : 超过低阈值但未达高阈值            连续 3–5 s 才进入
 *        Active: mean / peak / ratio 达 T_active   连续 2–3 s 即可进入
 *        从 Active 降级采用**更长的退出时间**
 *   6. 阈值全部为可配置常量，需通过真实宠物测试校准；
 *      V1 的目标是"观感上合理"，不是精确测量卡路里。
 */
#ifndef ACTIVITY_CLASSIFIER_H
#define ACTIVITY_CLASSIFIER_H

#include <stdint.h>

#include "pet_protocol.h"

/* ------------------------------------------------------------------ */
/* 可配置阈值（单位：mg，1 g = 1000 mg）                              */
/* 这些值必须通过真实宠物测试校准，不要写死在产品逻辑中。              */
/* ------------------------------------------------------------------ */

/** 重力基线低通系数（采样周期 100 ms；越大跟随越快） */
#define CLS_BASELINE_ALPHA        0.02f

/** 动态加速度均值低于此值视为"几乎不动" */
#define CLS_T_QUIET_MG            45.0f
/** Quiet 判定允许的峰值上限 */
#define CLS_T_QUIET_PEAK_MG       120.0f
/** 动态加速度均值超过此值视为活跃 */
#define CLS_T_ACTIVE_MG           260.0f
/** 峰值动态加速度超过此值也可判定活跃 */
#define CLS_T_ACTIVE_PEAK_MG      700.0f
/** 超阈值样本比例超过此值也可判定活跃 */
#define CLS_T_ACTIVE_RATIO        0.25f
/** 统计"超阈值样本比例"所用的阈值 */
#define CLS_T_SAMPLE_MG           180.0f

/** 窗口长度：2–5 秒，默认 3 秒 @100 ms */
#define CLS_WINDOW_SAMPLES        30

/** 最短保持时间（毫秒）——防抖 */
#define CLS_QUIET_ENTER_MS        15000u   /* 连续 10–20 s 才进入 Quiet */
#define CLS_LIGHT_ENTER_MS         4000u   /* 连续 3–5 s 才进入 Light */
#define CLS_ACTIVE_ENTER_MS        2500u   /* 连续 2–3 s 即可进入 Active */
/** 从 Active 降级采用更长退出时间 */
#define CLS_EXIT_FROM_ACTIVE_MS    6000u
/** 最快的升级路径：Active 直接进入所需时间 */
#define CLS_UPGRADE_FAST_MS        1200u

/* ------------------------------------------------------------------ */

typedef struct {
    /* 累计器 */
    float    sum_dyn;
    float    peak_dyn;
    uint16_t over_count;
    uint16_t count;

    /* 重力基线 */
    float    baseline_mg;
    uint8_t  baseline_samples;

    /* 输出 */
    pet_activity_state_t state;
    uint8_t              score;          /**< 0–255 细粒度活动强度 */
    float                mean_dyn_mg;
    float                peak_dyn_mg;
    float                over_ratio;

    /* 候选状态与保持计时 */
    pet_activity_state_t candidate;
    uint32_t             candidate_since_ms;
    uint32_t             last_sample_ms;
} activity_classifier_t;

/** 初始化（首次调用需要 now_ms 用于计时基准） */
void cls_init(activity_classifier_t *c, uint32_t now_ms);

/**
 * 送入一个加速度样本。
 *
 * @param ax_mg ay_mg az_mg 三轴加速度（mg）
 * @param now_ms            单调毫秒时间戳
 * @return 当前分类状态
 */
pet_activity_state_t cls_update(activity_classifier_t *c,
                                float ax_mg, float ay_mg, float az_mg,
                                uint32_t now_ms);

/** 立即发布一个状态（例如启动时强制 Active，用于配对广播） */
void cls_force_state(activity_classifier_t *c, pet_activity_state_t s, uint32_t now_ms);

/** 是否检测到传感器异常（连续多窗无样本） */
int cls_sensor_error(const activity_classifier_t *c, uint32_t now_ms);

#endif /* ACTIVITY_CLASSIFIER_H */
