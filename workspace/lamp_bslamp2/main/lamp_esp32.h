/*
 * lamp_esp32.h — core/lamp_driver 与 ESP32 LEDC/GPIO 之间的适配层
 *
 * 职责边界：
 *   core/lamp_driver.c   逻辑颜色 → 4 通道占空比（Gamma 2.2、最小稳定亮度、
 *                        全通道限流、层调制）—— 已被 300 项主机断言覆盖
 *   本文件             占空比 → LEDC 寄存器 + 主开关 GPIO
 *
 * 这里**不再有任何 Gamma 或限流数学**。以前 lamp_bslamp2/main/lamp_driver.c
 * 自带一份，会与 core/ 漂移；现在统一走 core。
 */
#ifndef LAMP_ESP32_H
#define LAMP_ESP32_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "lamp_driver.h"   /* core：lamp_driver_t / lamp_platform_t */

#ifdef __cplusplus
extern "C" {
#endif

/** LEDC 通道顺序，与 core 的 lamp_color_t 字段顺序（R,G,B,W）一致 */
extern const uint8_t lamp_esp32_gpio[LAMP_CH_COUNT];

/**
 * 初始化 LEDC 与主开关 GPIO，并把 core 的 lamp_driver_t 绑定到本层。
 *
 * 必须先拉高主开关 1/2，否则灯光电路不导通
 * （需求 4.2：不要在未验证电路的前提下直接写最小 PWM 程序）。
 *
 * @param drv 由 core 管理的驱动实例，函数会在其中填入平台回调
 */
esp_err_t lamp_esp32_init(lamp_driver_t *drv);

/** 主开关单独控制（上电顺序、维护模式用） */
esp_err_t lamp_esp32_power_enable(bool enable);

/** 读回某通道最近一次写入的占空比（诊断用） */
uint16_t lamp_esp32_last_duty(int channel);

#ifdef __cplusplus
}
#endif

#endif /* LAMP_ESP32_H */
