/*
 * imu_lsm6ds3.cpp — 板载 LSM6DS3TR-C 采集实现
 *
 * 量程与速率：
 *   ±2 g（2 g 档分辨率最高，1 LSB ≈ 0.061 mg；宠物活动不会持续超过 2 g）
 *   104 Hz ODR（上层 100 ms 轮询读取，足够且省电）
 *
 * 安装方向：
 *   需求 10.1 要求交付"IMU 型号、量程、采样率与安装方向说明"。
 *   本 V1 算法只用加速度向量模长 |a|，**对安装方向不敏感**，
 *   因此不要求标定安装方向；后续若要做姿态相关功能，需在佩戴端外壳
 *   上标明 +X/+Y/+Z 并在固件中加入方向矩阵。
 *
 * 两条实现路径（由 PET_IMU_USE_LIBRARY 选择）：
 *   1) 库路径（默认）：使用 Seeed_Arduino_LSM6DS3 / Adafruit_LSM6DS。
 *   2) 直读路径：直接用 Wire 读 LSM6DS3 寄存器，完全不依赖第三方库，
 *      适合库版本不匹配或想减少依赖的场景。
 *      注意需要在配置里指定 IMU I²C 引脚（XIAO nRF52840 上为 D4=SDA / D5=SCL）。
 */
#include "imu_lsm6ds3.h"

#include <Arduino.h>
#include <math.h>
#include <Wire.h>

#include "pet_status_broadcaster_config.h"

#if PET_IMU_USE_LIBRARY
#if defined(PET_USE_SEEED_LSM6DS3)
#include <LSM6DS3.h>
#elif defined(PET_USE_ADAFRUIT_LSM6DS)
#include <Adafruit_LSM6DS3TRC.h>
#else
#error "未选择 IMU 库：请定义 PET_USE_SEEED_LSM6DS3 或 PET_USE_ADAFRUIT_LSM6DS，或把 PET_IMU_USE_LIBRARY 设为 0 使用直读路径"
#endif
#endif

/* ------------------------------------------------------------------ */
/* LSM6DS3 寄存器（直读路径用，见 ST 数据手册）                        */
/* ------------------------------------------------------------------ */
#define LSM6DS3_WHO_AM_I_REG   0x0F
#define LSM6DS3_CTRL1_XL       0x10
#define LSM6DS3_OUTX_L_XL      0x28

/** CTRL1_XL = 0x60 => ODR 104 Hz, ±2 g, 带宽 400 Hz */
#define LSM6DS3_CTRL1_XL_104HZ_2G  0x60
#define LSM6DS3_ACCEL_LSB_PER_G    0.061f   /* mg per LSB at ±2 g */

static bool s_ready = false;
static bool s_use_library = (PET_IMU_USE_LIBRARY != 0);

#if PET_IMU_USE_LIBRARY
#if defined(PET_USE_SEEED_LSM6DS3)
/* Seeed 的 LSM6DS3Class 在 I2C 模式下忽略 CS 引脚（传 -1） */
static LSM6DS3Class s_imu(I2C_MODE, -1);
#elif defined(PET_USE_ADAFRUIT_LSM6DS)
static Adafruit_LSM6DS3TRC s_imu;
#endif
#endif

/* ------------------------------------------------------------------ */
/* 直读路径                                                           */
/* ------------------------------------------------------------------ */

static int direct_i2c_write(uint8_t reg, uint8_t value)
{
    Wire.beginTransmission(IMU_I2C_ADDRESS);
    Wire.write(reg);
    Wire.write(value);
    return (Wire.endTransmission() == 0) ? 0 : -1;
}

static int direct_i2c_read(uint8_t reg, uint8_t *buf, size_t len)
{
    Wire.beginTransmission(IMU_I2C_ADDRESS);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) {
        return -1;
    }
    if (Wire.requestFrom((uint8_t)IMU_I2C_ADDRESS, (uint8_t)len) != len) {
        return -1;
    }
    for (size_t i = 0; i < len; ++i) {
        buf[i] = (uint8_t)Wire.read();
    }
    return 0;
}

static int direct_init(void)
{
#if defined(PIN_WIRE_SDA) && defined(PIN_WIRE_SCL)
    Wire.setPins(PIN_WIRE_SDA, PIN_WIRE_SCL);
#endif
    Wire.begin();

    uint8_t who = 0;
    if (direct_i2c_read(LSM6DS3_WHO_AM_I_REG, &who, 1) != 0) {
        return -1;
    }
    /* LSM6DS3TR-C 的 WHO_AM_I 为 0x6A（部分批次 0x69） */
    if (who != 0x6A && who != 0x69) {
        return -2;
    }
    if (direct_i2c_write(LSM6DS3_CTRL1_XL, LSM6DS3_CTRL1_XL_104HZ_2G) != 0) {
        return -3;
    }
    return 0;
}

static int direct_read_mg(float *ax_mg, float *ay_mg, float *az_mg)
{
    uint8_t raw[6] = {0};
    if (direct_i2c_read(LSM6DS3_OUTX_L_XL, raw, sizeof(raw)) != 0) {
        return -1;
    }

    const int16_t x = (int16_t)((uint16_t)raw[0] | ((uint16_t)raw[1] << 8));
    const int16_t y = (int16_t)((uint16_t)raw[2] | ((uint16_t)raw[3] << 8));
    const int16_t z = (int16_t)((uint16_t)raw[4] | ((uint16_t)raw[5] << 8));

    *ax_mg = (float)x * LSM6DS3_ACCEL_LSB_PER_G;
    *ay_mg = (float)y * LSM6DS3_ACCEL_LSB_PER_G;
    *az_mg = (float)z * LSM6DS3_ACCEL_LSB_PER_G;
    return 0;
}

/* ------------------------------------------------------------------ */
/* 统一接口                                                           */
/* ------------------------------------------------------------------ */

int imu_init(void)
{
    s_ready = false;
    s_use_library = (PET_IMU_USE_LIBRARY != 0);

#if PET_IMU_USE_LIBRARY
    if (s_use_library) {
#if defined(PET_USE_SEEED_LSM6DS3)
        s_imu.settings.accelRange = 2;      /* ±2 g */
        s_imu.settings.accelSampleRate = 104;
        s_imu.settings.accelBandWidth = 50;
        s_imu.settings.gyroRange = 2000;    /* V1 不使用陀螺仪，按库要求设置 */
        s_imu.settings.gyroSampleRate = 104;
        if (s_imu.begin() != 0) {
            s_use_library = false;          /* 库路径失败：回退到直读 */
        }
#elif defined(PET_USE_ADAFRUIT_LSM6DS)
        if (!s_imu.begin_I2C(IMU_I2C_ADDRESS)) {
            s_use_library = false;
        } else {
            s_imu.setAccelRange(LSM6DS_ACCEL_RANGE_2_G);
            s_imu.setAccelDataRate(LSM6DS_RATE_104_HZ);
            s_imu.setGyroRange(LSM6DS_GYRO_RANGE_2000_DPS);
            s_imu.setGyroDataRate(LSM6DS_RATE_104_HZ);
        }
#endif
        if (s_use_library) {
            s_ready = true;
            return 0;
        }
    }
#endif

    /* 直读回退 */
    if (direct_init() != 0) {
        s_ready = false;
        return -10;
    }
    s_ready = true;
    s_use_library = false;
    return 0;
}

int imu_is_ready(void)
{
    return s_ready ? 1 : 0;
}

int imu_read_accel_mg(float *ax_mg, float *ay_mg, float *az_mg)
{
    if (!s_ready || ax_mg == NULL || ay_mg == NULL || az_mg == NULL) {
        return -1;
    }

#if PET_IMU_USE_LIBRARY
    if (s_use_library) {
        float ax_g = 0.0f, ay_g = 0.0f, az_g = 0.0f;

#if defined(PET_USE_SEEED_LSM6DS3)
        ax_g = s_imu.readFloatAccelX();
        ay_g = s_imu.readFloatAccelY();
        az_g = s_imu.readFloatAccelZ();
#elif defined(PET_USE_ADAFRUIT_LSM6DS)
        sensors_event_t accel;
        sensors_event_t gyro;
        sensors_event_t temp;
        s_imu.getEvent(&accel, &gyro, &temp);
        const float g = 9.80665f;
        ax_g = accel.acceleration.x / g;
        ay_g = accel.acceleration.y / g;
        az_g = accel.acceleration.z / g;
#endif

        if (isnan(ax_g) || isnan(ay_g) || isnan(az_g)) {
            return -2;
        }
        *ax_mg = ax_g * 1000.0f;
        *ay_mg = ay_g * 1000.0f;
        *az_mg = az_g * 1000.0f;
        return 0;
    }
#endif

    return direct_read_mg(ax_mg, ay_mg, az_mg);
}
