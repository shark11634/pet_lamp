/*
 * night_mode.h — 夜间模式与手动豁免（平台无关）
 *
 * 需求依据与决策：
 *   - 需求 4.4：「不做 3 Hz 以上的可见闪烁」「不在夜间把 Active 设为 100% 高亮」
 *     「不把 Lost 设计成持续红色爆闪 —— 失联可能只是宠物走远、设备没电或灯刚断电重启」
 *   - 需求 12.4：「先保证低亮舒适。这是桌面氛围灯，不是报警器；夜间效果比高亮彩色更重要」
 *   - 决策 D8 ：时间窗 22:00–07:00 + 无时钟空闲推断 + App/网页开关
 *   - 决策 D9 ：手动豁免 10 分钟可配（1–120 分钟）
 *   - 决策 D10：豁免期间暂停自动宠物灯效、夜间压制不生效；超时回落自动模式
 *   - 决策 D14：夜间 Active 上限 18（≈7% 逻辑亮度）
 *   - 决策 O6 ：豁免期间默认不用极暗提示状态变化
 *
 * 双判据设计（不依赖网络时钟也能工作）：
 *   1. 有时间源时：按 hour ∈ [start, end) 判断（跨午夜时按 [start,24) ∪ [0,end)）
 *   2. 无时间源时：连续 NIGHT_IDLE_INFER_MS 无用户操作且无状态变化 → 视为夜间
 *   两者取或：只要任一成立就进夜间模式，避免"没网就整夜全亮"。
 */
#ifndef NIGHT_MODE_H
#define NIGHT_MODE_H

#include "pet_config.h"
#include "pet_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 配置（可由网页 / YAML 覆盖并写入 NVS）                              */
/* ------------------------------------------------------------------ */

typedef struct {
    bool     enabled;              /**< 夜间模式总开关 */
    uint8_t  start_hour;           /**< 时间窗起点 0–23 */
    uint8_t  end_hour;             /**< 时间窗终点 0–23（不含） */
    uint32_t idle_infer_ms;        /**< 无时钟空闲推断阈值 */
    uint32_t override_ms;          /**< 手动豁免时长（决策 D9） */
    uint8_t  cap_quiet;            /**< 夜间亮度上限（逻辑 0–255） */
    uint8_t  cap_light;
    uint8_t  cap_active;
    uint8_t  cap_lost;
    uint32_t active_throttle_ms;   /**< Active 夜间响应节流 */
    bool     suppress_lowbat_hint; /**< 夜间抑制低电量提示 */
} pet_night_config_t;

/** 用 pet_config.h 的默认值填充配置 */
void pet_night_config_defaults(pet_night_config_t *cfg);

/* ------------------------------------------------------------------ */
/* 运行时状态                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    pet_night_config_t cfg;

    /* 用户交互 */
    bool     manual_mode;          /**< 决策 D10：是否处于手动模式 */
    uint32_t override_until_ms;    /**< 手动模式到期时刻 */
    uint32_t last_action_ms;       /**< 最近一次用户操作 */

    /* 夜间判定 */
    bool     window_active;        /**< 时间窗内 */
    bool     idle_inferred;        /**< 空闲推断成立 */
    bool     forced;               /**< 用户手动强制（开关）而不看时间 */

    /* 节流 */
    uint32_t last_active_render_ms;
    bool     active_throttled;     /**< 本次 tick 是否因节流被抑制 */

    /* 统计 */
    uint32_t override_count;       /**< 进入手动模式次数 */
    uint32_t throttled_count;      /**< 被节流次数 */
} pet_night_t;

/* ------------------------------------------------------------------ */
/* API                                                                */
/* ------------------------------------------------------------------ */

/** 初始化（载入配置；cfg 为 NULL 时用默认值） */
void pet_night_init(pet_night_t *n, const pet_night_config_t *cfg, uint32_t now_ms);

/** 是否处于夜间模式（综合时间窗 + 空闲推断 + 用户强制开关） */
bool pet_night_is_night(const pet_night_t *n);

/**
 * 用户主动操作时调用（面板按键、滑条、网页、MQTT）。
 *
 * 决策 D9/D10：进入手动模式并刷新豁免计时；重复调用会延长豁免。
 * 这是"手动豁免"的唯一入口，所有用户操作都必须走这里。
 */
void pet_night_on_user_action(pet_night_t *n, uint32_t now_ms);

/** 宠物状态发生显著变化时调用（仅用于空闲推断重置，不进入手动模式） */
void pet_night_on_state_change(pet_night_t *n, uint32_t now_ms);

/** 直接退出手动模式（例如网页点"恢复自动"） */
void pet_night_exit_manual(pet_night_t *n, uint32_t now_ms);

/**
 * 周期 tick（建议 1 Hz）。
 *
 * @param hour        当前小时 0–23；无时钟时忽略
 * @param clock_valid 时钟是否可信（NTP/HA 已同步）
 */
void pet_night_tick(pet_night_t *n, uint32_t now_ms, int hour, bool clock_valid);

/** 手动模式剩余毫秒；不在手动模式返回 0 */
uint32_t pet_night_override_remaining_ms(const pet_night_t *n, uint32_t now_ms);

/**
 * 给定状态的夜间亮度上限（逻辑 0–255）。
 * 非夜间模式或手动模式时返回 255（不压制）。
 */
uint8_t pet_night_cap_for(const pet_night_t *n, pet_lamp_state_t st);

/** 夜间是否抑制低电量提示 */
bool pet_night_suppress_lowbat(const pet_night_t *n);

/**
 * Active 状态的夜间响应节流：每分钟最多放行一次。
 *
 * @return true 允许本次渲染；false 表示应保持上一次的渲染结果
 */
bool pet_night_allow_active_render(pet_night_t *n, uint32_t now_ms);

/** 夜间模式名（日志用） */
const char *pet_night_mode_name(const pet_night_t *n, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* NIGHT_MODE_H */
