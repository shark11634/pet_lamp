/*
 * pet_status_broadcaster.ino — 宠物活动状态灯 V1 佩戴端主程序
 *
 * 硬件：Seeed Studio XIAO nRF52840（Sense 版，使用板载 LSM6DS3TR-C 六轴 IMU）
 * 需求：3.1 佩戴端职责 / 3.2 推荐硬件 / 3.3 MCU 选择 / 3.4 运动状态定义
 *       3.5 广播机制 / 3.6 自定义数据协议 / 3.7 佩戴端伪代码 / 3.8 验收要求
 *
 * 主循环结构完全对应需求 3.7 的伪代码：
 *   init IMU / init BLE advertising / init battery monitor /
 *   load DeviceID and sequence from NVS(flash)
 *   loop every 100 ms : read IMU, update rolling activity window
 *   loop every 1 s    : classify activity, read battery, build advertising packet,
 *                       increment sequence, advertise packet,
 *                       on activity state change: advertise immediately 1–3 times,
 *                       on battery below threshold: set LOW_BATTERY flag,
 *                       on sensor error: set SENSOR_ERROR flag,
 *                       continue heartbeat advertising whenever possible
 *
 * 编译：见 wearable_xiao/README.md（arduino-cli / Arduino IDE）
 *
 * ⚠️ 首次使用请确认板级包与库：Seeed nRF52 Boards + Adafruit Bluefruit nRF52 Libraries
 */

#include <Arduino.h>
#include <nrf.h>

#include "activity_classifier.h"
#include "battery_monitor.h"
#include "ble_broadcaster.h"
#include "imu_lsm6ds3.h"
#include "pet_protocol.h"
#include "pet_status_broadcaster_config.h"

/* ================================================================== */
/* 状态                                                               */
/* ================================================================== */

static activity_classifier_t g_cls;
static uint16_t              g_sequence = 0;
static uint8_t               g_battery_pct = 100;
static bool                  g_charging = false;
static bool                  g_sensor_error = false;
static pet_activity_state_t  g_published_state = PET_STATE_QUIET;
static bool                  g_have_published = false;

static uint32_t g_last_imu_ms = 0;
static uint32_t g_last_adv_ms = 0;
static uint32_t g_last_bat_ms = 0;
static uint32_t g_last_log_ms = 0;
static uint32_t g_boot_ms = 0;

/* ================================================================== */
/* DeviceID                                                           */
/* ================================================================== */

/**
 * 取 32 位 DeviceID。
 *
 * 优先使用编译期配置的 DEVICE_ID；若为 0，则从 nRF52 FICR 的 DEVICEID
 * 派生出稳定的非零值（同一颗芯片恒定），满足"每个佩戴端唯一"的要求。
 *
 * 正式产品应改为：从 NVS/Flash 读取 + 配对时写入（见 README 的"DeviceID 写入流程"）。
 */
static uint32_t resolve_device_id(void)
{
    if (DEVICE_ID != 0u) {
        return DEVICE_ID;
    }

    uint32_t lo = NRF_FICR->DEVICEID[0];
    uint32_t hi = NRF_FICR->DEVICEID[1];
    uint32_t id = lo ^ (hi * 2654435761u);   /* Knuth 乘法散列 */
    if (id == 0u || id == 0xFFFFFFFFu) {
        id = 0x5A17C0DEu;                    /* 极端情况的兜底常量 */
    }
    return id;
}

/* ================================================================== */
/* 组包与广播                                                        */
/* ================================================================== */

static uint8_t build_flags(void)
{
    uint8_t flags = 0;

    /* V1 无佩戴检测，WORN 固定为 1（协议 §3 说明） */
    flags |= PET_FLAG_WORN;

    if (g_charging) {
        flags |= PET_FLAG_CHARGING;
    }
    if (g_sensor_error) {
        flags |= PET_FLAG_SENSOR_ERROR;
    }
    if (g_battery_pct < LOW_BATTERY_THRESHOLD_PCT) {
        flags |= PET_FLAG_LOW_BATTERY;
    }

#if PAIR_REQUEST_ON_BOOT
    /* 上电窗口内持续置 PAIR_REQUEST，便于灯端配对模式识别（需求 7.2） */
    if ((uint32_t)(millis() - g_boot_ms) < PAIR_REQUEST_BOOT_WINDOW_MS) {
        flags |= PET_FLAG_PAIR_REQUEST;
    }
#endif

    return flags;
}

static void publish(uint32_t device_id, bool immediate)
{
    pet_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));

    pkt.device_id = device_id;
    pkt.sequence = g_sequence;
    pkt.activity_state = (uint8_t)g_cls.state;
    pkt.activity_score = g_cls.score;
    pkt.battery_percent = g_battery_pct;
    pkt.flags = build_flags();

    ble_broadcaster_update(&pkt, immediate ? 1 : 0);

    g_published_state = g_cls.state;
    g_have_published = true;
    ++g_sequence;   /* 16-bit 无符号递增，满 65535 后自然回绕到 0（协议 §5） */
}

/* ================================================================== */
/* 串口日志                                                           */
/* ================================================================== */

static void log_status(void)
{
    Serial.print(F("["));
    Serial.print(millis() / 1000u);
    Serial.print(F("s] state="));
    Serial.print(pet_state_name((uint8_t)g_cls.state));
    Serial.print(F(" score="));
    Serial.print(g_cls.score);
    Serial.print(F(" mean="));
    Serial.print(g_cls.mean_dyn_mg, 1);
    Serial.print(F("mg peak="));
    Serial.print(g_cls.peak_dyn_mg, 1);
    Serial.print(F("mg ratio="));
    Serial.print(g_cls.over_ratio, 2);
    Serial.print(F(" bat="));
    Serial.print(g_battery_pct);
    Serial.print(F("% ("));
    Serial.print(battery_last_mv());
    Serial.print(F("mV) seq="));
    Serial.print(g_sequence - 1u);
    Serial.print(F(" adv="));
    Serial.print(ble_broadcaster_is_running() ? F("on") : F("off"));
    Serial.print(F(" flags=0x"));
    Serial.println(build_flags(), HEX);
}

/* ================================================================== */
/* Arduino 入口                                                       */
/* ================================================================== */

void setup(void)
{
    Serial.begin(SERIAL_BAUD);
    /* nRF52 USB CDC 需要等待串口就绪，超时 2 秒 */
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < 2000u) {
        delay(10);
    }

    Serial.println();
    Serial.println(F("=== 宠物活动状态灯 V1 佩戴端 ==="));
    Serial.println(F(FIRMWARE_VERSION));
    Serial.print(F("DeviceID = 0x"));
    Serial.println(resolve_device_id(), HEX);

    g_boot_ms = millis();

    /* --- IMU --- */
    if (imu_init() != 0) {
        g_sensor_error = true;
        Serial.println(F("[ERR] IMU 初始化失败（将置 SENSOR_ERROR flag）"));
    } else {
        Serial.println(F("[ OK] LSM6DS3TR-C 就绪：±2 g / 104 Hz"));
    }

    /* --- 电池 --- */
    battery_init();
    g_battery_pct = battery_read_percent();
    g_charging = battery_is_charging() != 0;
    Serial.print(F("[ OK] 电池 "));
    Serial.print(g_battery_pct);
    Serial.print(F("% / "));
    Serial.print(battery_last_mv());
    Serial.println(F(" mV"));

    /* --- 分类器 --- */
    cls_init(&g_cls, millis());

    /* --- BLE 广播 --- */
    if (ble_broadcaster_init() != 0) {
        Serial.println(F("[ERR] BLE 广播初始化失败"));
    } else {
        Serial.print(F("[ OK] BLE 广播已启动，间隔 "));
        Serial.print(TASK_ADV_INTERVAL_MS);
        Serial.println(F(" ms"));
    }

    /* 立刻广播第一包 */
    uint32_t device_id = resolve_device_id();
    publish(device_id, true);
    g_last_adv_ms = millis();
    g_last_imu_ms = millis();
    g_last_bat_ms = millis();
    g_last_log_ms = millis();

    Serial.println(F("开始运行：每 100 ms 采样，每 1 s 广播一次心跳"));
}

void loop(void)
{
    const uint32_t now = millis();
    const uint32_t device_id = resolve_device_id();

    /* ---- loop every 100 ms: read IMU, update rolling activity window ---- */
    if ((uint32_t)(now - g_last_imu_ms) >= TASK_IMU_INTERVAL_MS) {
        g_last_imu_ms = now;

        float ax = 0.0f, ay = 0.0f, az = 0.0f;
        if (imu_read_accel_mg(&ax, &ay, &az) == 0) {
            g_sensor_error = false;
            (void)cls_update(&g_cls, ax, ay, az, now);
        } else {
            g_sensor_error = true;
        }
    }

    /* ---- 传感器异常检测（连续 1 秒无样本） ---- */
    if (!g_sensor_error && cls_sensor_error(&g_cls, now)) {
        g_sensor_error = true;
    }

    /* ---- 电量检测（每 10 秒） ---- */
    if ((uint32_t)(now - g_last_bat_ms) >= TASK_BATTERY_INTERVAL_MS) {
        g_last_bat_ms = now;
        g_battery_pct = battery_read_percent();
        g_charging = battery_is_charging() != 0;
    }

    /* ---- loop every 1 s: classify, build packet, advertise ---- */
    if ((uint32_t)(now - g_last_adv_ms) >= TASK_ADV_INTERVAL_MS) {
        g_last_adv_ms = now;

        /* 状态变化 => 立即额外广播 1–3 次（需求 3.5 / 3.7） */
        const bool changed = (!g_have_published) || (g_cls.state != g_published_state);
        publish(device_id, changed);

        if (changed) {
            Serial.print(F("状态变化 -> "));
            Serial.println(pet_state_name((uint8_t)g_cls.state));
        }

        /* 任何时候都要保证心跳广播在跑（低电量也不停发，需求 3.5） */
        ble_broadcaster_heartbeat();
    }

    /* ---- 日志汇总 ---- */
    if ((uint32_t)(now - g_last_log_ms) >= TASK_LOG_INTERVAL_MS) {
        g_last_log_ms = now;
        log_status();
    }
}
