/*
 * host_protocol_test.c — 协议实现测试（C99，替代原 Python 向量自检）
 *
 * 覆盖：
 *   1. 测试向量 TV-1..TV-4（参考实现 pet_vectors.c）
 *   2. 负例 TV-5/TV-6、长度不足、版本不支持、Sequence 回绕
 *   3. 参考实现 vs 固件实现（core/pet_protocol.c）交叉验证
 *   4. 解析统计分类正确（crc_err / ver_err / len_err）
 *   5. 边界：battery > 100 被钳位、flags 高位被清零、DeviceID 全字节往返
 */
#include <stdio.h>
#include <string.h>

#include "pet_protocol.h"
#include "pet_test.h"
#include "pet_vectors.h"

/* ------------------------------------------------------------------ */

static void test_vectors_cross_check(void)
{
    /* 参考实现自检（含与固件实现的交叉验证），静默模式只返回失败数 */
    t_eq(petv_self_test(false), 0, "参考实现自检 + 与固件实现交叉验证");
}

static void test_parse_accept(void)
{
    uint8_t mfg[PET_MFG_DATA_LEN];
    pet_packet_t in;
    memset(&in, 0, sizeof(in));
    in.device_id       = 0xDEADBEEFu;
    in.sequence        = 0xBEEFu;
    in.activity_state  = PET_STATE_LIGHT;
    in.activity_score  = 137u;
    in.battery_percent = 42u;
    in.flags           = PET_FLAG_WORN | PET_FLAG_PAIR_REQUEST;
    pet_build_mfg_data(mfg, &in);

    pet_rx_stats_t st;
    pet_packet_t out;
    memset(&st, 0, sizeof(st));
    const pet_parse_result_t r = pet_parse_mfg_data(mfg, sizeof(mfg), &out, &st);

    t_eq(r, PET_PARSE_OK, "解析返回 OK");
    t_eq((long)out.device_id, (long)0xDEADBEEFu, "DeviceID 往返一致");
    t_eq(out.sequence, 0xBEEFu, "Sequence 往返一致");
    t_eq(out.activity_state, PET_STATE_LIGHT, "ActivityState 往返一致");
    t_eq(out.activity_score, 137, "ActivityScore 往返一致");
    t_eq(out.battery_percent, 42, "BatteryPercent 往返一致");
    t_eq(out.flags, PET_FLAG_WORN | PET_FLAG_PAIR_REQUEST, "Flags 往返一致");
    t_eq(out.protocol_version, PET_PROTOCOL_VERSION, "ProtocolVersion == 0x01");
    t_eq(st.valid_packets, 1, "有效包计数 +1");
    t_eq(st.invalid_packets, 0, "无效包计数为 0");
}

static void test_parse_reject(void)
{
    uint8_t mfg[PET_MFG_DATA_LEN];
    pet_packet_t in;
    memset(&in, 0, sizeof(in));
    in.device_id = 0x12345678u;
    in.battery_percent = 100u;
    in.flags = PET_FLAG_WORN;

    pet_rx_stats_t st;
    pet_packet_t out;
    memset(&st, 0, sizeof(st));

    /* 篡改 ActivityState（Payload 偏移 7 = MFG[9]） */
    pet_build_mfg_data(mfg, &in);
    mfg[2 + 7] ^= 0x01u;
    t_eq(pet_parse_mfg_data(mfg, sizeof(mfg), &out, &st),
         PET_PARSE_ERR_CRC, "篡改 ActivityState -> CRC 错误");

    /* 篡改 Flags（Payload 偏移 10 = MFG[12]） */
    pet_build_mfg_data(mfg, &in);
    mfg[2 + 10] ^= 0x80u;
    t_eq(pet_parse_mfg_data(mfg, sizeof(mfg), &out, &st),
         PET_PARSE_ERR_CRC, "篡改 Flags -> CRC 错误");

    /* 错误 CompanyID */
    pet_build_mfg_data(mfg, &in);
    mfg[0] = 0x00u;
    t_eq(pet_parse_mfg_data(mfg, sizeof(mfg), &out, &st),
         PET_PARSE_ERR_COMPANY_ID, "错误 CompanyID -> COMPANY_ID 错误");

    /* 长度不足 */
    pet_build_mfg_data(mfg, &in);
    t_eq(pet_parse_mfg_data(mfg, PET_MFG_DATA_LEN - 1u, &out, &st),
         PET_PARSE_ERR_LENGTH, "长度不足 -> LENGTH 错误");

    /* 版本不支持（修正 CRC 以隔离版本判定） */
    pet_build_mfg_data(mfg, &in);
    mfg[2] = 0x02u;
    mfg[2 + PET_CRC_COVER_LEN] = pet_crc8(&mfg[2], PET_CRC_COVER_LEN);
    t_eq(pet_parse_mfg_data(mfg, sizeof(mfg), &out, &st),
         PET_PARSE_ERR_VERSION, "ProtocolVersion 0x02 -> VERSION 错误");

    t_eq(st.valid_packets, 0, "负例均未计入 valid_packets");
    t_eq(st.crc_errors, 2, "crc_err 计数 = 2");
    t_eq(st.company_id_errors, 1, "company_id_err 计数 = 1");
    t_eq(st.length_errors, 1, "len_err 计数 = 1");
    t_eq(st.version_errors, 1, "ver_err 计数 = 1");
    t_eq(st.invalid_packets, 5, "invalid 总数 = 5");
}

static void test_sequence_wraparound(void)
{
    t_check(pet_seq_is_newer(0x0001u, 0x0000u), "1 > 0");
    t_check(pet_seq_is_newer(0x0000u, 0xFFFFu), "回绕 0 > 0xFFFF");
    t_check(!pet_seq_is_newer(0xFFFFu, 0x0000u), "0xFFFF 不比 0 新");
    t_check(!pet_seq_is_newer(0x0005u, 0x0005u), "相同序号不是更新");
    t_check(pet_seq_is_newer(0x8000u, 0x7FFFu), "0x8000 比 0x7FFF 新（恰好半程）");
    t_check(!pet_seq_is_newer(0x7FFFu, 0x8000u), "0x7FFF 不比 0x8000 新（半程之外视为旧）");
}

static void test_field_clamping(void)
{
    uint8_t mfg[PET_MFG_DATA_LEN];
    pet_packet_t in;
    pet_packet_t out;
    pet_rx_stats_t st;
    memset(&in, 0, sizeof(in));
    in.device_id = 0x01020304u;

    /* battery > 100 应被钳位到 100 */
    in.battery_percent = 200u;
    in.flags = 0xFFu;   /* 高位应被清零 */
    pet_build_mfg_data(mfg, &in);
    memset(&st, 0, sizeof(st));
    t_eq(pet_parse_mfg_data(mfg, sizeof(mfg), &out, &st), PET_PARSE_OK,
         "battery=200 仍能通过解析");
    t_eq(out.battery_percent, 100, "battery 被钳位到 100");
    t_eq(out.flags & 0xE0u, 0, "Flags bit5-7 被清零（保留位）");
    t_eq(out.flags & 0x1Fu, 0x1Fu, "Flags bit0-4 全部保留");

    /* activity_state 只有 2 位有效 */
    memset(&in, 0, sizeof(in));
    in.device_id = 0x01020304u;
    in.battery_percent = 50u;
    in.activity_state = 0xFFu;
    pet_build_mfg_data(mfg, &in);
    memset(&st, 0, sizeof(st));
    t_eq(pet_parse_mfg_data(mfg, sizeof(mfg), &out, &st), PET_PARSE_OK,
         "activity_state 越界仍能通过字段解析");
    t_eq(out.activity_state, 0x03u, "activity_state 被截到低 2 位");
}

static void test_device_id_bytes(void)
{
    /* 四个字节都要能独立往返，防止 endianness 或字节顺序错误 */
    static const uint32_t ids[] = {
        0x00000001u, 0x00000100u, 0x00010000u, 0x01000000u,
        0xFFFFFFFFu, 0x12345678u, 0xA5A5A5A5u,
    };
    int ok = 1;
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) {
        uint8_t mfg[PET_MFG_DATA_LEN];
        pet_packet_t in;
        pet_packet_t out;
        pet_rx_stats_t st;
        memset(&in, 0, sizeof(in));
        memset(&st, 0, sizeof(st));
        in.device_id = ids[i];
        in.battery_percent = 50u;
        in.flags = PET_FLAG_WORN;
        pet_build_mfg_data(mfg, &in);
        if (pet_parse_mfg_data(mfg, sizeof(mfg), &out, &st) != PET_PARSE_OK) {
            ok = 0;
            break;
        }
        if (out.device_id != ids[i]) {
            t_info("DeviceID 0x%08lX 往返得到 0x%08lX",
                   (unsigned long)ids[i], (unsigned long)out.device_id);
            ok = 0;
            break;
        }
        /* 小端序检查：DeviceID 最低字节应在 Payload 偏移 1 = MFG[3] */
        if (mfg[3] != (uint8_t)(ids[i] & 0xFFu)) {
            t_info("DeviceID 0x%08lX 不是小端序写入", (unsigned long)ids[i]);
            ok = 0;
            break;
        }
    }
    t_check(ok, "DeviceID 全字节往返且为小端序");
}

/* ------------------------------------------------------------------ */

int main(void)
{
    static const pet_test_case_t cases[] = {
        { "测试向量 + 参考/固件交叉验证", test_vectors_cross_check },
        { "解析正例",                     test_parse_accept },
        { "解析负例与错误分类",           test_parse_reject },
        { "Sequence 回绕",                test_sequence_wraparound },
        { "字段钳位与保留位",             test_field_clamping },
        { "DeviceID 字节序",              test_device_id_bytes },
    };

    printf("=====================================================================\n");
    printf("宠物活动状态灯 V1 — 协议主机测试\n");
    printf("=====================================================================\n");

    const int fails = t_run("协议", cases, sizeof(cases) / sizeof(cases[0]));

    printf("\n=====================================================================\n");
    printf("%s（失败 %d 项）\n", fails == 0 ? "协议测试全部通过" : "协议测试存在失败", fails);
    printf("=====================================================================\n");
    return (fails == 0) ? 0 : 1;
}
