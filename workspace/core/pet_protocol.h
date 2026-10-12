/*
 * pet_protocol.h — 宠物活动状态灯 V1 广播协议（平台无关，两端共用）
 *
 * 依据《宠物活动状态灯（V1）工程需求与实施说明》3.6 / 3.7 / 4.6 节。
 * 协议规范与测试向量见 protocol/README.md 与 protocol/pet_vectors.c。
 *
 * 纯 C99，无平台依赖，可在主机上编译做单元测试。
 * 佩戴端与灯端都用这一份实现（唯一事实来源，不再有"同步副本不一致"问题）。
 */
#ifndef PET_PROTOCOL_H
#define PET_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * 基础类型（pet_activity_state_t / pet_lamp_state_t）集中在 pet_types.h，
 * 避免与灯端状态机重复定义。本文件只负责**协议**：常量、解析结果、报文结构。
 */
#include "pet_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 常量                                                               */
/* ------------------------------------------------------------------ */

/** Manufacturer Specific Data 的 AD Type */
#define PET_AD_TYPE_MANUFACTURER 0xFFu

/** 开发阶段占位 Company ID；量产前申请或替换为自有分配值 */
#define PET_COMPANY_ID 0xFFFFu

/** V1 协议版本 */
#define PET_PROTOCOL_VERSION 0x01u

/** Payload 长度（不含 Company ID 的 AD 头部） */
#define PET_PAYLOAD_LEN 14u

/** Manufacturer Specific Data 总长度：2 (CompanyID) + 14 (Payload) */
#define PET_MFG_DATA_LEN (PET_PAYLOAD_LEN + 2u)

/** CRC 覆盖长度：Payload[0..12] */
#define PET_CRC_COVER_LEN 13u

/** 状态变化时立即发送的冗余广播次数 */
#define PET_REDUNDANT_ADV_COUNT 3u

/** 低电量阈值（百分比），与 Flags.LOW_BATTERY 联动 */
#define PET_LOW_BATTERY_THRESHOLD 20u

/** Flags 位（协议 §3，偏移 12） */
#define PET_FLAG_WORN         (1u << 0)
#define PET_FLAG_CHARGING     (1u << 1)
#define PET_FLAG_SENSOR_ERROR (1u << 2)
#define PET_FLAG_PAIR_REQUEST (1u << 3)
#define PET_FLAG_LOW_BATTERY  (1u << 4)

/* ------------------------------------------------------------------ */
/* 解析结果                                                           */
/* ------------------------------------------------------------------ */

/** 解析失败原因，便于串口诊断与有效包率统计 */
typedef enum {
    PET_PARSE_OK = 0,
    PET_PARSE_ERR_SHORT,
    PET_PARSE_ERR_NO_MFG_DATA,
    PET_PARSE_ERR_COMPANY_ID,
    PET_PARSE_ERR_LENGTH,
    PET_PARSE_ERR_VERSION,
    PET_PARSE_ERR_CRC,
} pet_parse_result_t;

/**
 * 解析后的广播内容。
 * 注意：DeviceID / Sequence 的新旧判定由上层状态机负责，
 * 本结构只承载"字段正确"的包。
 */
typedef struct {
    uint16_t company_id;
    uint8_t  protocol_version;
    uint32_t device_id;         /**< 小端读取，协议内唯一标识 */
    uint16_t sequence;
    uint8_t  activity_state;    /**< pet_activity_state_t */
    uint8_t  activity_score;    /**< 0–255 */
    uint8_t  battery_percent;   /**< 0–100，>100 视为异常但不因 CRC 拒绝 */
    uint8_t  flags;
    int8_t   rssi;              /**< 由扫描回调填充，非协议字段 */
} pet_packet_t;

/** 解析统计，用于 diagnostics 模块输出（4.6 / 9 节日志要求） */
typedef struct {
    uint32_t valid_packets;
    uint32_t invalid_packets;
    uint32_t crc_errors;
    uint32_t version_errors;
    uint32_t length_errors;
    uint32_t company_id_errors;
    uint32_t duplicate_packets;
} pet_rx_stats_t;

/* ------------------------------------------------------------------ */
/* API                                                                */
/* ------------------------------------------------------------------ */

/** CRC-8/ATM：poly=0x07, init=0x00，覆盖 data[0..len) */
uint8_t pet_crc8(const uint8_t *data, size_t len);

/**
 * 从完整 Manufacturer Specific Data（含前 2 字节 Company ID）解析。
 *
 * @param mfg      指向 AD Type 0xFF 之后的数据：
 *                 mfg[0..1] = CompanyID，mfg[2..15] = Payload（共 PET_MFG_DATA_LEN 字节）
 * @param len      mfg 的字节数，期望 PET_MFG_DATA_LEN
 * @param out      解析结果（成功时写入）
 * @param stats    可为 NULL；按失败原因累加计数
 * @return PET_PARSE_OK 或具体失败原因
 */
pet_parse_result_t pet_parse_mfg_data(const uint8_t *mfg, size_t len,
                                      pet_packet_t *out, pet_rx_stats_t *stats);

/**
 * 构造 Manufacturer Specific Data（佩戴端用）。
 *
 * @param out      至少 PET_MFG_DATA_LEN 字节；
 *                 out[0..1] = CompanyID，out[2..15] = Payload，CRC 自动计算
 * @param in       待写入的字段；company_id / protocol_version 会被强制为协议常量
 */
void pet_build_mfg_data(uint8_t *out, const pet_packet_t *in);

/**
 * 16-bit 序号回绕安全的新旧判定（协议 §5）。
 * 返回 true 表示 s 比 last 更新。
 */
static inline bool pet_seq_is_newer(uint16_t s, uint16_t last)
{
    return (int16_t)(s - last) > 0;
}

/** 状态名（日志用） */
const char *pet_state_name(uint8_t state);

/** 解析错误名（日志用） */
const char *pet_parse_error_name(pet_parse_result_t r);

#ifdef __cplusplus
}
#endif

#endif /* PET_PROTOCOL_H */
