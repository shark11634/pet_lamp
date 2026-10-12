/*
 * ble_scanner.h — NimBLE 被动扫描 + AD 结构遍历 + Manufacturer Data 提取
 *
 * 需求依据：
 *   §4.1 灯端职责：持续被动扫描 BLE 广播
 *   §2.1 最终运行原则：灯端作为 Scanner/Observer，**不发起连接**
 *   §4.6 处理流程：AD 遍历与 ManufacturerData 查找属于本模块
 *
 * 职责边界（与 core 的分工）：
 *   本模块  → 把广播里 0xFF 的 Manufacturer Specific Data **原样**取出并入队
 *   core    → pet_parse_mfg_data() 做六层字段校验（CompanyID / 长度 / 版本 /
 *             CRC / DeviceID[由上层] / Sequence[由上层]）
 *
 * 为什么不在回调里解析：NimBLE 回调运行在主机任务上下文，解析 + 日志会拖慢
 * 收包；而且"多狗"要求把解析结果喂给设备表，那属于业务层。
 */
#ifndef BLE_SCANNER_H
#define BLE_SCANNER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "pet_config.h"      /* core：PET_MFG_DATA_LEN 等协议常量 */
#include "pet_protocol.h"    /* core：pet_rx_stats_t */

#ifdef __cplusplus
extern "C" {
#endif

/** 一条原始广播（尚未解析；mfg 是 AD Type 0xFF 之后的完整字节，含 CompanyID） */
typedef struct {
    uint8_t  mfg[PET_MFG_DATA_LEN];
    uint8_t  mfg_len;
    int8_t   rssi;
    uint8_t  addr[6];        /**< 广播源地址（仅日志用，不用于绑定） */
    uint32_t rx_time_ms;
} ble_raw_adv_t;

/** NimBLE 初始化 + 启动被动扫描（不主动请求 SCAN_RSP，省电且不干扰佩戴端） */
esp_err_t ble_scanner_start(void);

/**
 * 阻塞等待下一条原始广播，最长等待 timeout_ms。
 *
 * @return true 取到一条；false 超时
 */
bool ble_scanner_wait(ble_raw_adv_t *out, uint32_t timeout_ms);

/**
 * 本模块自身可观测的统计（与 core 的包级统计互补）。
 *
 * 需求 §8.5 的日志字段里：
 *   valid / invalid / crc_err / ver_err / len_err 由 core 的 pet_rx_stats_t 提供
 *   本函数提供的是"AD 层"计数：收到了多少条 0xFF 广播、多少条被截断/异常
 */
typedef struct {
    uint32_t adv_total;        /**< 收到的广播总条数（含无 MFG 的） */
    uint32_t mfg_found;        /**< 含 0xFF Manufacturer Data 的条数 */
    uint32_t mfg_dropped;      /**< 长度不符/异常而丢弃的条数 */
    uint32_t queue_dropped;    /**< 队列满导致丢弃的条数 */
} ble_scanner_stats_t;

void ble_scanner_get_stats(ble_scanner_stats_t *stats);

/** 最近一条广播的 RSSI；无样本时 valid 为 false */
int8_t ble_scanner_last_rssi(bool *valid);

/** 停止并释放扫描（维护 / 恢复模式用） */
void ble_scanner_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* BLE_SCANNER_H */
