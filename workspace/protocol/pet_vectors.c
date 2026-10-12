/*
 * pet_vectors.c — 协议参考实现 + 测试向量自检（C99，无脚本语言依赖）
 *
 * 本文件既是**独立的参考实现**（用于交叉验证 core/pet_protocol.c），
 * 也是测试向量的唯一定义处：pet_tool.c 的 vectors 子命令与
 * test/host_protocol_test.c 都调用它。
 *
 * 布局约定（与 core/pet_protocol.h、protocol/README.md 完全一致）：
 *   Manufacturer Specific Data (16 字节) = CompanyID(2) + Payload(14)
 *     Payload[0]      ProtocolVersion
 *     Payload[1..4]   DeviceID
 *     Payload[5..6]   Sequence
 *     Payload[7]      ActivityState
 *     Payload[8]      ActivityScore
 *     Payload[9]      BatteryPercent
 *     Payload[10]     Flags
 *     Payload[11..12] Reserved (0)
 *     Payload[13]     CRC-8（覆盖 Payload[0..12]）
 *
 * 纯 C99，只依赖标准库。
 */
#include "pet_vectors.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* 参考实现（与 core/pet_protocol.c 独立编写，用于交叉验证）            */
/* ------------------------------------------------------------------ */

uint8_t petv_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0x00u;
    if (data == NULL) {
        return 0x00u;
    }
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            if ((crc & 0x80u) != 0u) {
                crc = (uint8_t)((uint8_t)(crc << 1) ^ 0x07u);
            } else {
                crc = (uint8_t)(crc << 1);
            }
        }
    }
    return crc;
}

void petv_build(const petv_params_t *p, uint8_t out[PETV_MFG_LEN])
{
    if (p == NULL || out == NULL) {
        return;
    }
    memset(out, 0, PETV_MFG_LEN);
    out[0] = (uint8_t)(PETV_COMPANY_ID & 0xFFu);
    out[1] = (uint8_t)((PETV_COMPANY_ID >> 8) & 0xFFu);

    uint8_t *q = &out[2];
    q[0] = PETV_PROTOCOL_VERSION;
    q[1] = (uint8_t)(p->device_id & 0xFFu);
    q[2] = (uint8_t)((p->device_id >> 8) & 0xFFu);
    q[3] = (uint8_t)((p->device_id >> 16) & 0xFFu);
    q[4] = (uint8_t)((p->device_id >> 24) & 0xFFu);
    q[5] = (uint8_t)(p->sequence & 0xFFu);
    q[6] = (uint8_t)((p->sequence >> 8) & 0xFFu);
    q[7] = (uint8_t)(p->activity_state & 0x03u);
    q[8] = p->activity_score;
    q[9] = (p->battery_percent > 100u) ? 100u : p->battery_percent;
    q[10] = (uint8_t)(p->flags & 0x1Fu);
    q[11] = 0u;
    q[12] = 0u;
    q[13] = petv_crc8(q, PETV_CRC_COVER_LEN);
}

bool petv_verify(const uint8_t mfg[PETV_MFG_LEN])
{
    if (mfg == NULL) {
        return false;
    }
    if ((uint16_t)(mfg[0] | ((uint16_t)mfg[1] << 8)) != PETV_COMPANY_ID) {
        return false;
    }
    if (mfg[2] != PETV_PROTOCOL_VERSION) {
        return false;
    }
    const uint8_t *q = &mfg[2];
    return (petv_crc8(q, PETV_CRC_COVER_LEN) == q[PETV_CRC_FIELD_OFFSET]);
}

bool petv_seq_is_newer(uint16_t s, uint16_t last)
{
    return (int16_t)(s - last) > 0;
}

/* ------------------------------------------------------------------ */
/* 测试向量表                                                          */
/* ------------------------------------------------------------------ */

/*
 * 期望值由参考实现生成后人工确认，并被两端实现共同验证过：
 *   TV-1 CRC = 0xC8，TV-2 CRC = 0xCE，TV-3 CRC = 0x36
 * 这三个值在开发过程中曾暴露出"CRC 覆盖范围错位 2 字节"的真实缺陷
 * （详见 protocol/README.md §4.2），请勿随意改动。
 */
const petv_case_t PETV_CASES[] = {
    {
        "TV-1 Quiet 心跳 (bat=100, flags=0x11)",
        { 0x12345678u, 0x0000u, 0u,   0u, 100u, 0x11u },
        { 0xFFu, 0xFFu, 0x01u, 0x78u, 0x56u, 0x34u, 0x12u,
          0x00u, 0x00u, 0x00u, 0x00u, 0x64u, 0x11u, 0x00u, 0x00u, 0xC8u },
    },
    {
        "TV-2 Active (seq=0x012A, score=200, bat=85)",
        { 0x12345678u, 0x012Au, 2u, 200u,  85u, 0x01u },
        { 0xFFu, 0xFFu, 0x01u, 0x78u, 0x56u, 0x34u, 0x12u,
          0x2Au, 0x01u, 0x02u, 0xC8u, 0x55u, 0x01u, 0x00u, 0x00u, 0xCEu },
    },
    {
        "TV-3 低电量 (seq=0x0010, bat=14, flags=0x11)",
        { 0x12345678u, 0x0010u, 0u,   0u,  14u, 0x11u },
        { 0xFFu, 0xFFu, 0x01u, 0x78u, 0x56u, 0x34u, 0x12u,
          0x10u, 0x00u, 0x00u, 0x00u, 0x0Eu, 0x11u, 0x00u, 0x00u, 0x36u },
    },
};

const size_t PETV_CASE_COUNT = sizeof(PETV_CASES) / sizeof(PETV_CASES[0]);

/* ------------------------------------------------------------------ */
/* 输出辅助                                                            */
/* ------------------------------------------------------------------ */

void petv_hex(const uint8_t *b, size_t n, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0u) {
        return;
    }
    size_t w = 0;
    for (size_t i = 0; i < n && w + 3u < out_len; ++i) {
        const int k = snprintf(&out[w], out_len - w, "%s%02X",
                               (i == 0u) ? "" : " ", b[i]);
        if (k <= 0) {
            break;
        }
        w += (size_t)k;
    }
    out[(w < out_len) ? w : (out_len - 1u)] = '\0';
}

/* ------------------------------------------------------------------ */
/* 自检                                                               */
/* ------------------------------------------------------------------ */

int petv_self_test(bool verbose)
{
    int fails = 0;
    char hex[3 * PETV_MFG_LEN + 8];

    if (verbose) {
        printf("=== 协议参考实现自检（pet_vectors.c）===\n");
        printf("布局：MFG = %u 字节（CompanyID 2 + Payload %u），CRC 覆盖 Payload[0..%u]\n",
               (unsigned)PETV_MFG_LEN, (unsigned)PETV_PAYLOAD_LEN,
               (unsigned)(PETV_CRC_COVER_LEN - 1u));
    }

    /* --- 向量比对 --------------------------------------------------- */
    for (size_t i = 0; i < PETV_CASE_COUNT; ++i) {
        const petv_case_t *c = &PETV_CASES[i];
        uint8_t got[PETV_MFG_LEN];
        petv_build(&c->params, got);

        const int built_ok = (memcmp(got, c->expect, PETV_MFG_LEN) == 0);
        const int verify_ok = petv_verify(got) ? 1 : 0;
        const int crc_ok = (petv_crc8(&got[2], PETV_CRC_COVER_LEN) ==
                            got[2 + PETV_CRC_FIELD_OFFSET]) ? 1 : 0;
        const int ok = built_ok && verify_ok && crc_ok;

        if (verbose) {
            petv_hex(got, PETV_MFG_LEN, hex, sizeof(hex));
            printf("%s\n    built  = %s\n", c->name, hex);
            petv_hex(c->expect, PETV_MFG_LEN, hex, sizeof(hex));
            printf("    expect = %s\n", hex);
            printf("    [%s] 构造一致  [%s] verify  [%s] CRC\n",
                   built_ok ? "PASS" : "FAIL",
                   verify_ok ? "PASS" : "FAIL",
                   crc_ok ? "PASS" : "FAIL");
        }
        if (!ok) {
            ++fails;
        }
    }

    /* --- TV-4 全零 -------------------------------------------------- */
    {
        uint8_t zero[PETV_CRC_COVER_LEN];
        memset(zero, 0, sizeof(zero));
        const uint8_t crc = petv_crc8(zero, sizeof(zero));
        const int ok = (crc == 0x00u);
        if (verbose) {
            printf("TV-4 全零 CRC = 0x%02X（期望 0x00）[%s]\n",
                   crc, ok ? "PASS" : "FAIL");
        }
        if (!ok) {
            ++fails;
        }
    }

    /* --- TV-5 篡改 ActivityState（Payload 偏移 7 = MFG[9]） ---------- */
    {
        uint8_t mfg[PETV_MFG_LEN];
        petv_build(&PETV_CASES[0].params, mfg);
        mfg[2 + 7] ^= 0x01u;
        const int ok = (petv_verify(mfg) == false);
        if (verbose) {
            printf("TV-5 篡改 ActivityState -> verify=%s [%s]\n",
                   ok ? "拒绝（正确）" : "接受（错误！）", ok ? "PASS" : "FAIL");
        }
        if (!ok) {
            ++fails;
        }
    }

    /* --- TV-6 错误 CompanyID ---------------------------------------- */
    {
        uint8_t mfg[PETV_MFG_LEN];
        petv_build(&PETV_CASES[0].params, mfg);
        mfg[0] = 0x00u;
        const int ok = (petv_verify(mfg) == false);
        if (verbose) {
            printf("TV-6 错误 CompanyID -> verify=%s [%s]\n",
                   ok ? "拒绝（正确）" : "接受（错误！）", ok ? "PASS" : "FAIL");
        }
        if (!ok) {
            ++fails;
        }
    }

    /* --- 序号回绕 --------------------------------------------------- */
    {
        struct { uint16_t s, last; bool want; } cases[] = {
            { 0x0001u, 0x0000u, true  },
            { 0x0000u, 0xFFFFu, true  },   /* 回绕 */
            { 0xFFFFu, 0x0000u, false },
            { 0x0005u, 0x0005u, false },
        };
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            const bool got = petv_seq_is_newer(cases[i].s, cases[i].last);
            const int ok = (got == cases[i].want);
            if (verbose) {
                printf("SEQ seq=%04X last=%04X -> %d（期望 %d）[%s]\n",
                       cases[i].s, cases[i].last, (int)got, (int)cases[i].want,
                       ok ? "PASS" : "FAIL");
            }
            if (!ok) {
                ++fails;
            }
        }
    }

    /* --- 固件实现交叉验证 ------------------------------------------- */
    {
        int cross_ok = 1;
        for (size_t i = 0; i < PETV_CASE_COUNT; ++i) {
            uint8_t ref[PETV_MFG_LEN];
            uint8_t impl[PET_MFG_DATA_LEN];
            petv_build(&PETV_CASES[i].params, ref);

            pet_packet_t pkt;
            memset(&pkt, 0, sizeof(pkt));
            pkt.device_id       = PETV_CASES[i].params.device_id;
            pkt.sequence        = PETV_CASES[i].params.sequence;
            pkt.activity_state  = PETV_CASES[i].params.activity_state;
            pkt.activity_score  = PETV_CASES[i].params.activity_score;
            pkt.battery_percent = PETV_CASES[i].params.battery_percent;
            pkt.flags           = PETV_CASES[i].params.flags;
            pet_build_mfg_data(impl, &pkt);

            if (memcmp(ref, impl, PETV_MFG_LEN) != 0) {
                cross_ok = 0;
                if (verbose) {
                    petv_hex(ref, PETV_MFG_LEN, hex, sizeof(hex));
                    printf("交叉验证失败 #%u：参考=%s\n", (unsigned)i, hex);
                    petv_hex(impl, PETV_MFG_LEN, hex, sizeof(hex));
                    printf("                 固件=%s\n", hex);
                }
            }

            /* 固件解析路径 */
            pet_packet_t out;
            pet_rx_stats_t st;
            memset(&st, 0, sizeof(st));
            if (pet_parse_mfg_data(impl, PET_MFG_DATA_LEN, &out, &st) != PET_PARSE_OK) {
                cross_ok = 0;
                if (verbose) {
                    printf("交叉验证失败 #%u：固件解析未返回 OK\n", (unsigned)i);
                }
            } else if (out.device_id != pkt.device_id ||
                       out.sequence != pkt.sequence ||
                       out.activity_state != pkt.activity_state ||
                       out.activity_score != pkt.activity_score ||
                       out.battery_percent != pkt.battery_percent ||
                       out.flags != pkt.flags) {
                cross_ok = 0;
                if (verbose) {
                    printf("交叉验证失败 #%u：字段往返不一致\n", (unsigned)i);
                }
            }
        }
        if (verbose) {
            printf("参考实现 vs 固件实现（core/pet_protocol.c）交叉验证 [%s]\n",
                   cross_ok ? "PASS" : "FAIL");
        }
        if (!cross_ok) {
            ++fails;
        }
    }

    if (verbose) {
        printf("=== %s（失败 %d 项）===\n", (fails == 0) ? "全部通过" : "存在失败", fails);
    }
    return fails;
}

/* ------------------------------------------------------------------ */
/* 生成报告（pet_tool vectors 用）                                     */
/* ------------------------------------------------------------------ */

int petv_report_vectors(void)
{
    char hex[3 * PETV_MFG_LEN + 8];

    printf("=====================================================================\n");
    printf("宠物活动状态灯 V1 — 协议测试向量（size_t 由 pet_vectors.c 定义）\n");
    printf("=====================================================================\n");
    printf("Manufacturer Specific Data：%u 字节 = CompanyID(2) + Payload(%u)\n",
           (unsigned)PETV_MFG_LEN, (unsigned)PETV_PAYLOAD_LEN);
    printf("CRC-8/ATM：poly=0x07 init=0x00，覆盖 Payload[0..%u]，校验位在 Payload[%u]\n",
           (unsigned)(PETV_CRC_COVER_LEN - 1u), (unsigned)PETV_CRC_FIELD_OFFSET);
    printf("AD 结构： [len=17][0xFF] + 16 字节 MFG = 18 字节\n\n");

    for (size_t i = 0; i < PETV_CASE_COUNT; ++i) {
        const petv_case_t *c = &PETV_CASES[i];
        uint8_t got[PETV_MFG_LEN];
        petv_build(&c->params, got);

        petv_hex(got, PETV_MFG_LEN, hex, sizeof(hex));
        printf("%s\n", c->name);
        printf("  MFG        = %s\n", hex);
        printf("  CompanyID  = %04X\n", (unsigned)PETV_COMPANY_ID);
        printf("  DeviceID   = 0x%08lX\n", (unsigned long)c->params.device_id);
        printf("  Sequence   = %u\n", (unsigned)c->params.sequence);
        printf("  State      = %u (%s)\n", (unsigned)c->params.activity_state,
               (c->params.activity_state == 0u) ? "Quiet" :
               (c->params.activity_state == 1u) ? "Light" : "Active");
        printf("  Score      = %u\n", (unsigned)c->params.activity_score);
        printf("  Battery    = %u%%\n", (unsigned)c->params.battery_percent);
        printf("  Flags      = 0x%02X\n", (unsigned)c->params.flags);
        printf("  CRC        = 0x%02X\n", got[2 + PETV_CRC_FIELD_OFFSET]);
        printf("  verify     = %s\n\n", petv_verify(got) ? "PASS" : "FAIL");
    }

    printf("负例（必须被拒）：\n");
    printf("  TV-5 篡改 ActivityState（MFG[9] 取反 bit0）\n");
    printf("  TV-6 错误 CompanyID（MFG[0]=0x00）\n\n");
    printf("序号回绕：\n");
    printf("  seq 0001 / last 0000 -> 更新\n");
    printf("  seq 0000 / last FFFF -> 更新（回绕）\n");
    printf("  seq FFFF / last 0000 -> 不更新\n");
    printf("  seq 0005 / last 0005 -> 不更新\n");
    printf("=====================================================================\n");

    return petv_self_test(false);
}
