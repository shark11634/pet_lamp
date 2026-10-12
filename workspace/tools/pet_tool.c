/*
 * pet_tool.c — 宠物活动状态灯 V1 离线工具（C99，替代原两个 Python 脚本）
 *
 * 子命令：
 *   vectors                         打印测试向量并跑自检
 *   build [选项]                    离线构造广播包，核对 CRC 与字段
 *   log --file <日志> [--expect <id>] [--csv <文件>]
 *                                   解析灯端串口日志，出链路/稳定性报告
 *
 * 为什么用 C 而不是 Python：
 *   1. 与固件共用同一份协议实现（core/pet_protocol.c），不存在两份实现漂移
 *   2. 整个工程零 Python 依赖（ESPHome 组件必需的 __init__.py 是 PC 端代码
 *      生成约定，不进设备、不参与本工具）
 *
 * 构建：
 *   gcc -std=c99 -O2 -I ../core -I ../protocol pet_tool.c \
 *       ../core/pet_protocol.c ../protocol/pet_vectors.c -o pet_tool -lm
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pet_config.h"
#include "pet_protocol.h"
#include "pet_types.h"
#include "pet_vectors.h"

#define LOG_MAX_DEVICES 16
#define LINE_MAX_LEN    512

/* ================================================================== */
/* 通用工具                                                           */
/* ================================================================== */

static void usage(const char *argv0)
{
    printf("宠物活动状态灯 V1 离线工具\n\n");
    printf("用法：\n");
    printf("  %s vectors\n", argv0);
    printf("      打印测试向量并运行自检\n\n");
    printf("  %s build [--state 0|1|2] [--seq N] [--score N] [--battery N]\n", argv0);
    printf("                 [--flags 0xNN] [--device-id 0xNNNNNNNN]\n");
    printf("                 [--lamp-device-id 0xNNNNNNNN] [--mfg \"FF FF ...\"]\n");
    printf("      构造 / 校验一个广播包\n\n");
    printf("  %s log --file <日志文件> [--expect 0xNNNNNNNN] [--csv <输出>]\n", argv0);
    printf("      解析灯端串口日志，输出链路与稳定性报告\n\n");
    printf("示例：\n");
    printf("  %s build --state 2 --seq 300 --battery 85 --lamp-device-id 0x12345678\n", argv0);
    printf("  %s log --file lamp.log --expect 0x12345678 --csv report.csv\n", argv0);
}

static int parse_int(const char *s, long *out)
{
    char *end = NULL;
    const long v = strtol(s, &end, 0);
    if (end == s || (end != NULL && *end != '\0')) {
        return -1;
    }
    *out = v;
    return 0;
}

static void hex_dump(const uint8_t *b, size_t n, char *out, size_t out_len)
{
    size_t w = 0;
    if (out == NULL || out_len == 0u) {
        return;
    }
    for (size_t i = 0; i < n; ++i) {
        const int k = snprintf(&out[w], out_len - w, (i == 0u) ? "%02X" : " %02X", b[i]);
        if (k <= 0 || (size_t)k >= out_len - w) {
            break;
        }
        w += (size_t)k;
    }
    out[(w < out_len) ? w : (out_len - 1u)] = '\0';
}

/* ================================================================== */
/* build                                                              */
/* ================================================================== */

/** 完整 AD 结构：Flags + TxPower + ManufacturerData + ShortenedName(≤5) */
static size_t build_ad(const uint8_t mfg[PET_MFG_DATA_LEN],
                       const char *name, uint8_t *out, size_t out_len)
{
    uint8_t tmp[31];
    size_t w = 0;

    /* Flags: LE General Discoverable */
    tmp[w++] = 0x02; tmp[w++] = 0x01; tmp[w++] = 0x06;
    /* TxPower 0 dBm */
    tmp[w++] = 0x02; tmp[w++] = 0x0A; tmp[w++] = 0x00;
    /* Manufacturer Specific Data */
    tmp[w++] = (uint8_t)(PET_MFG_DATA_LEN + 1u);
    tmp[w++] = PET_AD_TYPE_MANUFACTURER;
    memcpy(&tmp[w], mfg, PET_MFG_DATA_LEN);
    w += PET_MFG_DATA_LEN;
    /* Shortened Local Name，预算内截断 */
    {
        size_t budget = 31u - w - 2u;
        const char *nm = (name != NULL) ? name : "PetTag";
        size_t n = strlen(nm);
        if (n > budget) {
            n = budget;
        }
        if (n > 0u) {
            tmp[w++] = (uint8_t)(n + 1u);
            tmp[w++] = 0x08;
            memcpy(&tmp[w], nm, n);
            w += n;
        }
    }
    if (w > 31u) {
        return 0u;
    }
    if (out != NULL && out_len >= w) {
        memcpy(out, tmp, w);
    }
    return w;
}

static int cmd_build(int argc, char **argv)
{
    pet_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.device_id = 0x12345678u;
    pkt.flags = PET_FLAG_WORN;
    pkt.battery_percent = 100;

    uint32_t lamp_id = 0u;
    bool have_lamp_id = false;
    const char *mfg_hex = NULL;
    int state_arg = -1;

    for (int i = 0; i < argc; ++i) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        long n = 0;
        if (strcmp(a, "--state") == 0 && v != NULL && parse_int(v, &n) == 0) {
            state_arg = (int)n; ++i;
        } else if (strcmp(a, "--seq") == 0 && v != NULL && parse_int(v, &n) == 0) {
            pkt.sequence = (uint16_t)n; ++i;
        } else if (strcmp(a, "--score") == 0 && v != NULL && parse_int(v, &n) == 0) {
            pkt.activity_score = (uint8_t)n; ++i;
        } else if (strcmp(a, "--battery") == 0 && v != NULL && parse_int(v, &n) == 0) {
            pkt.battery_percent = (uint8_t)n; ++i;
        } else if (strcmp(a, "--flags") == 0 && v != NULL && parse_int(v, &n) == 0) {
            pkt.flags = (uint8_t)n; ++i;
        } else if (strcmp(a, "--device-id") == 0 && v != NULL && parse_int(v, &n) == 0) {
            pkt.device_id = (uint32_t)n; ++i;
        } else if (strcmp(a, "--lamp-device-id") == 0 && v != NULL && parse_int(v, &n) == 0) {
            lamp_id = (uint32_t)n; have_lamp_id = true; ++i;
        } else if (strcmp(a, "--mfg") == 0 && v != NULL) {
            mfg_hex = v; ++i;
        } else {
            printf("未知或缺少参数：%s\n", a);
            return 2;
        }
    }

    uint8_t mfg[PET_MFG_DATA_LEN];
    memset(mfg, 0, sizeof(mfg));
    bool parsed_only = false;

    if (mfg_hex != NULL) {
        /* 解析给定的 16 字节 MFG（十六进制串，允许空格） */
        size_t n = 0;
        const char *p = mfg_hex;
        while (*p != '\0' && n < PET_MFG_DATA_LEN) {
            while (*p == ' ' || *p == ':') {
                ++p;
            }
            if (*p == '\0') {
                break;
            }
            char pair[3] = { p[0], (p[0] != '\0' && p[1] != '\0') ? p[1] : '0', '\0' };
            char *end = NULL;
            const long v = strtol(pair, &end, 16);
            if (end == pair) {
                printf("MFG 十六进制串解析失败，位置：%s\n", p);
                return 2;
            }
            mfg[n++] = (uint8_t)v;
            p += (p[1] != '\0') ? 2 : 1;
        }
        if (n != PET_MFG_DATA_LEN) {
            printf("错误：MFG 必须是 %u 字节，收到 %u 字节\n",
                   (unsigned)PET_MFG_DATA_LEN, (unsigned)n);
            return 2;
        }
        parsed_only = true;
        pkt.activity_state = mfg[2 + 7];
        pkt.device_id = (uint32_t)mfg[3] | ((uint32_t)mfg[4] << 8) |
                        ((uint32_t)mfg[5] << 16) | ((uint32_t)mfg[6] << 24);
    } else {
        pkt.activity_state = (state_arg >= 0) ? (uint8_t)state_arg : PET_STATE_QUIET;
        pet_build_mfg_data(mfg, &pkt);
    }

    char hex[3 * PET_MFG_DATA_LEN + 4];
    printf("=====================================================================\n");
    printf("广播包解析\n");
    printf("=====================================================================\n");
    hex_dump(mfg, PET_MFG_DATA_LEN, hex, sizeof(hex));
    printf("  MFG (16B)   : %s\n", hex);

    pet_rx_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    pet_packet_t out;
    const pet_parse_result_t r = pet_parse_mfg_data(mfg, PET_MFG_DATA_LEN, &out, &stats);
    printf("  解析结果    : %s\n", pet_parse_error_name(r));

    if (r == PET_PARSE_OK) {
        printf("  CompanyID   : 0x%04X\n", out.company_id);
        printf("  Protocol    : 0x%02X\n", out.protocol_version);
        printf("  DeviceID    : 0x%08lX\n", (unsigned long)out.device_id);
        printf("  Sequence    : %u\n", (unsigned)out.sequence);
        printf("  State       : %u (%s)\n", (unsigned)out.activity_state,
               pet_state_name(out.activity_state));
        printf("  Score       : %u\n", (unsigned)out.activity_score);
        printf("  Battery     : %u%%\n", (unsigned)out.battery_percent);
        printf("  Flags       : 0x%02X%s%s%s%s\n", (unsigned)out.flags,
               (out.flags & PET_FLAG_WORN) ? " WORN" : "",
               (out.flags & PET_FLAG_CHARGING) ? " CHARGING" : "",
               (out.flags & PET_FLAG_SENSOR_ERROR) ? " SENSOR_ERROR" : "",
               (out.flags & PET_FLAG_LOW_BATTERY) ? " LOW_BATTERY" : "");
    }

    /* 六层校验逐条结论（对应需求 4.6 灯端处理流程） */
    printf("---------------------------------------------------------------------\n");
    int checks_failed = 0;
    struct { const char *name; int ok; } checks[6];
    checks[0].name = "包含正确 CompanyID";
    checks[0].ok = (uint16_t)(mfg[0] | ((uint16_t)mfg[1] << 8)) == PET_COMPANY_ID;
    checks[1].name = "长度符合预期";
    checks[1].ok = true;
    checks[2].name = "ProtocolVersion 受支持";
    checks[2].ok = (mfg[2] == PET_PROTOCOL_VERSION);
    checks[3].name = "CRC 正确";
    checks[3].ok = (pet_crc8(&mfg[2], PET_CRC_COVER_LEN) == mfg[2 + PET_CRC_COVER_LEN]);
    checks[4].name = "DeviceID 等于绑定值";
    checks[4].ok = (!have_lamp_id) || (out.device_id == lamp_id);
    checks[5].name = "Sequence 比上次更新";
    checks[5].ok = true;   /* 无历史状态，单包无法判定 */
    for (int i = 0; i < 6; ++i) {
        printf("  [%s] %s\n", checks[i].ok ? "PASS" : "FAIL", checks[i].name);
        if (!checks[i].ok) {
            ++checks_failed;
        }
    }
    if (have_lamp_id) {
        printf("  [INFO] 灯端绑定 DeviceID = 0x%08lX\n", (unsigned long)lamp_id);
    }

    uint8_t ad[31];
    const size_t ad_len = build_ad(mfg, "PetTag", ad, sizeof(ad));
    if (ad_len > 0u) {
        char ad_hex[3 * 31 + 4];
        hex_dump(ad, ad_len, ad_hex, sizeof(ad_hex));
        printf("---------------------------------------------------------------------\n");
        printf("  AD 数据     : %s\n", ad_hex);
        printf("  AD 长度     : %u/31 字节\n", (unsigned)ad_len);
    }

    printf("=====================================================================\n");
    if (parsed_only) {
        printf("灯端会%s此包（CRC=%s）\n",
               checks_failed == 0 ? "接受" : "丢弃",
               checks[3].ok ? "正确" : "错误");
    } else {
        printf("灯端会%s此包\n", checks_failed == 0 ? "接受" : "丢弃");
    }
    return (checks_failed == 0) ? 0 : 1;
}

/* ================================================================== */
/* log                                                                */
/* ================================================================== */

typedef struct {
    int      used;
    uint32_t device_id;
    long     valid;
    long     invalid;
    long     crc_err;
    long     ver_err;
    long     len_err;
    long     duplicate;
    long     seq;
    long     rssi_sum;
    long     rssi_n;
    int      rssi_min;
    int      rssi_max;
    long     last_seen_age_ms;
    long     state_changes;
    long     lost_count;
    long     recoveries;
} log_dev_t;

typedef struct {
    log_dev_t dev[LOG_MAX_DEVICES];
    int       dev_count;
    long      rows;
    char      last_state[24];
    long      transitions;
    long      lost_events;
    long      boot_count;
    char      firmware[64];
    char      boot_reason[64];
} log_summary_t;

static log_dev_t *log_dev_slot(log_summary_t *s, uint32_t id)
{
    for (int i = 0; i < s->dev_count; ++i) {
        if (s->dev[i].used && s->dev[i].device_id == id) {
            return &s->dev[i];
        }
    }
    if (s->dev_count >= LOG_MAX_DEVICES) {
        return NULL;
    }
    log_dev_t *d = &s->dev[s->dev_count++];
    memset(d, 0, sizeof(*d));
    d->used = 1;
    d->device_id = id;
    d->rssi_min = 2147483647;
    d->rssi_max = -2147483647 - 1;
    return d;
}

/** 从行里取形如 key=value 的值；找到返回指向 value 的静态缓冲 */
static int kv_get(const char *line, const char *key, char *out, size_t out_len)
{
    const size_t klen = strlen(key);
    const char *p = line;
    while ((p = strstr(p, key)) != NULL) {
        /* 要求前面是行首或分隔符，避免匹配到子串 */
        if (p != line) {
            const char c = *(p - 1);
            if (!(c == ' ' || c == '\t' || c == '[' || c == ',' || c == '(')) {
                p += klen;
                continue;
            }
        }
        if (p[klen] == '=') {
            const char *v = p + klen + 1;
            size_t n = 0;
            while (v[n] != '\0' && !isspace((unsigned char)v[n]) &&
                   v[n] != ',' && v[n] != ']' && v[n] != ')' && n + 1u < out_len) {
                out[n] = v[n];
                ++n;
            }
            out[n] = '\0';
            return (n > 0u) ? 0 : -1;
        }
        p += klen;
    }
    return -1;
}

static long kv_long(const char *line, const char *key, long dflt)
{
    char buf[64];
    long n = 0;
    if (kv_get(line, key, buf, sizeof(buf)) != 0) {
        return dflt;
    }
    if (parse_int(buf, &n) != 0) {
        return dflt;
    }
    return n;
}

static int cmd_log(int argc, char **argv)
{
    const char *path = NULL;
    const char *csv_path = NULL;
    uint32_t expect_id = 0u;
    bool have_expect = false;

    for (int i = 0; i < argc; ++i) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        long n = 0;
        if (strcmp(a, "--file") == 0 && v != NULL) {
            path = v; ++i;
        } else if (strcmp(a, "--csv") == 0 && v != NULL) {
            csv_path = v; ++i;
        } else if (strcmp(a, "--expect") == 0 && v != NULL && parse_int(v, &n) == 0) {
            expect_id = (uint32_t)n; have_expect = true; ++i;
        } else {
            printf("未知或缺少参数：%s\n", a);
            return 2;
        }
    }
    if (path == NULL) {
        printf("缺少 --file <日志文件>\n");
        return 2;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        printf("无法打开日志文件：%s\n", path);
        return 2;
    }

    log_summary_t s;
    memset(&s, 0, sizeof(s));

    char line[LINE_MAX_LEN];
    while (fgets(line, sizeof(line), f) != NULL) {
        char buf[64];

        if (kv_get(line, "firmware", buf, sizeof(buf)) == 0) {
            snprintf(s.firmware, sizeof(s.firmware), "%s", buf);
        }
        if (strstr(line, "bootreason=") != NULL) {
            ++s.boot_count;
            const char *p = strchr(line, '(');
            const char *q = (p != NULL) ? strchr(p, ')') : NULL;
            if (p != NULL && q != NULL && (size_t)(q - p) < sizeof(s.boot_reason)) {
                memcpy(s.boot_reason, p + 1, (size_t)(q - p - 1));
                s.boot_reason[q - p - 1] = '\0';
            }
        }

        /* 状态切换：支持 "状态 A -> B" 与 "state A -> B" 两种写法 */
        {
            const char *arrow = strstr(line, " -> ");
            if (arrow != NULL && (strstr(line, "状态") != NULL ||
                                  strstr(line, "state ") != NULL)) {
                ++s.transitions;
                if (strncmp(arrow + 4, "Lost", 4) == 0) {
                    ++s.lost_events;
                }
            }
        }

        /* 汇总行判据：同时含 valid= 与 state= */
        if (strstr(line, "valid=") == NULL || strstr(line, "state=") == NULL) {
            continue;
        }
        ++s.rows;

        uint32_t id = 0u;
        if (kv_get(line, "dev", buf, sizeof(buf)) == 0) {
            long n = 0;
            if (parse_int(buf, &n) == 0) {
                id = (uint32_t)n;
            }
        } else if (kv_get(line, "bound", buf, sizeof(buf)) == 0) {
            long n = 0;
            if (parse_int(buf, &n) == 0) {
                id = (uint32_t)n;
            }
        }
        log_dev_t *d = log_dev_slot(&s, id);
        if (d == NULL) {
            continue;
        }
        d->valid      = kv_long(line, "valid", d->valid);
        d->invalid    = kv_long(line, "invalid", d->invalid);
        d->crc_err    = kv_long(line, "crc_err", d->crc_err);
        d->ver_err    = kv_long(line, "ver_err", d->ver_err);
        d->len_err    = kv_long(line, "len_err", d->len_err);
        d->duplicate  = kv_long(line, "duplicate", d->duplicate);
        d->seq        = kv_long(line, "seq", d->seq);
        d->state_changes = kv_long(line, "transitions", d->state_changes);
        d->lost_count = kv_long(line, "lost", d->lost_count);
        d->recoveries = kv_long(line, "recoveries", d->recoveries);
        const long age = kv_long(line, "last_seen_age_ms", -1);
        if (age >= 0) {
            d->last_seen_age_ms = age;
        }
        if (kv_get(line, "state", buf, sizeof(buf)) == 0) {
            snprintf(s.last_state, sizeof(s.last_state), "%s", buf);
        }
        if (kv_get(line, "rssi", buf, sizeof(buf)) == 0) {
            long r = 0;
            if (parse_int(buf, &r) == 0 && r != 0) {
                d->rssi_sum += r;
                ++d->rssi_n;
                if (r < d->rssi_min) d->rssi_min = (int)r;
                if (r > d->rssi_max) d->rssi_max = (int)r;
            }
        }
    }
    fclose(f);

    printf("=====================================================================\n");
    printf("宠物活动状态灯 V1 — 链路与稳定性报告\n");
    printf("=====================================================================\n");
    if (s.rows == 0) {
        printf("未找到汇总行（需要同时包含 state= 与 valid= 的 diagnostics 输出）。\n");
        return 2;
    }
    printf("汇总行数            : %ld\n", s.rows);
    if (s.firmware[0] != '\0') {
        printf("固件版本            : %s\n", s.firmware);
    }
    if (s.boot_reason[0] != '\0') {
        printf("最近启动原因        : %s\n", s.boot_reason);
    }
    if (s.boot_count > 1) {
        printf("启动次数（掉电/重启）: %ld\n", s.boot_count);
    }
    printf("最后状态            : %s\n", s.last_state[0] ? s.last_state : "?");
    printf("状态切换累计        : %ld（需求 8.4：≥ 500 次）\n", s.transitions);
    printf("进入 Lost 累计      : %ld（需求 8.4：失联/恢复循环 ≥ 100 次）\n", s.lost_events);

    printf("---------------------------------------------------------------------\n");
    printf("设备数              : %d\n", s.dev_count);
    printf("%-12s %8s %8s %7s %7s %9s %12s\n",
           "DeviceID", "valid", "invalid", "crc错", "ver错", "RSSI中位", "失联龄ms");
    for (int i = 0; i < s.dev_count; ++i) {
        const log_dev_t *d = &s.dev[i];
        const double rssi_med = (d->rssi_n > 0) ? ((double)d->rssi_sum / (double)d->rssi_n) : 0.0;
        char idbuf[16];
        if (d->device_id != 0u) {
            snprintf(idbuf, sizeof(idbuf), "0x%08lX", (unsigned long)d->device_id);
        } else {
            snprintf(idbuf, sizeof(idbuf), "(未标注)");
        }
        printf("%-12s %8ld %8ld %7ld %7ld %9.1f %12ld\n",
               idbuf, d->valid, d->invalid, d->crc_err, d->ver_err,
               rssi_med, d->last_seen_age_ms);
    }

    printf("---------------------------------------------------------------------\n");
    for (int i = 0; i < s.dev_count; ++i) {
        const log_dev_t *d = &s.dev[i];
        const long total = d->valid + d->invalid;
        const double validity = (total > 0) ? (100.0 * (double)d->valid / (double)total) : 0.0;
        char idbuf[16];
        snprintf(idbuf, sizeof(idbuf), "0x%08lX", (unsigned long)d->device_id);
        printf("[%s] 有效包率 %.2f%%（需求 8.3：3–5 米无遮挡应接近 100%%）\n",
               idbuf, validity);
        if (d->rssi_n > 0) {
            printf("          RSSI 样本 %ld，最小/最大 %d/%d dBm\n",
                   d->rssi_n, d->rssi_min, d->rssi_max);
        }
        printf("          重复/过旧丢弃 %ld，序号 %ld，Lost %ld / 恢复 %ld\n",
               d->duplicate, d->seq, d->lost_count, d->recoveries);
        if (have_expect && d->device_id == expect_id) {
            printf("          期望绑定 0x%08lX -> PASS\n", (unsigned long)expect_id);
        }
    }
    if (have_expect) {
        int found = 0;
        for (int i = 0; i < s.dev_count; ++i) {
            if (s.dev[i].device_id == expect_id) {
                found = 1;
            }
        }
        if (!found) {
            printf("期望绑定 0x%08lX -> FAIL（日志中未出现）\n", (unsigned long)expect_id);
        }
    }
    printf("=====================================================================\n");
    printf("提示：把上面数值抄进 docs/验收测试记录模板.md 对应章节。\n");

    if (csv_path != NULL) {
        FILE *c = fopen(csv_path, "wb");
        if (c == NULL) {
            printf("无法写出 CSV：%s\n", csv_path);
            return 2;
        }
        fprintf(c, "device_id,valid,invalid,crc_err,ver_err,len_err,duplicate,seq,"
                   "rssi_min,rssi_max,rssi_avg,last_seen_age_ms,lost,recoveries\n");
        for (int i = 0; i < s.dev_count; ++i) {
            const log_dev_t *d = &s.dev[i];
            const double avg = (d->rssi_n > 0) ? ((double)d->rssi_sum / (double)d->rssi_n) : 0.0;
            fprintf(c, "0x%08lX,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%d,%d,%.1f,%ld,%ld,%ld\n",
                    (unsigned long)d->device_id, d->valid, d->invalid, d->crc_err,
                    d->ver_err, d->len_err, d->duplicate, d->seq,
                    (d->rssi_n > 0) ? d->rssi_min : 0,
                    (d->rssi_n > 0) ? d->rssi_max : 0,
                    avg, d->last_seen_age_ms, d->lost_count, d->recoveries);
        }
        fclose(c);
        printf("已写出 CSV: %s\n", csv_path);
    }
    return 0;
}

/* ================================================================== */
/* main                                                               */
/* ================================================================== */

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }
    const char *cmd = argv[1];

    if (strcmp(cmd, "vectors") == 0) {
        return petv_report_vectors();
    }
    if (strcmp(cmd, "build") == 0) {
        return cmd_build(argc - 2, argv + 2);
    }
    if (strcmp(cmd, "log") == 0) {
        return cmd_log(argc - 2, argv + 2);
    }
    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
        usage(argv[0]);
        return 0;
    }

    printf("未知子命令：%s\n\n", cmd);
    usage(argv[0]);
    return 2;
}
