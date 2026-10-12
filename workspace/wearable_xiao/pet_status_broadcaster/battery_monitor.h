/*
 * battery_monitor.h — 电池电压 / 电量百分比 / 充电状态
 * 需求 3.2 节：电量检测 ADC 分压或电量计 IC，需输出 0–100% 或至少低电量标志。
 * 需求 3.8 节：电量变化可正确显示到广播包。
 */
#ifndef BATTERY_MONITOR_H
#define BATTERY_MONITOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化 ADC（若板级包已提供电池宏则直接使用，无需额外初始化） */
void battery_init(void);

/**
 * 读取电池电压（mV）。失败返回 0。
 */
uint32_t battery_read_mv(void);

/**
 * 读取电量百分比 0–100。
 * 线性映射 BATTERY_EMPTY_MV..BATTERY_FULL_MV；
 * 返回值已钳位到 0–100。
 */
uint8_t battery_read_percent(void);

/**
 * 是否正在充电。
 * 优先使用板级包提供的充电状态宏；无法判断时返回 0（未知按未充电处理，
 * 但**不会**因此置 CHARGING 位造成误报）。
 */
int battery_is_charging(void);

/** 最近一次读取的电压（mV），用于日志 */
uint32_t battery_last_mv(void);

#ifdef __cplusplus
}
#endif

#endif /* BATTERY_MONITOR_H */
