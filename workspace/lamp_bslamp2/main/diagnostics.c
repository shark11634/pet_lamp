/*
 * diagnostics.c — 可观测性日志（需求 §8.5）
 *
 * 输出格式刻意做成**一行一个字段组**且带稳定前缀，这样
 * tools/pet_tool.exe log 能直接解析出有效包率与 RSSI 报告：
 *
 *   I (12345) app: DIAG valid=812 dup=17 unbound=3 ad=1043 mfg=845 \
 *                  crc=0 ver=0 len=2 rssi=-63 seq=515 age=412 \
 *                  state=Active src=merge caps=18 lost=1
 *
 * 字段顺序一旦变化要同步更新 tools/pet_tool.c 的解析器。
 */
#include "diagnostics.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "app_config.h"
#include "ble_scanner.h"
#include "lamp_effects.h"

static const char *TAG = "diag";

static pet_registry_t           *s_reg;
static pet_night_t              *s_night;
static const pet_nvs_settings_t *s_settings;
static pet_rx_stats_t            s_rx;          /* 解析层累计（含失败） */
static uint32_t                  s_last_report_ms;
static uint32_t                  s_lost_transitions;

void diagnostics_init(pet_registry_t *reg, pet_night_t *night,
                      const pet_nvs_settings_t *settings)
{
    s_reg = reg;
    s_night = night;
    s_settings = settings;
    memset(&s_rx, 0, sizeof(s_rx));
    s_last_report_ms = 0u;
    s_lost_transitions = 0u;
    diagnostics_log_boot();
}

void diagnostics_add_rx_stats(const pet_rx_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }
    s_rx.valid_packets      += stats->valid_packets;
    s_rx.invalid_packets    += stats->invalid_packets;
    s_rx.crc_errors         += stats->crc_errors;
    s_rx.version_errors     += stats->version_errors;
    s_rx.length_errors      += stats->length_errors;
    s_rx.company_id_errors  += stats->company_id_errors;
    s_rx.duplicate_packets  += stats->duplicate_packets;
}

/** 夜间状态的可读标签（core 只给布尔值，标签属于展示层） */
static const char *night_label(const pet_night_t *n, uint32_t now_ms)
{
    if (n == NULL) {
        return "?";
    }
    if (!n->cfg.enabled) {
        return "Off";
    }
    if (n->manual_mode) {
        return "Manual";
    }
    return pet_night_is_night(n) ? "Night" : "Day";
}

void diagnostics_log_boot(void)
{
    const esp_reset_reason_t reason = esp_reset_reason();
    const char *name;
    switch (reason) {
        case ESP_RST_POWERON:  name = "POWERON";  break;
        case ESP_RST_EXT:      name = "EXT";      break;
        case ESP_RST_SW:       name = "SW";       break;
        case ESP_RST_PANIC:    name = "PANIC";    break;
        case ESP_RST_INT_WDT:  name = "INT_WDT";  break;
        case ESP_RST_TASK_WDT: name = "TASK_WDT"; break;
        case ESP_RST_WDT:      name = "WDT";      break;
        case ESP_RST_BROWNOUT: name = "BROWNOUT"; break;
        case ESP_RST_DEEPSLEEP:name = "DEEPSLEEP";break;
        default:               name = "UNKNOWN";  break;
    }
    ESP_LOGI(TAG, "BOOT reason=%s(%d) fw=%s free_heap=%" PRIu32,
             name, (int)reason, FIRMWARE_VERSION,
             (uint32_t)esp_get_free_heap_size());
}

/**
 * 计算"最近一次收到有效包"的年龄。
 * 返回 UINT32_MAX 表示从未收到过。
 */
static uint32_t merged_age_ms(const pet_registry_t *reg, uint32_t now_ms,
                              uint8_t *out_source)
{
    uint8_t src = PET_FOCUS_MERGE;
    (void)pet_reg_merge(reg, now_ms, s_settings ? s_settings->lost_timeout_ms
                                               : LOST_TIMEOUT_MS,
                        &src);
    if (out_source != NULL) {
        *out_source = src;
    }
    const uint64_t age = pet_reg_lost_age_ms(reg, now_ms);
    if (age == UINT64_MAX) {
        return UINT32_MAX;
    }
    return (age > 0xFFFFFFFEull) ? 0xFFFFFFFEu : (uint32_t)age;
}

void diagnostics_report_now(uint32_t now_ms, const pet_registry_t *reg,
                            const pet_night_t *night, uint32_t lost_timeout_ms)
{
    if (reg == NULL || night == NULL) {
        return;
    }

    uint8_t src = PET_FOCUS_MERGE;
    const uint32_t age = merged_age_ms(reg, now_ms, &src);
    const pet_lamp_state_t st =
        pet_reg_merge(reg, now_ms, lost_timeout_ms, NULL);

    if (st == PET_LAMP_LOST) {
        ++s_lost_transitions;
    }

    /* AD 层计数（ble_scanner） */
    ble_scanner_stats_t bs;
    ble_scanner_get_stats(&bs);
    bool rssi_valid = false;
    const int8_t rssi = ble_scanner_last_rssi(&rssi_valid);

    /* 焦点设备的序号 / RSSI 快照 */
    uint32_t seq = 0u;
    int rssi_dev = 0;
    if (src != PET_FOCUS_MERGE && src < pet_reg_count(reg)) {
        const pet_device_t *d = pet_reg_device(reg, src);
        if (d != NULL) {
            seq = d->last_sequence;
            rssi_dev = d->rssi;
        }
    }

    ESP_LOGI(TAG,
             "DIAG valid=%" PRIu32 " dup=%" PRIu32 " unbound=%" PRIu32
             " ad=%" PRIu32 " mfg=%" PRIu32
             " crc=%" PRIu32 " ver=%" PRIu32 " len=%" PRIu32
             " rssi=%d seq=%" PRIu32 " age=%s"
             " state=%s src=%s caps=%u lost=%" PRIu32 " night=%s",
             s_rx.valid_packets, reg->total_dup, reg->total_unbound,
             bs.adv_total, bs.mfg_found,
             s_rx.crc_errors, s_rx.version_errors, s_rx.length_errors,
             rssi_valid ? (int)rssi : rssi_dev,
             (unsigned)seq,
             (age == UINT32_MAX) ? "never" : "ms",
             lamp_state_name(st),
             (src == PET_FOCUS_MERGE) ? "merge" : "one",
             (unsigned)pet_reg_cap(reg), s_lost_transitions,
             night_label(night, now_ms));

    if (age != UINT32_MAX) {
        /* 单独一行，方便 tools/pet_tool.c 用固定正则抓 age_ms */
        ESP_LOGI(TAG, "DIAG_AGE ms=%" PRIu32, age);
    }

    if (src != PET_FOCUS_MERGE && src < pet_reg_count(reg)) {
        const pet_device_t *d = pet_reg_device(reg, src);
        if (d != NULL) {
            char name[PET_DEV_NAME_LEN + 8];
            pet_reg_display_name(reg, src, name, sizeof(name));
            ESP_LOGI(TAG,
                     "DIAG_DEV name=\"%s\" id=0x%08X battery=%u flags=0x%02X"
                     " packets=%" PRIu32 " resync=%u",
                     name, (unsigned)d->device_id, (unsigned)d->battery_percent,
                     (unsigned)d->flags, d->packets_total,
                     (unsigned)d->seq_resync_count);
        }
    }
}

void diagnostics_maybe_report(uint32_t now_ms, const pet_registry_t *reg,
                              const pet_night_t *night, uint32_t lost_timeout_ms)
{
    if (s_last_report_ms != 0u &&
        (uint32_t)(now_ms - s_last_report_ms) < TASK_DIAG_INTERVAL_MS) {
        return;
    }
    s_last_report_ms = now_ms;
    diagnostics_report_now(now_ms, reg, night, lost_timeout_ms);
}

void diagnostics_report_pairing_start(uint32_t window_ms)
{
    ESP_LOGI(TAG, "PAIRING start window_ms=%" PRIu32
                  " （佩戴端上电后 60 秒内会置 PAIR_REQUEST）",
             window_ms);
}

void diagnostics_report_pairing_end(uint8_t bound_count)
{
    ESP_LOGI(TAG, "PAIRING end bound=%u", (unsigned)bound_count);
}
