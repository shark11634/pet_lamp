/*
 * diagnostics.h — 串口日志与可观测性
 *
 * 需求依据：
 *   §6.3 运行频率
 *   §8.5 可观测性：日志至少输出 bootreason、firmware version、BLE 收包计数
 *        （valid / invalid / CRC 错误）、last RSSI、last sequence、
 *        last_seen 时长、当前状态、当前目标亮度/颜色、Lost 转换次数
 *
 * 本模块只读 core 的状态，不修改任何业务数据。
 */
#ifndef DIAGNOSTICS_H
#define DIAGNOSTICS_H

#include <stdbool.h>
#include <stdint.h>

#include "device_registry.h"   /* core */
#include "night_mode.h"        /* core */
#include "pet_protocol.h"      /* core */
#include "storage.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 使用的 core 状态指针（引用，不复制） */
void diagnostics_init(pet_registry_t *reg, pet_night_t *night,
                      const pet_nvs_settings_t *settings);

/**
 * 累加一次解析统计。
 *
 * 为什么不从 registry 读：解析失败的包**根本到不了** registry
 * （pet_parse_mfg_data 就退回了），因此 CRC/版本/长度错误只能在
 * 解析层计数。
 */
void diagnostics_add_rx_stats(const pet_rx_stats_t *stats);

/** 记录启动原因（冷启动 / 看门狗 / 软复位 …）与固件版本 */
void diagnostics_log_boot(void);

/**
 * 周期性输出汇总（需求 §8.5 的 10–60 秒）。
 *
 * 内部按 TASK_DIAG_INTERVAL_MS 限流，因此可以在 1 Hz 的状态任务里直接调用。
 * 输出字段：
 *   bootreason, firmware version, bound devices,
 *   BLE valid / dup / unbound / AD 层计数, CRC 错误,
 *   last RSSI, last sequence, last_seen age,
 *   合并状态, 目标亮度上限, Lost 转换次数
 */
void diagnostics_maybe_report(uint32_t now_ms, const pet_registry_t *reg,
                              const pet_night_t *night, uint32_t lost_timeout_ms);

/** 强制输出一次（网页"立即诊断"或维护模式用） */
void diagnostics_report_now(uint32_t now_ms, const pet_registry_t *reg,
                            const pet_night_t *night, uint32_t lost_timeout_ms);

/** 配对窗口开始 / 结束的事件日志（需求 §7.2 的流程可见性） */
void diagnostics_report_pairing_start(uint32_t window_ms);
void diagnostics_report_pairing_end(uint8_t bound_count);

#ifdef __cplusplus
}
#endif

#endif /* DIAGNOSTICS_H */
