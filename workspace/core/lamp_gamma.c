/*
 * lamp_gamma.c — Gamma 2.2 曲线（平台无关，驱动层与灯效层共用）
 *
 * 为什么单独一个文件：Gamma 是**纯函数**，没有平台依赖，但既被
 * lamp_driver.c（占空比换算）用，也被测试与分析工具用。
 * 把它从驱动里拆出来之后：
 *   · 想验证曲线的人不必链接整个驱动
 *   · ESPHome 组件在换成硬件 LEDC 之后仍能复用同一条曲线
 *
 * 查表动机：灯效 50 Hz、每帧 4 个通道，逐帧做 powf 不划算；
 * 查表一次构建、之后只做乘法。
 */
#include "lamp_gamma.h"

#include <math.h>
#include <stdbool.h>

/* 逻辑亮度 0–255 → 占空比（LAMP_PWM_MAX_DUTY 尺度） */
static uint16_t s_gamma_lut[256];
static bool     s_gamma_ready;

static void gamma_lut_build(void)
{
    if (s_gamma_ready) {
        return;
    }
    for (int i = 0; i < 256; ++i) {
        if (i == 0) {
            s_gamma_lut[i] = 0u;
            continue;
        }
        const float lin = (float)i / 255.0f;
        const float corr = powf(lin, LAMP_GAMMA);
        float duty = corr * (float)LAMP_PWM_MAX_DUTY;
        if (duty < 1.0f) {
            duty = 1.0f;   /* 非零逻辑亮度不落在 0 占空比上 */
        }
        s_gamma_lut[i] = (uint16_t)(duty + 0.5f);
    }
    s_gamma_ready = true;
}

uint16_t lamp_gamma_duty(uint8_t logical_level, float channel_max)
{
    gamma_lut_build();

    uint8_t lvl = logical_level;
    if (lvl != 0u && lvl < LAMP_MIN_STABLE_LEVEL) {
        lvl = (uint8_t)LAMP_MIN_STABLE_LEVEL;   /* 需求 4.5：提高最小稳定亮度 */
    }
    float duty = (float)s_gamma_lut[lvl] * channel_max;
    if (duty > (float)LAMP_PWM_MAX_DUTY) {
        duty = (float)LAMP_PWM_MAX_DUTY;
    }
    if (duty < 0.0f) {
        duty = 0.0f;
    }
    return (uint16_t)(duty + 0.5f);
}

uint16_t lamp_duty_to_bits(uint16_t duty, uint8_t result_bits)
{
    if (result_bits == 0u || result_bits >= LAMP_PWM_RES_BITS) {
        return duty;      /* 平台位数足够：不换算，保留全部精度 */
    }
    if (duty == 0u) {
        return 0u;
    }
    const uint32_t max = (1u << result_bits) - 1u;
    uint32_t scaled = ((uint32_t)duty * max + (LAMP_PWM_MAX_DUTY / 2u)) /
                      LAMP_PWM_MAX_DUTY;
    /* 非零占空比不得被缩放成 0：否则低亮档会直接熄灯
     * （这与 LAMP_MIN_STABLE_LEVEL 是同一类保护） */
    if (scaled == 0u) {
        scaled = 1u;
    }
    if (scaled > max) {
        scaled = max;
    }
    return (uint16_t)scaled;
}
