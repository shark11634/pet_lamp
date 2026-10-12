/*
 * imu_lsm6ds3.h — 三轴加速度采集（XIAO nRF52840 Sense 板载 LSM6DS3TR-C）
 * 需求 3.2 节：IMU 三轴加速度计即可，优先低功耗；
 *              V1 主要用加速度幅度与变化量，不强制使用陀螺仪。
 *
 * 兼容性说明：
 *   本模块优先使用 Seeed nRF52 板级包自带的 Seeed_Arduino_LSM6DS3 库；
 *   若该库不存在，则回退到 Adafruit_LSM6DS3（Adafruit_LSM6DS / LSM6DSOX 库族），
 *   两者都提供 LSM6DS3 支持且 API 接近。
 */
#ifndef IMU_LSM6DS3_H
#define IMU_LSM6DS3_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 采样周期 100 ms（需求 3.7 伪代码：loop every 100 ms: read IMU） */
#define IMU_SAMPLE_INTERVAL_MS 100u

/** 初始化板载 IMU；返回 0 成功，非 0 为错误码 */
int imu_init(void);

/**
 * 读取三轴加速度。
 *
 * @param ax_mg ay_mg az_mg 输出，单位 mg（1 g = 1000 mg）
 * @return 0 成功，非 0 失败（上层应据此置 SENSOR_ERROR flag）
 */
int imu_read_accel_mg(float *ax_mg, float *ay_mg, float *az_mg);

/** 是否已初始化成功 */
int imu_is_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* IMU_LSM6DS3_H */
