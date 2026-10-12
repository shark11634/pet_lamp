/*
 * storage.c — NVS 持久化实现
 *
 * 写入策略（需求 §6.3）：只在这三种时机写 Flash
 *   1. 新设备绑定 / 解绑 / 清空
 *   2. 名字变更
 *   3. 设置变更（夜间模式、豁免时长、手动灯光）
 * 绝不在收包路径上写。
 */
#include "storage.h"

#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

#include "app_config.h"

static const char *TAG = "storage";

#define KEY_BIND  "bind"
#define KEY_NAMES "names"
#define KEY_SET   "set"

static bool s_ready;

/* ------------------------------------------------------------------ */
/* 工具                                                               */
/* ------------------------------------------------------------------ */

static esp_err_t open_nvs(nvs_handle_t *out, nvs_open_mode_t mode)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    return nvs_open(NVS_NAMESPACE, mode, out);
}

/** 读一个 blob；不存在或长度不符都返回错误（由调用方走默认值） */
static esp_err_t read_blob(const char *key, void *buf, size_t expected)
{
    nvs_handle_t h;
    esp_err_t err = open_nvs(&h, NVS_READONLY);
    if (err != ESP_OK) {
        return err;
    }
    size_t len = expected;
    err = nvs_get_blob(h, key, buf, &len);
    nvs_close(h);
    if (err == ESP_OK && len != expected) {
        ESP_LOGW(TAG, "%s 长度 %u 与期望 %u 不符，忽略记录",
                 key, (unsigned)len, (unsigned)expected);
        return ESP_ERR_INVALID_SIZE;
    }
    return err;
}

static esp_err_t write_blob(const char *key, const void *buf, size_t len)
{
    nvs_handle_t h;
    esp_err_t err = open_nvs(&h, NVS_READWRITE);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, key, buf, len);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写入 %s 失败: %s", key, esp_err_to_name(err));
    }
    return err;
}

/* ------------------------------------------------------------------ */
/* 初始化                                                             */
/* ------------------------------------------------------------------ */

esp_err_t storage_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* 分区需要重新格式化：先擦再初始化 */
        ESP_LOGW(TAG, "NVS 需要格式化（%s）", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败: %s（设置将不会被持久化）", esp_err_to_name(err));
        s_ready = false;
        return err;
    }
    s_ready = true;
    ESP_LOGI(TAG, "NVS 就绪（命名空间 '%s'）", NVS_NAMESPACE);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 绑定列表                                                           */
/* ------------------------------------------------------------------ */

esp_err_t storage_load_bindings(pet_registry_t *reg)
{
    if (reg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    pet_nvs_bindings_t b;
    memset(&b, 0, sizeof(b));

    const esp_err_t err = read_blob(KEY_BIND, &b, sizeof(b));
    if (err != ESP_OK || b.magic != PET_NVS_MAGIC) {
        ESP_LOGI(TAG, "无绑定记录（%s），等待配对",
                 (err == ESP_OK) ? "magic 不符" : esp_err_to_name(err));
        return ESP_ERR_NOT_FOUND;
    }

    /* 容量优先：决策 D6 允许网页改成 1–16 */
    if (b.max_devices >= 1u && b.max_devices <= PET_DEV_MAX_LIMIT) {
        pet_reg_set_cap(reg, b.max_devices);
    }

    uint8_t added = 0u;
    for (uint8_t i = 0; i < b.count && i < PET_DEV_MAX_LIMIT; ++i) {
        if (b.device_id[i] == 0u) {
            continue;
        }
        if (pet_reg_add(reg, b.device_id[i], 0u) < 0) {
            ESP_LOGW(TAG, "绑定记录第 %u 条超出容量，忽略", (unsigned)i);
            break;
        }
        ++added;
    }
    pet_reg_set_focus(reg, b.focus);

    ESP_LOGI(TAG, "载入绑定：%u 只（容量 %u，焦点 %s）", (unsigned)added,
             (unsigned)pet_reg_cap(reg),
             (pet_reg_focus(reg) == PET_FOCUS_MERGE) ? "合并" : "单只");
    return ESP_OK;
}

esp_err_t storage_save_bindings(const pet_registry_t *reg)
{
    if (reg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    pet_nvs_bindings_t b;
    memset(&b, 0, sizeof(b));
    b.magic = PET_NVS_MAGIC;
    b.count = pet_reg_count(reg);
    b.focus = pet_reg_focus(reg);
    b.max_devices = pet_reg_cap(reg);

    const uint8_t n = (b.count > PET_DEV_MAX_LIMIT) ? PET_DEV_MAX_LIMIT : b.count;
    for (uint8_t i = 0; i < n; ++i) {
        const pet_device_t *d = pet_reg_device(reg, i);
        b.device_id[i] = (d != NULL) ? d->device_id : 0u;
    }
    return write_blob(KEY_BIND, &b, sizeof(b));
}

/* ------------------------------------------------------------------ */
/* 名字                                                               */
/* ------------------------------------------------------------------ */

esp_err_t storage_load_names(pet_registry_t *reg)
{
    if (reg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    pet_nvs_names_t nm;
    memset(&nm, 0, sizeof(nm));

    const esp_err_t err = read_blob(KEY_NAMES, &nm, sizeof(nm));
    if (err != ESP_OK || nm.magic != PET_NVS_MAGIC) {
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t applied = 0u;
    for (uint8_t i = 0; i < nm.count && i < PET_DEV_MAX_LIMIT; ++i) {
        if (nm.device_id[i] == 0u) {
            continue;
        }
        /* 只给已绑定的设备套名字；名字为空表示用默认名 Pet-N（决策 D13） */
        if (pet_reg_find(reg, nm.device_id[i]) < 0) {
            continue;
        }
        if (pet_reg_set_name(reg, nm.device_id[i], nm.name[i]) == 0) {
            ++applied;
        }
    }
    ESP_LOGI(TAG, "载入自定义名字：%u 条", (unsigned)applied);
    return (applied > 0u) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t storage_save_names(const pet_registry_t *reg)
{
    if (reg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    pet_nvs_names_t nm;
    memset(&nm, 0, sizeof(nm));
    nm.magic = PET_NVS_MAGIC;
    nm.count = pet_reg_count(reg);

    const uint8_t n = (nm.count > PET_DEV_MAX_LIMIT) ? PET_DEV_MAX_LIMIT : nm.count;
    for (uint8_t i = 0; i < n; ++i) {
        const pet_device_t *d = pet_reg_device(reg, i);
        if (d == NULL) {
            continue;
        }
        nm.device_id[i] = d->device_id;
        memcpy(nm.name[i], d->name, PET_DEV_NAME_LEN);
        nm.name[i][PET_DEV_NAME_LEN - 1] = '\0';
    }
    return write_blob(KEY_NAMES, &nm, sizeof(nm));
}

/* ------------------------------------------------------------------ */
/* 设置                                                               */
/* ------------------------------------------------------------------ */

void storage_settings_defaults(pet_nvs_settings_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->magic = PET_NVS_MAGIC;

    pet_night_config_t cfg;
    pet_night_config_defaults(&cfg);
    storage_settings_from_night(out, &cfg);

    out->lost_timeout_ms = LOST_TIMEOUT_MS;

    /* 手动灯光默认：关灯、100%、3500K、暖白、日光模式 */
    out->manual_off = 1u;
    out->manual_on = 0u;
    out->manual_brightness = PET_BRIGHTNESS_MAX;
    out->manual_light_mode = PET_LIGHT_MODE_DAY;
    out->manual_color_temp = 3500u;
    out->manual_rgb = 0xFFC080u;
}

void storage_settings_from_night(pet_nvs_settings_t *out,
                                 const pet_night_config_t *cfg)
{
    if (out == NULL || cfg == NULL) {
        return;
    }
    out->night_enabled = cfg->enabled ? 1u : 0u;
    out->night_start = cfg->start_hour;
    out->night_end = cfg->end_hour;
    out->cap_quiet = cfg->cap_quiet;
    out->cap_light = cfg->cap_light;
    out->cap_active = cfg->cap_active;
    out->cap_lost = cfg->cap_lost;
    out->suppress_lowbat = cfg->suppress_lowbat_hint ? 1u : 0u;
    out->override_ms = cfg->override_ms;
    out->night_idle_ms = cfg->idle_infer_ms;
    out->night_throttle_ms = cfg->active_throttle_ms;
}

esp_err_t storage_load_settings(pet_nvs_settings_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    pet_nvs_settings_t s;
    memset(&s, 0, sizeof(s));

    const esp_err_t err = read_blob(KEY_SET, &s, sizeof(s));
    if (err != ESP_OK || s.magic != PET_NVS_MAGIC) {
        storage_settings_defaults(out);
        ESP_LOGI(TAG, "无设置记录，使用默认值");
        return ESP_ERR_NOT_FOUND;
    }
    *out = s;
    ESP_LOGI(TAG, "载入设置：夜间 %s %02u:00-%02u:00，豁免 %u ms",
             s.night_enabled ? "开" : "关", (unsigned)s.night_start,
             (unsigned)s.night_end, (unsigned)s.override_ms);
    return ESP_OK;
}

esp_err_t storage_save_settings(const pet_nvs_settings_t *s)
{
    if (s == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    pet_nvs_settings_t copy = *s;
    copy.magic = PET_NVS_MAGIC;
    return write_blob(KEY_SET, &copy, sizeof(copy));
}

/* ------------------------------------------------------------------ */
/* 维护                                                               */
/* ------------------------------------------------------------------ */

esp_err_t storage_clear_bindings(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    nvs_handle_t h;
    esp_err_t err = open_nvs(&h, NVS_READWRITE);
    if (err != ESP_OK) {
        return err;
    }
    /* 绑定与名字一起清；设置保留（用户重新配对后不想再调一遍夜间参数） */
    (void)nvs_erase_key(h, KEY_BIND);
    (void)nvs_erase_key(h, KEY_NAMES);
    err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "已清除全部绑定与名字（设置保留）");
    return err;
}
