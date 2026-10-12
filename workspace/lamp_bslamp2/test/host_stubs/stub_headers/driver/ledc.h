/*
 * host_stubs/stub_headers/driver/ledc.h — 主机测试替身
 *
 * 只提供 lamp_driver.c 用到的部分。占空比被记录到 host_ledc_duty()，
 * 便于断言"逻辑亮度 → Gamma 占空比"的真实曲线。
 */
#ifndef STUB_DRIVER_LEDC_H
#define STUB_DRIVER_LEDC_H

#include <stdint.h>

#include "esp_err.h"

typedef enum {
    LEDC_LOW_SPEED_MODE = 0,
    LEDC_HIGH_SPEED_MODE,
    LEDC_SPEED_MODE_MAX,
} ledc_mode_t;

typedef enum {
    LEDC_TIMER_0 = 0,
    LEDC_TIMER_1,
    LEDC_TIMER_2,
    LEDC_TIMER_3,
    LEDC_TIMER_MAX,
} ledc_timer_t;

typedef enum {
    LEDC_CHANNEL_0 = 0,
    LEDC_CHANNEL_1,
    LEDC_CHANNEL_2,
    LEDC_CHANNEL_3,
    LEDC_CHANNEL_4,
    LEDC_CHANNEL_5,
    LEDC_CHANNEL_6,
    LEDC_CHANNEL_7,
    LEDC_CHANNEL_MAX,
} ledc_channel_t;

typedef enum {
    LEDC_TIMER_1_BIT = 1,
    LEDC_TIMER_8_BIT = 8,
    LEDC_TIMER_10_BIT = 10,
    LEDC_TIMER_12_BIT = 12,
    LEDC_TIMER_13_BIT = 13,
    LEDC_TIMER_14_BIT = 14,
    LEDC_TIMER_15_BIT = 15,
} ledc_timer_bit_t;

typedef enum {
    LEDC_AUTO_CLK = 0,
    LEDC_USE_APB_CLK,
    LEDC_USE_REF_TICK,
    LEDC_USE_RC_FAST_CLK,
    LEDC_USE_XTAL_CLK,
} ledc_clk_cfg_t;

typedef enum {
    LEDC_INTR_DISABLE = 0,
    LEDC_INTR_FADE_END,
    LEDC_INTR_MAX,
} ledc_intr_type_t;

typedef struct {
    ledc_mode_t speed_mode;
    ledc_timer_bit_t duty_resolution;
    ledc_timer_t timer_num;
    uint32_t freq_hz;
    ledc_clk_cfg_t clk_cfg;
} ledc_timer_config_t;

typedef struct {
    int gpio_num;
    ledc_mode_t speed_mode;
    ledc_channel_t channel;
    ledc_intr_type_t intr_type;
    ledc_timer_t timer_sel;
    uint32_t duty;
    int hpoint;
} ledc_channel_config_t;

esp_err_t ledc_timer_config(const ledc_timer_config_t *cfg);
esp_err_t ledc_channel_config(const ledc_channel_config_t *cfg);
esp_err_t ledc_set_duty(ledc_mode_t speed_mode, ledc_channel_t channel, uint32_t duty);
esp_err_t ledc_update_duty(ledc_mode_t speed_mode, ledc_channel_t channel);
/** 回读定时器实际载波频率（替身会原样返回最后一次请求的值） */
uint32_t ledc_get_freq(ledc_mode_t speed_mode, ledc_timer_t timer);

/** 测试辅助：读取某通道最近一次写入的占空比 */
uint32_t host_ledc_duty(ledc_channel_t channel);
/** 测试辅助：读取某通道绑定的 GPIO */
int host_ledc_gpio(ledc_channel_t channel);
/** 测试辅助：读取某通道绑定的定时器 */
int host_ledc_timer(ledc_channel_t channel);
/** 测试辅助：读取某定时器最近一次请求的频率；未知返回 0 */
uint32_t host_ledc_timer_freq(ledc_timer_t timer);
/** 测试辅助：清空记录 */
void host_ledc_reset(void);

#endif /* STUB_DRIVER_LEDC_H */
