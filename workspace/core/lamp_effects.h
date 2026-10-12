/*
 * lamp_effects.h — 灯效引擎（平台无关）
 *
 * 需求 4.3 失联判断、4.4 灯效参数、4.5 调光与 Gamma、7.3 手动优先级。
 *
 * 关键设计（来自实际调试结论，见 docs/最终可执行方案.md §4 WP1）：
 *  1. 呼吸 / 失联降亮作为**层系数 layer** 交给平台输出层，在 Gamma **之后**
 *     相乘。若乘在逻辑亮度上，2%–6% 亮度经 Gamma 2.2 压缩后只剩几十个
 *     LSB，量化步长会吃掉整个波形，慢呼吸完全不可见。
 *  2. 熄灭必须硬切到 0；但**不能**因为"某帧算出来是 0"就归零 —— 从黑场
 *     起步时首帧必然取整为 0，那样会把本应点亮的状态永久锁死。
 *     只有"用户主动熄灯"或"亮度上限为 0"才硬切。
 *  3. 平滑时对目标非 0 的通道设下限 1，否则弱通道（如 Quiet 的 b 只有 3/255）
 *     会一直舍入成 0，与"整幅颜色全为 0"无法区分。
 *
 * 本文件不得 include 任何平台头文件。
 */
#ifndef LAMP_EFFECTS_H
#define LAMP_EFFECTS_H

#include "pet_config.h"
#include "pet_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 输入                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    pet_lamp_state_t state;            /**< 合并后的灯端状态 */
    uint8_t  activity_score;           /**< 0–255，状态内亮度插值 */
    uint8_t  brightness_limit_pct;     /**< 滑条：全局亮度上限 0–100 */
    uint8_t  brightness_cap;           /**< 夜间压制上限（逻辑 0–255；255 = 不压制） */
    bool     manual_off;               /**< 用户短期熄灯 */
    bool     manual_mode;              /**< 决策 D10：手动模式 → 暂停自动灯效 */
    bool     low_battery;              /**< Flags.LOW_BATTERY */
    bool     lowbat_hint_allowed;      /**< 夜间可抑制提示 */
    uint64_t lost_age_ms;              /**< 距最后一次有效包的时间 */
    bool     night_active;             /**< 夜间：Lost 呼吸周期放大 */
    const pet_manual_t *manual;        /**< manual_mode 为真时的固定输出；可为 NULL */
} lamp_fx_input_t;

/* ------------------------------------------------------------------ */
/* 状态                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    lamp_color_q8_t current;           /**< 当前输出（Q8 定点：0 .. 255*256） */
    lamp_color_t target;               /**< 目标颜色（常规 8-bit） */
    bool         recovering;           /**< 是否正在执行"恢复连接动画" */
    uint32_t     recover_until_ms;     /**< 恢复动画结束时间 */
    uint32_t     next_hint_ms;         /**< 下一次低电量提示时间 */
    uint32_t     hint_until_ms;        /**< 当前提示结束时间 */
    /* 平台输出 */
    lamp_output_fn out_fn;             /**< 输出回调；NULL 时静默（主机测试可只读 target） */
    void          *out_user;
} lamp_fx_state_t;

/* ------------------------------------------------------------------ */
/* API                                                                */
/* ------------------------------------------------------------------ */

/** 绑定平台输出回调 */
void lamp_effects_set_output(lamp_fx_state_t *fx, lamp_output_fn fn, void *user);

/** 初始化灯效引擎（清状态、目标为 BOOT 暖白） */
void lamp_effects_init(lamp_fx_state_t *fx);

/**
 * 计算给定状态的目标颜色（供单元测试与标定使用）。
 * 不含呼吸、不含夜间压制、不含手动模式。
 *
 * @param score 0–255 活动强度，在状态亮度区间内线性插值
 */
lamp_color_t lamp_effects_target_color(pet_lamp_state_t state, uint8_t score);

/**
 * 主循环 tick，建议 50 Hz（TASK_FX_INTERVAL_MS）。
 *
 * 内部顺序：目标值 → 夜间压制 → 失联降亮 → 呼吸 → 恢复动画 →
 *          低电量提示 → 层系数计算 → 平滑 → 输出回调。
 *
 * @param now_ms 单调毫秒时间戳
 */
void lamp_effects_tick(lamp_fx_state_t *fx, const lamp_fx_input_t *in, uint32_t now_ms);

/** 请求执行"恢复连接动画"（从 LOST 恢复时调用） */
void lamp_effects_trigger_recover(lamp_fx_state_t *fx, uint32_t now_ms);

/** 手动模式下的固定输出换算（网页 / MQTT / 面板都走这里，保证一致） */
lamp_color_t lamp_manual_to_color(const pet_manual_t *m);

/** 状态名（日志用，包含 Boot/Lost） */
const char *lamp_state_name(pet_lamp_state_t s);

#ifdef __cplusplus
}
#endif

#endif /* LAMP_EFFECTS_H */
