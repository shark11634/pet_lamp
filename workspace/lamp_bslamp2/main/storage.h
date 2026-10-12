/*
 * storage.h — NVS 持久化（多狗版）
 *
 * 需求依据：
 *   §6.2 storage 模块
 *   §6.3 写入策略：**仅在绑定或设置改变时写 Flash**，不要每收一包就写
 *   §7.2 首次配对后绑定 DeviceID
 *   决策 D5：每只狗可命名（名字也要持久化）
 *   决策 D6：设备表容量可配 1–16
 *
 * 三块数据分开存，避免"改一个名字重写整张表"：
 *   PETLAMP/bind   绑定列表 + 焦点 + 容量
 *   PETLAMP/names  每只狗的自定义名字
 *   PETLAMP/set    夜间模式 / 豁免时长 / 失联超时 / 手动灯光状态
 *
 * 所有结构带 magic 字段：布局一旦变更就递增 magic，读到旧版本直接当"无记录"
 * 处理（宁可回到默认值，也不要按错位的字段解释二进制）。
 */
#ifndef STORAGE_H
#define STORAGE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "device_registry.h"   /* core：pet_registry_t / PET_DEV_* */
#include "night_mode.h"        /* core：pet_night_config_t */
#include "pet_config.h"        /* core：LOST_TIMEOUT_MS 等默认值 */

#ifdef __cplusplus
extern "C" {
#endif

/** 布局版本；改动任一 blob 的字段顺序/类型就要 +1 */
#define PET_NVS_MAGIC  0x50455431u   /* 'PET1' */

/** 绑定列表 */
typedef struct {
    uint32_t magic;
    uint8_t  count;
    uint8_t  focus;                  /**< PET_FOCUS_MERGE = 合并显示 */
    uint8_t  max_devices;            /**< 运行期容量 1–PET_DEV_MAX_LIMIT */
    uint8_t  reserved;
    uint32_t device_id[PET_DEV_MAX_LIMIT];
} pet_nvs_bindings_t;

/** 每只狗的自定义名字 */
typedef struct {
    uint32_t magic;
    uint8_t  count;
    uint8_t  reserved[3];
    uint32_t device_id[PET_DEV_MAX_LIMIT];
    char     name[PET_DEV_MAX_LIMIT][PET_DEV_NAME_LEN];
} pet_nvs_names_t;

/** 运行期设置 + 手动灯光状态 */
typedef struct {
    uint32_t magic;
    /* 夜间模式 */
    uint8_t  night_enabled;
    uint8_t  night_start;            /**< 0–23 */
    uint8_t  night_end;              /**< 0–23 */
    uint8_t  cap_quiet;
    uint8_t  cap_light;
    uint8_t  cap_active;
    uint8_t  cap_lost;
    uint8_t  suppress_lowbat;
    /* 时长 */
    uint32_t lost_timeout_ms;
    uint32_t override_ms;
    uint32_t night_idle_ms;
    uint32_t night_throttle_ms;
    /* 手动灯光（对齐米家属性模型） */
    uint8_t  manual_off;
    uint8_t  manual_on;
    uint8_t  manual_brightness;      /**< 1–100 */
    uint8_t  manual_light_mode;      /**< PET_LIGHT_MODE_COLOR / DAY */
    uint32_t manual_color_temp;      /**< 1700–6500 K */
    uint32_t manual_rgb;             /**< 1–16777215 */
} pet_nvs_settings_t;

/** 初始化 NVS 分区（失败不致命：全部走默认值，只是不能持久化） */
esp_err_t storage_init(void);

/* ---- 绑定列表 ---- */
/** 载入绑定列表；无记录时把 registry 置为默认状态并返回 ESP_ERR_NOT_FOUND */
esp_err_t storage_load_bindings(pet_registry_t *reg);
esp_err_t storage_save_bindings(const pet_registry_t *reg);

/* ---- 名字 ---- */
esp_err_t storage_load_names(pet_registry_t *reg);
esp_err_t storage_save_names(const pet_registry_t *reg);

/* ---- 设置 ---- */
esp_err_t storage_load_settings(pet_nvs_settings_t *out);
esp_err_t storage_save_settings(const pet_nvs_settings_t *s);

/** 用 core 的默认值填充设置结构（载入失败时使用） */
void storage_settings_defaults(pet_nvs_settings_t *out);

/** 把 core 的夜间配置写入设置结构（保存前调用） */
void storage_settings_from_night(pet_nvs_settings_t *out,
                                 const pet_night_config_t *cfg);

/** 维护模式：清除全部绑定（保留设置） */
esp_err_t storage_clear_bindings(void);

#ifdef __cplusplus
}
#endif

#endif /* STORAGE_H */
