/*
 * ble_broadcaster.cpp — Bluefruit 广播实现
 *
 * 库依赖：Adafruit Bluefruit nRF52 Library（随 Seeed nRF52 Boards 板级包一起安装）。
 *         在 Arduino IDE 的库管理器里搜索并安装
 *         "Adafruit Bluefruit nRF52 Libraries"（提供 bluefruit.h）。
 *
 * 广播包结构（Legacy，31 字节上限）：
 *   Flags               3 字节
 *   TxPower             3 字节
 *   Manufacturer Data   2 (AD 头) + 16 (CompanyID 2 + Payload 14) = 18 字节
 *   名称                放 Scan Response（不占广播包空间）
 *   合计 24 字节  ✔ 未超过 31 字节上限，无需 Extended Advertising。
 *
 * 关于"状态变化立即广播"的实现说明（需求 3.5 节）：
 *   Bluefruit 的 BLEAdvertising 只暴露单个广播集，没有"立刻触发一次广播"的 API。
 *   本实现的做法是：内容更新后先 stop() 再 start()，SoftDevice 会在下一次广播
 *   事件（≤1 s）发送新内容；然后再 stop()/start() 若干次以追加冗余广播。
 *   这会**推迟**该周期的自动广播，属于需求允许的"立即额外广播"语义
 *   （冗余广播以 40 ms 间隔连发，然后恢复 1 s 周期）。
 *   若后续需要严格 1 s 周期 + 独立冗余广播，应改用 nRF5 SDK 的多广播集
 *   （ble_gap_adv_set_configure）或改用 NCS/Zephyr 的扩展广播。
 */
#include "ble_broadcaster.h"

#include <Arduino.h>

#include "bluefruit.h"

#include "pet_status_broadcaster_config.h"

static uint8_t s_mfg[PET_MFG_DATA_LEN];
static pet_packet_t s_last;
static bool s_ready = false;

/* ------------------------------------------------------------------ */
/* 广播数据构建                                                       */
/* ------------------------------------------------------------------ */

static void rebuild_adv_data(void)
{
    Bluefruit.Advertising.clearData();
    Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAG_LE_GENERAL_DISC_MODE);
    Bluefruit.Advertising.addTxPower();
    (void)Bluefruit.Advertising.addManufacturerData(s_mfg, sizeof(s_mfg));

    /* 名称放 Scan Response，为 Manufacturer Data 留足空间 */
    Bluefruit.ScanResponse.clearData();
    (void)Bluefruit.ScanResponse.addName();
}

static void restart_advertising(void)
{
    Bluefruit.Advertising.stop();
    (void)Bluefruit.Advertising.start(0);
}

/* ------------------------------------------------------------------ */
/* 公开 API                                                           */
/* ------------------------------------------------------------------ */

int ble_broadcaster_init(void)
{
    Bluefruit.begin();
    Bluefruit.setName(BLE_DEVICE_NAME);
    /* +4 dBm：室内 1–10 m（需求 3.8）覆盖足够；nRF52840 支持 -40..+8 dBm */
    Bluefruit.setTxPower(4);

    /* 不注册任何 GATT 服务：V1 只用广播，灯端也不会发起连接 */

    memset(s_mfg, 0, sizeof(s_mfg));
    memset(&s_last, 0, sizeof(s_last));
    s_last.device_id = DEVICE_ID;
    s_last.battery_percent = 100;
    s_last.flags = PET_FLAG_WORN;
    pet_build_mfg_data(s_mfg, &s_last);

    rebuild_adv_data();

    /* V1 固定 1 秒间隔（ADV_INTERVAL_UNITS = 1600 × 0.625 ms = 1000 ms） */
    Bluefruit.Advertising.setInterval(ADV_INTERVAL_UNITS, ADV_INTERVAL_UNITS);
    Bluefruit.Advertising.setFastTimeout(0);
    Bluefruit.Advertising.restartOnDisconnect(false);

    if (!Bluefruit.Advertising.start(0)) {
        s_ready = false;
        return -1;
    }

    s_ready = true;
    return 0;
}

void ble_broadcaster_update(const pet_packet_t *pkt, int immediate)
{
    if (pkt == NULL) {
        return;
    }

    /* 强制协议常量并重算 CRC（防止上层误填） */
    s_last = *pkt;
    s_last.company_id = PET_COMPANY_ID;
    s_last.protocol_version = PET_PROTOCOL_VERSION;
    pet_build_mfg_data(s_mfg, &s_last);

    /* 让新内容生效并尽快发出 */
    restart_advertising();

    if (immediate) {
        /* 状态变化：追加 1–3 个冗余广播，提高灯端必达率 */
        for (uint8_t i = 1u; i < ADV_REDUNDANT_BURST; ++i) {
            delay(ADV_BURST_GAP_MS);
            restart_advertising();
        }
    }
}

void ble_broadcaster_heartbeat(void)
{
    if (s_ready && !Bluefruit.Advertising.isRunning()) {
        (void)Bluefruit.Advertising.start(0);
    }
}

int ble_broadcaster_is_running(void)
{
    return (s_ready && Bluefruit.Advertising.isRunning()) ? 1 : 0;
}
