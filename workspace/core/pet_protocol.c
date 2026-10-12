/*
 * pet_protocol.c — 宠物活动状态灯 V1 协议解析/构造（两端共用）
 *
 * 纯 C99，无平台依赖，可用于主机单元测试与两端固件。
 */
#include "pet_protocol.h"

#include <string.h>

#define PET_CRC8_POLY 0x07u

uint8_t pet_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0x00u;

    if (data == NULL) {
        return 0x00u;
    }

    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            if ((crc & 0x80u) != 0u) {
                crc = (uint8_t)((uint8_t)(crc << 1) ^ PET_CRC8_POLY);
            } else {
                crc = (uint8_t)(crc << 1);
            }
        }
    }
    return crc;
}

static uint16_t rd_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static void wr_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void wr_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void bump(uint32_t *counter)
{
    if (counter != NULL) {
        ++(*counter);
    }
}

pet_parse_result_t pet_parse_mfg_data(const uint8_t *mfg, size_t len,
                                      pet_packet_t *out,
                                      pet_rx_stats_t *stats)
{
    if (mfg == NULL || out == NULL) {
        return PET_PARSE_ERR_SHORT;
    }
    if (len < PET_MFG_DATA_LEN) {
        bump(stats ? &stats->invalid_packets : NULL);
        bump(stats ? &stats->length_errors : NULL);
        return PET_PARSE_ERR_LENGTH;
    }
    if (rd_le16(mfg) != PET_COMPANY_ID) {
        bump(stats ? &stats->invalid_packets : NULL);
        bump(stats ? &stats->company_id_errors : NULL);
        return PET_PARSE_ERR_COMPANY_ID;
    }
    if (mfg[2] != PET_PROTOCOL_VERSION) {
        bump(stats ? &stats->invalid_packets : NULL);
        bump(stats ? &stats->version_errors : NULL);
        return PET_PARSE_ERR_VERSION;
    }
    /* CRC 覆盖 Payload 偏移 0–12（即 mfg[2..14]），校验字段在 Payload 偏移 13（mfg[15]） */
    if (pet_crc8(&mfg[2], PET_CRC_COVER_LEN) != mfg[2 + PET_CRC_COVER_LEN]) {
        bump(stats ? &stats->invalid_packets : NULL);
        bump(stats ? &stats->crc_errors : NULL);
        return PET_PARSE_ERR_CRC;
    }

    /* Payload 起始 = mfg + 2 */
    const uint8_t *p = &mfg[2];

    memset(out, 0, sizeof(*out));
    out->company_id       = rd_le16(mfg);
    out->protocol_version = p[0];
    out->device_id        = rd_le32(&p[1]);
    out->sequence         = rd_le16(&p[5]);
    out->activity_state   = p[7];
    out->activity_score   = p[8];
    out->battery_percent  = p[9];
    out->flags            = p[10];
    out->rssi             = 0;

    if (stats != NULL) {
        ++stats->valid_packets;
    }
    return PET_PARSE_OK;
}

void pet_build_mfg_data(uint8_t *out, const pet_packet_t *in)
{
    if (out == NULL || in == NULL) {
        return;
    }

    /* 布局（out 共 PET_MFG_DATA_LEN = 16 字节）：
     *   out[0..1]  = Company ID
     *   out[2..15] = Payload，Payload 偏移 k 对应 out[2 + k]
     *     Payload[0]      ProtocolVersion   -> out[2]
     *     Payload[1..4]   DeviceID          -> out[3..6]
     *     Payload[5..6]   Sequence          -> out[7..8]
     *     Payload[7]      ActivityState     -> out[9]
     *     Payload[8]      ActivityScore     -> out[10]
     *     Payload[9]      BatteryPercent    -> out[11]
     *     Payload[10]     Flags             -> out[12]
     *     Payload[11..12] Reserved (0)      -> out[13..14]
     *     Payload[13]     CRC-8             -> out[15]
     *   CRC 覆盖 Payload 偏移 0–12，即 out[2..14]。
     */
    uint8_t *p = &out[2];

    wr_le16(out, PET_COMPANY_ID);
    p[0]  = PET_PROTOCOL_VERSION;
    wr_le32(&p[1], in->device_id);
    wr_le16(&p[5], in->sequence);
    p[7]  = (uint8_t)(in->activity_state & 0x03u);
    p[8]  = in->activity_score;
    p[9]  = (in->battery_percent > 100u) ? 100u : in->battery_percent;
    p[10] = (uint8_t)(in->flags & 0x1Fu);  /* bit5–7 保留，必须为 0 */
    p[11] = 0u;                            /* 保留 */
    p[12] = 0u;                            /* 保留 */
    p[13] = pet_crc8(p, PET_CRC_COVER_LEN);
}

const char *pet_state_name(uint8_t state)
{
    switch (state) {
    case PET_STATE_QUIET:  return "Quiet";
    case PET_STATE_LIGHT:  return "Light";
    case PET_STATE_ACTIVE: return "Active";
    default:               return "Unknown";
    }
}

const char *pet_parse_error_name(pet_parse_result_t r)
{
    switch (r) {
    case PET_PARSE_OK:                 return "OK";
    case PET_PARSE_ERR_SHORT:          return "SHORT";
    case PET_PARSE_ERR_NO_MFG_DATA:    return "NO_MFG_DATA";
    case PET_PARSE_ERR_COMPANY_ID:     return "COMPANY_ID";
    case PET_PARSE_ERR_LENGTH:         return "LENGTH";
    case PET_PARSE_ERR_VERSION:        return "VERSION";
    case PET_PARSE_ERR_CRC:            return "CRC";
    default:                           return "?";
    }
}
