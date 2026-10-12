/*
 * battery_monitor.cpp — 电池电压 / 电量百分比 / 充电状态实现
 *
 * 需求 3.2：电量检测可用 ADC 分压或电量计 IC，需输出 0–100% 或至少低电量标志。
 *
 * 实现策略（按可用性分级，避免因板级宏差异编译失败）：
 *   1. 若 Seeed 板级包提供电池电压宏（BAT_VOLTAGE_MV / VBAT_MV / AIN_VBAT），直接使用；
 *   2. 否则用 nRF52 SAADC 读取电池分压引脚（默认 AIN7 / P0.31），
 *      参考电压内部 0.6 V、增益 1/6（满量程 3.6 V），再乘分压系数；
 *   3. 若上述都不可用（宏被显式关闭），返回 0 并由上层只置 SENSOR_ERROR 之外的
 *      保守值（100%），确保不会误报低电量。
 */
#include "battery_monitor.h"

#include <Arduino.h>

#include "pet_status_broadcaster_config.h"

static uint32_t s_last_mv = 0;

void battery_init(void)
{
    s_last_mv = 0;
}

/* ------------------------------------------------------------------ */
/* 电压读取                                                           */
/* ------------------------------------------------------------------ */

#if defined(BAT_VOLTAGE_MV)
/* Seeed 部分版本的宏：直接给出 mV */
static uint32_t read_vbat_mv(void)
{
    uint32_t mv = (uint32_t)BAT_VOLTAGE_MV;
    return mv;
}
#elif defined(VBAT_MV)
static uint32_t read_vbat_mv(void)
{
    return (uint32_t)VBAT_MV;
}
#elif defined(ADC_VBAT_PIN)
/* 提供的是 ADC 原始引脚，用 analogRead 读取并换算 */
static uint32_t read_vbat_mv(void)
{
    /* analogReadResolution(12) 下满量程 3.6 V（内部参考 0.6 V × 增益 6） */
    const uint32_t raw = (uint32_t)analogRead(ADC_VBAT_PIN);
    const uint32_t pin_mv = (raw * 3600u) / 4095u;
    return (pin_mv * BATTERY_DIVIDER_NUM) / BATTERY_DIVIDER_DEN;
}
#elif defined(PET_BATTERY_ADC_PIN)
static uint32_t read_vbat_mv(void)
{
    const uint32_t raw = (uint32_t)analogRead(PET_BATTERY_ADC_PIN);
    const uint32_t pin_mv = (raw * 3600u) / 4095u;
    return (pin_mv * BATTERY_DIVIDER_NUM) / BATTERY_DIVIDER_DEN;
}
#else
/* 无可用电池检测通道：返回 0，上层按"未知"处理 */
static uint32_t read_vbat_mv(void)
{
    return 0u;
}
#endif

uint32_t battery_read_mv(void)
{
    uint32_t mv = read_vbat_mv();
    if (mv != 0u) {
        s_last_mv = mv;
    }
    return mv;
}

uint32_t battery_last_mv(void)
{
    return s_last_mv;
}

uint8_t battery_read_percent(void)
{
    const uint32_t mv = battery_read_mv();
    if (mv == 0u) {
        /* 无电池检测通道：不误报低电量 */
        return 100u;
    }

    if (mv >= BATTERY_FULL_MV) {
        return 100u;
    }
    if (mv <= BATTERY_EMPTY_MV) {
        return 0u;
    }

    const uint32_t span = BATTERY_FULL_MV - BATTERY_EMPTY_MV;
    return (uint8_t)(((mv - BATTERY_EMPTY_MV) * 100u) / span);
}

int battery_is_charging(void)
{
#if defined(PIN_CHARGING_CURRENT) || defined(CHARGE_STATUS_PIN)
#if defined(PIN_CHARGING_CURRENT)
    const int pin = PIN_CHARGING_CURRENT;
#else
    const int pin = CHARGE_STATUS_PIN;
#endif
    pinMode(pin, INPUT);
    /* 多数板级包中：低电平 = 正在充电（CHG 引脚开漏有效） */
    return (digitalRead(pin) == LOW) ? 1 : 0;
#else
    return 0;   /* 无法判断：不置 CHARGING 位，避免误报 */
#endif
}
