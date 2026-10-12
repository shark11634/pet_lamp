/*
 * lamp_gamma.h — Gamma 曲线（平台无关，被 lamp_effects 与 lamp_driver 共用）
 *
 * 单独抽出是为了避免 lamp_effects.c 依赖 lamp_driver.h（那会造成
 * 灯效层依赖驱动层，违反"核心库只依赖配置与类型"的分层）。
 */
#ifndef LAMP_GAMMA_H
#define LAMP_GAMMA_H

#include <stdint.h>

#include "pet_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 逻辑亮度（0–255）→ Gamma 校正后的占空比（0..LAMP_PWM_MAX_DUTY）。
 *
 * - 非零逻辑亮度会施加 LAMP_MIN_STABLE_LEVEL 下限，因此**不会**返回 0
 * - 完全熄灭必须走 layer = 0 的输出路径，而不是依赖本函数
 * - 结果按 channel_max 缩放（RGB 与白光可用不同系数，需求 4.5）
 */
uint16_t lamp_gamma_duty(uint8_t logical_level, float channel_max);

/**
 * 把 core 计算的占空比（LAMP_PWM_MAX_DUTY 尺度，默认 14 bit）换算到
 * 平台实际支持的占空比尺度。
 *
 * 为什么需要它：core 的 Gamma 表按 LAMP_PWM_RES_BITS 设计（低亮呼吸需要
 * 足够的量化档位）。但某些平台只能用较低位数 —— 典型例子是 Arduino 的
 * `analogWrite()` 只有 8 bit。此时必须按比例缩放，并且保证：
 *   · result_bits 为 0 或 >= LAMP_PWM_RES_BITS 时原样返回（不损失精度）
 *   · 非零占空比缩放后不得变成 0（否则低亮档直接熄灯）
 *
 * @param duty        core 算出的占空比（0..LAMP_PWM_MAX_DUTY）
 * @param result_bits 目标位数（1..LAMP_PWM_RES_BITS）；0 表示不换算
 */
uint16_t lamp_duty_to_bits(uint16_t duty, uint8_t result_bits);

#ifdef __cplusplus
}
#endif

#endif /* LAMP_GAMMA_H */
