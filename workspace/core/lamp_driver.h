/*
 * lamp_driver.h — 逻辑颜色 → PWM 占空比：Gamma 校正、全通道限流、层调制
 *                （平台无关）
 *
 * 需求 4.5：
 *   - 亮度由线性逻辑亮度换算成 Gamma 校正后的 PWM 占空比，建议 gamma = 2.2
 *   - 低亮不稳定时提高最小稳定亮度，而不是无限压低 PWM
 *   - RGB 与白光可分别采用不同的 Gamma 或最大通道系数
 *   - 需要全通道限流，避免 RGB + 白光同时满功率的热量与电流异常
 *
 * 平台交互只有一个函数指针：把 4 个通道的原始占空比写进硬件。
 * 这样 ESP-IDF（LEDC）、ESPHome（light output）、主机测试（记录数组）
 * 可以共用完全相同的 Gamma / 限流 / 层调制数学。
 */
#ifndef LAMP_DRIVER_H
#define LAMP_DRIVER_H

#include "lamp_gamma.h"
#include "pet_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 通道顺序，与 duty[4] 一致 */
enum {
    LAMP_CH_R = 0,
    LAMP_CH_G = 1,
    LAMP_CH_B = 2,
    LAMP_CH_W = 3,
    LAMP_CH_COUNT = 4,
};

/**
 * 平台输出：把 4 个通道的占空比（0..LAMP_PWM_MAX_DUTY）写入硬件。
 * @param duty 长度 LAMP_CH_COUNT 的数组
 */
typedef void (*lamp_pwm_write_fn)(const uint16_t *duty, void *user);

typedef struct {
    lamp_pwm_write_fn write;   /**< NULL 时只做计算不输出 */
    void             *user;
} lamp_platform_t;

/* ------------------------------------------------------------------ */
/* 纯函数（无状态，供测试与标定直接调用）                              */
/* ------------------------------------------------------------------ */

/* lamp_gamma_duty() 声明在 lamp_gamma.h（平台无关的 Gamma 曲线，
 * 由灯效层与驱动层共用），本头文件已包含它。 */

/**
 * 应用全通道限流（就地修改）。四通道逻辑亮度和不超过
 * LAMP_GLOBAL_CURRENT_LIMIT × 4 × 255。
 */
void lamp_apply_current_limit(lamp_color_t *c);

/**
 * 逻辑颜色 → 4 通道应输出的占空比（含 Gamma、限流、层调制）。
 *
 * @param layer      0.0–1.0 的层系数（呼吸 / 失联降亮），在 Gamma **之后**相乘
 * @param floor_duty 保留参数，当前实现忽略（详见 lamp_driver.c 的说明）
 */
void lamp_compute_duty(const lamp_color_t *c, float layer, uint16_t floor_duty,
                       uint16_t duty[LAMP_CH_COUNT]);

/* ------------------------------------------------------------------ */
/* 有状态封装（平台绑定后使用）                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    lamp_platform_t platform;
    uint16_t        duty[LAMP_CH_COUNT];
    bool            inited;
} lamp_driver_t;

/** 绑定平台并初始化（清 LUT 缓存） */
void lamp_driver_init(lamp_driver_t *d, const lamp_platform_t *platform);

/** 立即生效输出（layer = 1.0，无 floor） */
void lamp_driver_set(lamp_driver_t *d, const lamp_color_t *c);

/** 带层调制的输出；这也是 lamp_output_fn 可直接使用的形态 */
void lamp_driver_set_layer(lamp_driver_t *d, const lamp_color_t *c,
                           float layer, uint16_t floor_duty);

/** 读回当前占空比（调试 / 诊断用） */
void lamp_driver_get_duty(const lamp_driver_t *d, uint16_t out[LAMP_CH_COUNT]);

/** 完全熄灭（layer = 0） */
void lamp_driver_off(lamp_driver_t *d);

#ifdef __cplusplus
}
#endif

#endif /* LAMP_DRIVER_H */
