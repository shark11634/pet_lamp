/*
 * pet_status_broadcaster_config.h — 佩戴端编译期配置
 *
 * 需求 3.2 / 3.3 / 3.6 / 3.8 节。所有可配置项集中此处。
 */
#ifndef PET_STATUS_BROADCASTER_CONFIG_H
#define PET_STATUS_BROADCASTER_CONFIG_H

/* ================================================================== */
/* 1. 设备身份                                                        */
/* ================================================================== */

/**
 * 每个佩戴端唯一的 DeviceID（协议 §2 偏移 3–6，4 字节）。
 *
 * V1 做法：出厂/编译期写入。灯端绑定此 ID。
 * 正式产品应改为：从 NVS / Flash 读取，并支持配对时写入。
 * ⚠️ 多个佩戴端同时使用时必须互不相同。
 */
#define DEVICE_ID 0x12345678u

/** 固件版本字符串（串口日志用） */
#define FIRMWARE_VERSION "pet-broadcaster/1.0.0"

/* ================================================================== */
/* 2. IMU                                                             */
/* ================================================================== */

/**
 * 选择 IMU 实现路径：
 *   1 — 使用第三方库（推荐，见下面的库选择宏）
 *   0 — 不依赖任何 IMU 库，直接用 Wire 读 LSM6DS3 寄存器
 *       （WHO_AM_I 0x6A / CTRL1_XL=0x60 / OUTX_L_XL 起 6 字节）
 *       适合库版本不匹配或想减少依赖的场景。
 */
#define PET_IMU_USE_LIBRARY 1

/**
 * 选择 IMU 库（PET_IMU_USE_LIBRARY=1 时二选一）：
 *   PET_USE_SEEED_LSM6DS3  — Seeed nRF52 板级包自带 Seeed_Arduino_LSM6DS3（推荐）
 *   PET_USE_ADAFRUIT_LSM6DS — Adafruit LSM6DS 库族回退
 */
#define PET_USE_SEEED_LSM6DS3 1

/** 板载 LSM6DS3TR-C 的 I²C 地址（SA0 接地 => 0x6A） */
#define IMU_I2C_ADDRESS 0x6A

/* ================================================================== */
/* 3. 广播                                                            */
/* ================================================================== */

/** 广播设备名（便于 nRF Connect 人工确认，需求里程碑 M2） */
#define BLE_DEVICE_NAME "PetTag"

/**
 * V1 固定广播间隔：1 秒（需求 3.5 节：V1 建议先固定为每 1 秒一次，
 * 优先验证可靠性；电池优化放在后续阶段）。
 * 单位 0.625 ms，1600 * 0.625 = 1000 ms。
 */
#define ADV_INTERVAL_UNITS 1600

/** 状态变化时立即发送的冗余广播个数（协议 §1：1–3 个） */
#define ADV_REDUNDANT_BURST 3

/** 冗余广播之间的间隔（毫秒） */
#define ADV_BURST_GAP_MS 40

/** 低电量阈值（%），低于此值置 LOW_BATTERY flag（需求 3.6：例如 20%） */
#define LOW_BATTERY_THRESHOLD_PCT 20

/* ================================================================== */
/* 4. 电量检测                                                        */
/* ================================================================== */

/**
 * 电池满 / 空对应的电压（mV）。单节 Li-Po。
 * 若实际电池不同请按规格调整。
 */
#define BATTERY_FULL_MV  4200
#define BATTERY_EMPTY_MV 3000

/**
 * XIAO nRF52840 的电池电压通过 1/2 分压接到 ADC 引脚。
 *
 * 优先级：
 *   1. 板级包已定义 BAT_VOLTAGE_MV / VBAT_MV / ADC_VBAT_PIN 时直接使用；
 *   2. 否则用下面的 PET_BATTERY_ADC_PIN 走 analogRead。
 *
 * XIAO nRF52840 上电池分压接到 P0.31（AIN7），Arduino 里的编号通常为 31。
 * 若你的板级包用不同编号，改成对应值即可（或直接定义 ADC_VBAT_PIN）。
 */
#define PET_BATTERY_ADC_PIN 31

#define BATTERY_DIVIDER_NUM 2
#define BATTERY_DIVIDER_DEN 1

/* ================================================================== */
/* 5. 任务节奏                                                        */
/* ================================================================== */

/** IMU 采样周期（需求 3.7：loop every 100 ms） */
#define TASK_IMU_INTERVAL_MS 100u

/** 分类 + 组包 + 广播周期（需求 3.7：loop every 1 s） */
#define TASK_ADV_INTERVAL_MS 1000u

/** 电量检测周期 */
#define TASK_BATTERY_INTERVAL_MS 10000u

/** 串口日志汇总周期 */
#define TASK_LOG_INTERVAL_MS 10000u

/** 串口波特率 */
#define SERIAL_BAUD 115200

/* ================================================================== */
/* 6. 配对 / 维护                                                     */
/* ================================================================== */

/**
 * 上电后的前若干毫秒持续置 PAIR_REQUEST 位，
 * 便于灯端在配对模式窗口内识别（需求 7.2：佩戴端在 60 秒内把
 * PAIR_REQUEST 位置 1 并以 0.5–1 秒间隔广播）。
 *
 * 触发方式二选一：
 *   - 上电自动持续 60 秒（默认，便于首次绑定）；
 *   - 或由板载用户按键（如有）触发。
 */
#define PAIR_REQUEST_BOOT_WINDOW_MS 60000u

/** 是否允许通过"上电窗口"自动发 PAIR_REQUEST */
#define PAIR_REQUEST_ON_BOOT 1

#endif /* PET_STATUS_BROADCASTER_CONFIG_H */
