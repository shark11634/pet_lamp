/*
 * device_registry.h — 多宠物设备表与状态合并（平台无关）
 *
 * 需求依据：
 *   - §1.3 非目标：不要求多宠物，"但协议应预留扩展空间" → 协议本身已有 DeviceID，
 *     灯端只需维护一张表即可支持多狗，**协议零改动**
 *   - §4.6 灯端处理流程：DeviceID 绑定校验、Sequence 去重/回绕、last_seen 刷新
 *   - 决策 D6：容量编译期默认 8，运行期可配 1–16
 *   - 决策 D7：合并规则 = 最活跃者优先（Active > Light > Quiet）
 *   - 决策 O5：焦点锁定时不提示其他狗，但 Lost 判断仍看全部
 *
 * 关键正确性要求：**每只狗独立计时**。若只维护一个全局 last_seen，
 * "一只狗被带出门、另一只在家"会错误地让整盏灯进入 Lost。
 */
#ifndef DEVICE_REGISTRY_H
#define DEVICE_REGISTRY_H

#include "pet_config.h"
#include "pet_protocol.h"
#include "pet_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 单只设备记录                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t device_id;        /**< 协议内唯一标识 */
    bool     bound;            /**< 是否在绑定列表（配对模式加入） */
    bool     heard;            /**< 是否曾收到过合法包 */
    uint32_t last_seen_ms;     /**< 该设备最后一次有效包时间（独立计时） */
    uint16_t last_sequence;
    bool     has_sequence;
    uint8_t  seq_resync_count; /**< 连续重复序号计数，用于重启重新同步 */
    uint8_t  activity_state;   /**< pet_activity_state_t */
    uint8_t  activity_score;
    uint8_t  battery_percent;
    uint8_t  flags;
    int8_t   rssi;
    uint32_t last_change_ms;   /**< 该设备上一次状态变化时间 */
    char     name[PET_DEV_NAME_LEN];  /**< UTF-8 狗名；空串表示未命名 */
    /* 生命周期统计（网页显示，不持久化） */
    uint32_t packets_total;
    uint32_t active_ms_total;
    uint32_t light_ms_total;
    uint32_t quiet_ms_total;
    uint32_t first_seen_ms;
} pet_device_t;

/* ------------------------------------------------------------------ */
/* 注册表                                                             */
/* ------------------------------------------------------------------ */

/** 特殊焦点值：合并显示（决策 D7 默认） */
#define PET_FOCUS_MERGE 0xFFu

typedef struct {
    pet_device_t dev[PET_DEV_MAX_LIMIT];
    uint8_t      cap;          /**< 运行期容量 1..PET_DEV_MAX_LIMIT */
    uint8_t      count;        /**< 已绑定数量 */
    uint8_t      focus_index;  /**< PET_FOCUS_MERGE 或 slot 下标 */
    /* 累计统计 */
    uint32_t     total_valid;  /**< 有效包总数 */
    uint32_t     total_dup;    /**< 因序号重复/过旧丢弃数 */
    uint32_t     total_unbound;/**< 因未绑定丢弃数 */
} pet_registry_t;

/** 初始化；cap 会被钳到 1..PET_DEV_MAX_LIMIT */
void pet_reg_init(pet_registry_t *r, uint8_t cap);

/** 设置运行期容量（决策 D6）。返回实际生效值。缩小容量不会踢掉已有设备。 */
uint8_t pet_reg_set_cap(pet_registry_t *r, uint8_t cap);

/** 按 DeviceID 找 slot；未找到返回 -1 */
int8_t pet_reg_find(const pet_registry_t *r, uint32_t device_id);

/**
 * 加入一个新设备（配对模式用）。
 * @return 新 slot 下标；表满或已存在返回 -1 / 已存在下标由 find 处理
 */
int8_t pet_reg_add(pet_registry_t *r, uint32_t device_id, uint32_t now_ms);

/** 解绑并清空该 slot，后续 slot 前移以保持紧凑 */
bool pet_reg_remove(pet_registry_t *r, uint32_t device_id);

/** 清空全部绑定（维护模式用） */
void pet_reg_clear(pet_registry_t *r);

/** 是否已绑定该 DeviceID */
bool pet_reg_is_bound(const pet_registry_t *r, uint32_t device_id);

/** 已绑定数量 */
uint8_t pet_reg_count(const pet_registry_t *r);

/** 运行期容量（决策 D6：默认 8，可配 1–16） */
uint8_t pet_reg_cap(const pet_registry_t *r);

/** 当前焦点：PET_FOCUS_MERGE 或 slot 下标 */
uint8_t pet_reg_focus(const pet_registry_t *r);

/** 取 slot 内的设备；越界返回 NULL。平台层（NVS 持久化、网页）用。 */
const pet_device_t *pet_reg_device(const pet_registry_t *r, uint8_t index);

/** 设置焦点：PET_FOCUS_MERGE 或有效 slot 下标。返回是否接受。 */
bool pet_reg_set_focus(pet_registry_t *r, uint8_t index);

/** 设置某设备名字（UTF-8，超长截断；空串表示恢复未命名） */
bool pet_reg_set_name(pet_registry_t *r, uint32_t device_id, const char *name);

/** 取某设备名字；未命名时填入默认名 "Pet-N" */
void pet_reg_display_name(const pet_registry_t *r, uint8_t index,
                          char *out, size_t out_len);

/* ------------------------------------------------------------------ */
/* 收包                                                                */
/* ------------------------------------------------------------------ */

typedef enum {
    PET_RX_ACCEPTED = 0,       /**< 接受，已刷新 last_seen */
    PET_RX_ERR_UNBOUND,        /**< DeviceID 不在绑定列表 */
    PET_RX_ERR_STALE,          /**< 序号重复或过旧 */
    PET_RX_ERR_NO_SLOT,        /**< 表满，无法接受新设备 */
} pet_reg_result_t;

/**
 * 处理一个**已通过协议字段校验**的包。
 *
 * 内部完成需求 4.6 剩余步骤：绑定校验 → 序号判定（含回绕与重启重新同步）
 * → 刷新该设备 last_seen / 字段 / 统计。
 *
 * @param allow_new 未绑定时是否允许自动加入（配对模式为 true，正常监听为 false）
 */
pet_reg_result_t pet_reg_on_packet(pet_registry_t *r, const pet_packet_t *pkt,
                                   uint32_t now_ms, bool allow_new);

/** 周期性 tick（1 Hz）：累计各状态驻留时长 */
void pet_reg_tick(pet_registry_t *r, uint32_t now_ms);

/* ------------------------------------------------------------------ */
/* 合并                                                                */
/* ------------------------------------------------------------------ */

/** 该设备当前是否"在册"（距最后有效包 < lost_timeout_ms） */
bool pet_reg_device_alive(const pet_device_t *d, uint32_t now_ms,
                          uint32_t lost_timeout_ms);

/**
 * 合并所有在册设备的状态：
 *   - 决策 D7：最活跃者优先（Active > Light > Quiet）
 *   - 相同状态取 activity_score 更高者
 *   - 决策 O5：焦点锁定时只看该设备，但**只要还有任何设备在册**就不进 Lost
 *   - **所有**设备都超时才返回 PET_LAMP_LOST
 *
 * @param source_index 输出被选中设备的 slot 下标；全部 Lost 时为 0xFF
 * @return 合并后的灯端状态
 */
pet_lamp_state_t pet_reg_merge(const pet_registry_t *r, uint32_t now_ms,
                               uint32_t lost_timeout_ms, uint8_t *source_index);

/** 全部在册设备的置信度最高活动分（0–255） */
uint8_t pet_reg_merged_score(const pet_registry_t *r, uint8_t source_index);

/** 任一在册设备低电量 */
bool pet_reg_any_low_battery(const pet_registry_t *r, uint32_t now_ms,
                             uint32_t lost_timeout_ms);

/** 任一在册设备报 SENSOR_ERROR */
bool pet_reg_any_sensor_error(const pet_registry_t *r, uint32_t now_ms,
                              uint32_t lost_timeout_ms);

/** 距最近一次"任意设备"有效包的时间；从未收到返回 UINT64_MAX */
uint64_t pet_reg_lost_age_ms(const pet_registry_t *r, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* DEVICE_REGISTRY_H */
