/*
 * test/mock_driver.c — 直通驱动替身的实现（见 mock_driver.h 的说明）
 *
 * 只做一件事：把 core 的灯效输出直接喂给已绑定的写函数，
 * 中间不经过任何 lamp_driver_t 实例，从而让"接线正确性"可测。
 *
 * 注意：限流与占空比换算（lamp_compute_duty）用的是 core/lamp_duty.c 的
 * **真实实现**，本替身不复制任何数学。
 */
#include "mock_driver.h"

#include <string.h>

static lamp_pwm_write_fn g_write;
static void              *g_user;
static bool               g_released;

void mock_driver_bind(lamp_pwm_write_fn write, void *user)
{
    g_write = write;
    g_user = user;
}

void mock_driver_release(void)
{
    g_released = true;
}

void lamp_driver_init(lamp_driver_t *d, const lamp_platform_t *platform)
{
    if (d == NULL) {
        return;
    }
    if (platform != NULL) {
        d->platform = *platform;
    } else {
        d->platform.write = NULL;
        d->platform.user = NULL;
    }
    memset(d->duty, 0, sizeof(d->duty));
    d->inited = true;
}

void lamp_driver_set_layer(lamp_driver_t *d, const lamp_color_t *c,
                           float layer, uint16_t floor_duty)
{
    if (d == NULL || c == NULL || g_released) {
        return;
    }
    /* 真实数学：core/lamp_duty.c */
    lamp_compute_duty(c, layer, floor_duty, d->duty);
    if (g_write != NULL) {
        g_write(d->duty, g_user);
    }
}

void lamp_driver_set(lamp_driver_t *d, const lamp_color_t *c)
{
    lamp_driver_set_layer(d, c, 1.0f, 0u);
}

void lamp_driver_get_duty(const lamp_driver_t *d, uint16_t out[LAMP_CH_COUNT])
{
    if (d == NULL || out == NULL) {
        return;
    }
    memcpy(out, d->duty, sizeof(d->duty));
}

void lamp_driver_off(lamp_driver_t *d)
{
    if (d == NULL) {
        return;
    }
    memset(d->duty, 0, sizeof(d->duty));
    if (g_write != NULL) {
        g_write(d->duty, g_user);
    }
}
