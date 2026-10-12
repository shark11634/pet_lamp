/*
 * ble_broadcaster.h — BLE 广播（Manufacturer Specific Data）
 * 需求 3.5 节广播机制、3.6 节自定义数据协议。
 *
 * 采用 BLE Advertising 而不是常连 GATT：
 *   - 佩戴端耗电更低；
 *   - 不需要配对、连接维持、重连管理或手机参与；
 *   - 灯端上电即扫，天然适合检测"最后一次出现时间"。
 */
#ifndef BLE_BROADCASTER_H
#define BLE_BROADCASTER_H

#include <stdint.h>

#include "pet_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化 Bluefruit 并配置广播数据（设备名 + Manufacturer Data 占位） */
int ble_broadcaster_init(void);

/**
 * 设置当前广播内容并（可选）立即广播。
 *
 * @param pkt        要广播的字段（company_id / protocol_version 会被强制为协议常量，
 *                   CRC 自动计算）
 * @param immediate  为 true 时在返回前额外发送 ADV_REDUNDANT_BURST 个冗余广播
 *                   （需求：状态发生显著变化时立即额外广播 1–3 次）
 */
void ble_broadcaster_update(const pet_packet_t *pkt, int immediate);

/** 仅发送一次当前缓存的内容（心跳） */
void ble_broadcaster_heartbeat(void);

/** 广播是否正在运行 */
int ble_broadcaster_is_running(void);

#ifdef __cplusplus
}
#endif

#endif /* BLE_BROADCASTER_H */
