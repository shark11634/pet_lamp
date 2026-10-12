/*
 * ble_scanner.c — NimBLE 被动扫描与 Manufacturer Data 提取
 *
 * 设计要点：
 *  - 只做 Observer（passive scan），不建立连接，不依赖手机/云（需求 §2.1）
 *  - 广播回调只做最小工作（AD 遍历 + 复制 + 入队），解析交给业务层
 *  - AD 结构按长度字节遍历，容错处理截断与异常长度
 */
#include "ble_scanner.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"

#include "app_config.h"

static const char *TAG = "ble";

static QueueHandle_t     s_queue;
static bool              s_started;
static uint8_t           s_own_addr_type;
static ble_scanner_stats_t s_stats;
static int8_t            s_last_rssi;

/* ------------------------------------------------------------------ */
/* AD 结构解析                                                        */
/* ------------------------------------------------------------------ */

/**
 * 遍历 AD 结构，查找 Manufacturer Specific Data（0xFF）。
 *
 * @param out       找到时指向 mfg 数据（AD Type 之后的字节，含 CompanyID）
 * @return 数据长度；0 表示未找到或长度异常
 */
static uint8_t find_mfg_data(const uint8_t *data, uint8_t len, const uint8_t **out)
{
    const uint8_t *p = data;
    uint8_t remaining = len;

    while (remaining >= 2u) {
        const uint8_t field_len = p[0];
        if (field_len == 0u) {
            break;   /* 填充开始 */
        }
        /* field_len 含 type 字节，因此必须 field_len <= remaining - 1 */
        if (field_len > (uint8_t)(remaining - 1u)) {
            ESP_LOGW(TAG, "AD 结构长度异常：field_len=%u remaining=%u",
                     field_len, remaining);
            return 0u;
        }
        const uint8_t type = p[1];
        const uint8_t value_len = (uint8_t)(field_len - 1u);

        if (type == PET_AD_TYPE_MANUFACTURER) {
            if (value_len < 2u) {
                return 0u;   /* 连 Company ID 都不够 */
            }
            *out = &p[2];
            return value_len;
        }

        p += (uint8_t)(field_len + 1u);
        remaining = (uint8_t)(remaining - field_len - 1u);
    }
    return 0u;
}

/* ------------------------------------------------------------------ */
/* NimBLE 回调                                                        */
/* ------------------------------------------------------------------ */

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    if (event->type != BLE_GAP_EVENT_DISC) {
        return 0;
    }
    ++s_stats.adv_total;

    const uint8_t *mfg = NULL;
    const uint8_t mfg_len = find_mfg_data(event->disc.data, event->disc.length_data,
                                         &mfg);
    if (mfg_len == 0u || mfg == NULL) {
        return 0;
    }
    ++s_stats.mfg_found;

    /* 我们只关心前 PET_MFG_DATA_LEN 字节（16 字节）；
     * 多出来的字节属于厂商私有扩展，忽略而不是拒收。 */
    const uint8_t copy_len = (mfg_len > PET_MFG_DATA_LEN)
                                 ? (uint8_t)PET_MFG_DATA_LEN
                                 : mfg_len;
    if (copy_len != PET_MFG_DATA_LEN) {
        ++s_stats.mfg_dropped;
        ESP_LOGD(TAG, "MFG 长度 %u（期望 %u），丢弃", copy_len,
                 (unsigned)PET_MFG_DATA_LEN);
        return 0;
    }

    ble_raw_adv_t raw;
    memset(&raw, 0, sizeof(raw));
    memcpy(raw.mfg, mfg, PET_MFG_DATA_LEN);
    raw.mfg_len = PET_MFG_DATA_LEN;
    raw.rssi = (int8_t)event->disc.rssi;
    raw.rx_time_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    memcpy(raw.addr, event->disc.addr.val, 6);

    s_last_rssi = raw.rssi;

    /* 队列满时丢弃最旧的，保证不阻塞 NimBLE 主机任务 */
    if (xQueueSend(s_queue, &raw, 0) != pdTRUE) {
        ble_raw_adv_t dropped;
        (void)xQueueReceive(s_queue, &dropped, 0);
        (void)xQueueSend(s_queue, &raw, 0);
        ++s_stats.queue_dropped;
        ESP_LOGD(TAG, "扫描队列已满，丢弃最旧广播");
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* NimBLE 生命周期                                                     */
/* ------------------------------------------------------------------ */

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr 失败: %d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto 失败: %d", rc);
    }
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE 复位: %d", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();          /* 直到 nimble_port_stop() */
    nimble_port_freertos_deinit();
}

esp_err_t ble_scanner_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    s_queue = xQueueCreate(BLE_QUEUE_LEN, sizeof(ble_raw_adv_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(&s_stats, 0, sizeof(s_stats));
    s_last_rssi = 0;

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init 失败: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    nimble_port_freertos_init(host_task);

    /* 等主机同步；超时也继续尝试启动扫描，失败会返回错误码 */
    uint32_t waited = 0u;
    while (!ble_hs_synced() && waited < 3000u) {
        vTaskDelay(pdMS_TO_TICKS(50));
        waited += 50u;
    }
    if (!ble_hs_synced()) {
        ESP_LOGW(TAG, "NimBLE 主机未在 3 秒内同步");
    }

    struct ble_gap_disc_params params;
    memset(&params, 0, sizeof(params));
    params.passive = 1;              /* 被动扫描：不发 SCAN_REQ */
    params.itvl = 0;                 /* 0 = 控制器默认 */
    params.window = 0;
    params.filter_duplicates = 0;    /* 自行按 Sequence 去重，需要看到重复包做重启同步 */
    params.filter_policy = 0;
    params.limited = 0;

    const int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &params,
                               gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "启动被动扫描失败: %d", rc);
        return ESP_FAIL;
    }

    s_started = true;
    ESP_LOGI(TAG, "被动扫描已启动（passive observer，不连接；队列 %u 条）",
             (unsigned)BLE_QUEUE_LEN);
    return ESP_OK;
}

bool ble_scanner_wait(ble_raw_adv_t *out, uint32_t timeout_ms)
{
    if (out == NULL || s_queue == NULL) {
        return false;
    }
    return (xQueueReceive(s_queue, out, pdMS_TO_TICKS(timeout_ms)) == pdTRUE);
}

void ble_scanner_get_stats(ble_scanner_stats_t *stats)
{
    if (stats != NULL) {
        *stats = s_stats;
    }
}

int8_t ble_scanner_last_rssi(bool *valid)
{
    if (valid != NULL) {
        *valid = (s_stats.mfg_found > 0u);
    }
    return s_last_rssi;
}

void ble_scanner_stop(void)
{
    if (!s_started) {
        return;
    }
    (void)ble_gap_disc_cancel();
    (void)nimble_port_stop();
    s_started = false;
    ESP_LOGI(TAG, "扫描已停止");
}
