/*
 * test/mock_driver.h — 测试用的「直通」core 驱动替身（声明）
 *
 * 为什么需要它：
 *   host_integration_test 要验证的是 **core → lamp_esp32.c → LEDC** 这条接线。
 *   如果测试自己的 lamp_effects 输出到一个**独立的** lamp_driver_t 实例，
 *   而适配层写的是**它自己的**实例，那么 LEDC 替身永远是 0 —— 测试也就
 *   什么都没验证到（这个错误本工程真的犯过一次，见 host_integration_test.c
 *   的说明）。
 *
 *   本替身把 lamp_driver_set_layer() 直接接到 lamp_compute_duty()，不保留
 *   任何内部状态：
 *     core 的输出回调 → 同一次调用里就把占空比推进 LEDC 替身
 *   接线正确与否立刻可见，且测试不需要知道适配层内部持有哪个实例。
 *
 * 实现放在 mock_driver.c。构建时**不要**编译 core/lamp_driver.c，
 * 否则会与这些符号重定义（Gamma 曲线本身在头文件里的 lamp_gamma.h，
 * 因此仍然可用，无需链接 lamp_driver.c）。
 *
 * 它不替代 core/lamp_driver.c 的单元测试 —— Gamma、限流、floor 语义仍然由
 * test/host_effects_test.c 对着真实实现验证。
 */
#ifndef TEST_MOCK_DRIVER_H
#define TEST_MOCK_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

#include "lamp_driver.h"
#include "pet_config.h"
#include "pet_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 绑定真实写函数（通常是适配层注册的 platform.write） */
void mock_driver_bind(lamp_pwm_write_fn write, void *user);

/** 让替身停止产生输出（模拟"适配层未就绪"） */
void mock_driver_release(void);

/** 最近一次由替身算出的占空比 */
const uint16_t *mock_driver_last_duty(void);

/* ---- 与 core/lamp_driver.h 同名，链接时替换真实实现 ---- */
void lamp_driver_init(lamp_driver_t *d, const lamp_platform_t *platform);
void lamp_driver_set(lamp_driver_t *d, const lamp_color_t *c);
void lamp_driver_set_layer(lamp_driver_t *d, const lamp_color_t *c,
                           float layer, uint16_t floor_duty);

#ifdef __cplusplus
}
#endif

#endif /* TEST_MOCK_DRIVER_H */
