/*
 * lamp_driver.c — 有状态的灯驱动封装（平台无关）
 *
 * 本文件只做三件事：
 *   1. 持有平台写回调（lamp_platform_t）
 *   2. 缓存最近一次占空比（诊断 / 读取用）
 *   3. 把纯计算转发给 lamp_duty.c
 *
 * 相关文件分工：
 *   lamp_gamma.c    Gamma 2.2 曲线与平台位数换算（纯函数）
 *   lamp_duty.c     全通道限流与占空比换算（纯函数）
 *   lamp_driver.c   有状态封装（本文件）
 */
#include "lamp_driver.h"

#include "lamp_gamma.h"

#include <string.h>

void lamp_driver_init(lamp_driver_t *d, const lamp_platform_t *platform)
{
    if (d == NULL) {
        return;
    }
    memset(d, 0, sizeof(*d));
    if (platform != NULL) {
        d->platform = *platform;
    }
    /* Gamma 表由 lamp_gamma.c 惰性构建，这里不需要预热 */
    d->inited = true;
}

void lamp_driver_set(lamp_driver_t *d, const lamp_color_t *c)
{
    lamp_driver_set_layer(d, c, 1.0f, 0u);
}

void lamp_driver_set_layer(lamp_driver_t *d, const lamp_color_t *c,
                           float layer, uint16_t floor_duty)
{
    if (d == NULL || c == NULL) {
        return;
    }
    lamp_compute_duty(c, layer, floor_duty, d->duty);
    if (d->platform.write != NULL) {
        d->platform.write(d->duty, d->platform.user);
    }
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
    if (d->platform.write != NULL) {
        d->platform.write(d->duty, d->platform.user);
    }
}
