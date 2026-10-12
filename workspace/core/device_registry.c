/*
 * device_registry.c — 多宠物设备表与状态合并实现
 */
#include "device_registry.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* 内部工具                                                           */
/* ------------------------------------------------------------------ */

static void dev_clear(pet_device_t *d)
{
    memset(d, 0, sizeof(*d));
}

static uint8_t clamp_cap(uint8_t cap)
{
    if (cap < 1u) {
        return 1u;
    }
    if (cap > PET_DEV_MAX_LIMIT) {
        return (uint8_t)PET_DEV_MAX_LIMIT;
    }
    return cap;
}

/** 状态优先级：Active(2) > Light(1) > Quiet(0) */
static uint8_t state_priority(uint8_t activity_state)
{
    switch (activity_state) {
    case PET_STATE_ACTIVE: return 2u;
    case PET_STATE_LIGHT:  return 1u;
    default:               return 0u;
    }
}

static pet_lamp_state_t lamp_state_of(uint8_t activity_state)
{
    switch (activity_state) {
    case PET_STATE_ACTIVE: return PET_LAMP_ACTIVE;
    case PET_STATE_LIGHT:  return PET_LAMP_LIGHT;
    case PET_STATE_QUIET:  return PET_LAMP_QUIET;
    default:               return PET_LAMP_LOST;   /* 未知值不应到达此处 */
    }
}

/** 把 slot 从 idx 起整体前移，腾出尾部空位 */
static void compact_from(pet_registry_t *r, uint8_t idx)
{
    for (uint8_t i = idx; i + 1u < r->count; ++i) {
        r->dev[i] = r->dev[i + 1u];
    }
    if (r->count > 0u) {
        --r->count;
        dev_clear(&r->dev[r->count]);
    }
    /* 焦点索引随之修正 */
    if (r->focus_index != PET_FOCUS_MERGE) {
        if (r->focus_index == idx) {
            r->focus_index = PET_FOCUS_MERGE;
        } else if (r->focus_index > idx) {
            --r->focus_index;
        }
        if (r->focus_index >= r->count) {
            r->focus_index = PET_FOCUS_MERGE;
        }
    }
}

/* ------------------------------------------------------------------ */
/* 生命周期                                                           */
/* ------------------------------------------------------------------ */

void pet_reg_init(pet_registry_t *r, uint8_t cap)
{
    if (r == NULL) {
        return;
    }
    memset(r, 0, sizeof(*r));
    r->cap = clamp_cap(cap);
    r->focus_index = PET_FOCUS_MERGE;
}

uint8_t pet_reg_set_cap(pet_registry_t *r, uint8_t cap)
{
    if (r == NULL) {
        return 0u;
    }
    r->cap = clamp_cap(cap);
    /* 容量缩小时不踢已有设备，只是不允许再加新设备 */
    return r->cap;
}

int8_t pet_reg_find(const pet_registry_t *r, uint32_t device_id)
{
    if (r == NULL) {
        return -1;
    }
    for (uint8_t i = 0; i < r->count; ++i) {
        if (r->dev[i].bound && r->dev[i].device_id == device_id) {
            return (int8_t)i;
        }
    }
    return -1;
}

int8_t pet_reg_add(pet_registry_t *r, uint32_t device_id, uint32_t now_ms)
{
    if (r == NULL || device_id == 0u) {
        return -1;
    }
    const int8_t existing = pet_reg_find(r, device_id);
    if (existing >= 0) {
        return existing;
    }
    if (r->count >= r->cap || r->count >= PET_DEV_MAX_LIMIT) {
        return -1;   /* 表满：上层应提示用户 */
    }

    const uint8_t idx = r->count;
    dev_clear(&r->dev[idx]);
    r->dev[idx].device_id = device_id;
    r->dev[idx].bound = true;
    r->dev[idx].first_seen_ms = now_ms;
    ++r->count;
    return (int8_t)idx;
}

bool pet_reg_remove(pet_registry_t *r, uint32_t device_id)
{
    if (r == NULL) {
        return false;
    }
    const int8_t idx = pet_reg_find(r, device_id);
    if (idx < 0) {
        return false;
    }
    compact_from(r, (uint8_t)idx);
    return true;
}

void pet_reg_clear(pet_registry_t *r)
{
    if (r == NULL) {
        return;
    }
    const uint8_t cap = r->cap;
    const uint32_t v = r->total_valid;
    const uint32_t d = r->total_dup;
    const uint32_t u = r->total_unbound;
    memset(r, 0, sizeof(*r));
    r->cap = cap;
    r->focus_index = PET_FOCUS_MERGE;
    r->total_valid = v;
    r->total_dup = d;
    r->total_unbound = u;
}

bool pet_reg_is_bound(const pet_registry_t *r, uint32_t device_id)
{
    return pet_reg_find(r, device_id) >= 0;
}

uint8_t pet_reg_count(const pet_registry_t *r)
{
    return (r != NULL) ? r->count : 0u;
}

uint8_t pet_reg_cap(const pet_registry_t *r)
{
    return (r != NULL) ? r->cap : 0u;
}

uint8_t pet_reg_focus(const pet_registry_t *r)
{
    return (r != NULL) ? r->focus_index : PET_FOCUS_MERGE;
}

const pet_device_t *pet_reg_device(const pet_registry_t *r, uint8_t index)
{
    if (r == NULL || index >= r->count) {
        return NULL;
    }
    return &r->dev[index];
}

bool pet_reg_set_focus(pet_registry_t *r, uint8_t index)
{
    if (r == NULL) {
        return false;
    }
    if (index == PET_FOCUS_MERGE) {
        r->focus_index = PET_FOCUS_MERGE;
        return true;
    }
    if (index >= r->count) {
        return false;
    }
    r->focus_index = index;
    return true;
}

bool pet_reg_set_name(pet_registry_t *r, uint32_t device_id, const char *name)
{
    if (r == NULL) {
        return false;
    }
    const int8_t idx = pet_reg_find(r, device_id);
    if (idx < 0) {
        return false;
    }
    pet_device_t *d = &r->dev[idx];
    if (name == NULL || name[0] == '\0') {
        d->name[0] = '\0';
        return true;
    }
    /* 截断到 PET_DEV_NAME_LEN-1 字节，并保证不在 UTF-8 字符中间截断 */
    size_t n = strlen(name);
    if (n > PET_DEV_NAME_LEN - 1u) {
        n = PET_DEV_NAME_LEN - 1u;
        /* 回退到上一个非续接字节（UTF-8 续接字节形如 10xxxxxx） */
        while (n > 0u && ((unsigned char)name[n] & 0xC0u) == 0x80u) {
            --n;
        }
    }
    memcpy(d->name, name, n);
    d->name[n] = '\0';
    return true;
}

void pet_reg_display_name(const pet_registry_t *r, uint8_t index,
                          char *out, size_t out_len)
{
    if (out == NULL || out_len == 0u) {
        return;
    }
    out[0] = '\0';
    if (r == NULL || index >= r->count) {
        return;
    }
    if (r->dev[index].name[0] != '\0') {
        strncpy(out, r->dev[index].name, out_len - 1u);
        out[out_len - 1u] = '\0';
        return;
    }
    /* 默认名：Pet-1 … Pet-N（决策 O3：按加入顺序编号） */
    snprintf(out, out_len, "Pet-%u", (unsigned)(index + 1u));
}

/* ------------------------------------------------------------------ */
/* 收包                                                               */
/* ------------------------------------------------------------------ */

pet_reg_result_t pet_reg_on_packet(pet_registry_t *r, const pet_packet_t *pkt,
                                   uint32_t now_ms, bool allow_new)
{
    if (r == NULL || pkt == NULL) {
        return PET_RX_ERR_UNBOUND;
    }

    int8_t idx = pet_reg_find(r, pkt->device_id);
    if (idx < 0) {
        if (!allow_new) {
            ++r->total_unbound;
            return PET_RX_ERR_UNBOUND;
        }
        idx = pet_reg_add(r, pkt->device_id, now_ms);
        if (idx < 0) {
            return PET_RX_ERR_NO_SLOT;
        }
    }

    pet_device_t *d = &r->dev[idx];

    /* --- Sequence 去重 / 回绕 / 重启重新同步（需求 4.6） ------------- */
    if (d->has_sequence) {
        if (pet_seq_is_newer(pkt->sequence, d->last_sequence)) {
            d->seq_resync_count = 0u;
        } else if (pkt->sequence == d->last_sequence) {
            if ((uint8_t)(d->seq_resync_count + 1u) >= SEQ_RESYNC_PACKETS) {
                d->seq_resync_count = 0u;   /* 连续 N 个合法包后放宽一次 */
            } else {
                ++d->seq_resync_count;
                ++r->total_dup;
                return PET_RX_ERR_STALE;
            }
        } else {
            d->seq_resync_count = 0u;
            ++r->total_dup;
            return PET_RX_ERR_STALE;
        }
    }

    /* --- 接受：刷新字段 --------------------------------------------- */
    const bool had_sequence = d->has_sequence;
    const uint8_t prev_state = d->activity_state;
    d->last_sequence = pkt->sequence;
    d->has_sequence = true;
    d->last_seen_ms = now_ms;
    d->heard = true;
    d->activity_state = pkt->activity_state;
    d->activity_score = pkt->activity_score;
    d->battery_percent = pkt->battery_percent;
    d->flags = pkt->flags;
    d->rssi = pkt->rssi;
    ++d->packets_total;
    ++r->total_valid;
    if (!had_sequence || prev_state != pkt->activity_state) {
        d->last_change_ms = now_ms;
    }

    return PET_RX_ACCEPTED;
}

void pet_reg_tick(pet_registry_t *r, uint32_t now_ms)
{
    if (r == NULL) {
        return;
    }
    (void)now_ms;
    const uint32_t step = TASK_STATE_INTERVAL_MS;   /* 1 Hz */
    for (uint8_t i = 0; i < r->count; ++i) {
        pet_device_t *d = &r->dev[i];
        if (!d->heard) {
            continue;
        }
        switch (d->activity_state) {
        case PET_STATE_ACTIVE: d->active_ms_total += step; break;
        case PET_STATE_LIGHT:  d->light_ms_total  += step; break;
        case PET_STATE_QUIET:  d->quiet_ms_total  += step; break;
        default: break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* 合并                                                               */
/* ------------------------------------------------------------------ */

bool pet_reg_device_alive(const pet_device_t *d, uint32_t now_ms,
                          uint32_t lost_timeout_ms)
{
    if (d == NULL || !d->bound || !d->heard) {
        return false;
    }
    return ((uint32_t)(now_ms - d->last_seen_ms) < lost_timeout_ms);
}

pet_lamp_state_t pet_reg_merge(const pet_registry_t *r, uint32_t now_ms,
                               uint32_t lost_timeout_ms, uint8_t *source_index)
{
    if (source_index != NULL) {
        *source_index = PET_FOCUS_MERGE;
    }
    if (r == NULL || r->count == 0u) {
        return PET_LAMP_LOST;
    }

    /* 决策 O5：焦点锁定时只看该设备。
     * 焦点设备失联时返回 Lost（保持焦点语义一致：不显示其他狗的状态）；
     * 但"是否处于 Lost"的整体判断仍由调用方用 pet_reg_lost_age_ms() +
     * 其他设备是否在册来决定 —— 只要还有别的狗在册，就不算"全部失联"。 */
    if (r->focus_index != PET_FOCUS_MERGE && r->focus_index < r->count) {
        const pet_device_t *f = &r->dev[r->focus_index];
        if (source_index != NULL) {
            *source_index = r->focus_index;
        }
        if (pet_reg_device_alive(f, now_ms, lost_timeout_ms)) {
            return lamp_state_of(f->activity_state);
        }
        return PET_LAMP_LOST;
    }

    /* 决策 D7：最活跃者优先；同状态取 activity_score 更高者 */
    int best = -1;
    uint8_t best_prio = 0u;
    uint8_t best_score = 0u;

    for (uint8_t i = 0; i < r->count; ++i) {
        const pet_device_t *d = &r->dev[i];
        if (!pet_reg_device_alive(d, now_ms, lost_timeout_ms)) {
            continue;
        }
        const uint8_t prio = state_priority(d->activity_state);
        if (best < 0 || prio > best_prio ||
            (prio == best_prio && d->activity_score > best_score)) {
            best = (int)i;
            best_prio = prio;
            best_score = d->activity_score;
        }
    }

    if (best < 0) {
        return PET_LAMP_LOST;   /* 全部超时 */
    }
    if (source_index != NULL) {
        *source_index = (uint8_t)best;
    }
    return lamp_state_of(r->dev[best].activity_state);
}

uint8_t pet_reg_merged_score(const pet_registry_t *r, uint8_t source_index)
{    if (r == NULL) {
        return 0u;
    }
    if (source_index != PET_FOCUS_MERGE && source_index < r->count) {
        return r->dev[source_index].activity_score;
    }
    uint8_t best = 0u;
    for (uint8_t i = 0; i < r->count; ++i) {
        if (r->dev[i].activity_score > best) {
            best = r->dev[i].activity_score;
        }
    }
    return best;
}

bool pet_reg_any_low_battery(const pet_registry_t *r, uint32_t now_ms,
                             uint32_t lost_timeout_ms)
{
    if (r == NULL) {
        return false;
    }
    for (uint8_t i = 0; i < r->count; ++i) {
        const pet_device_t *d = &r->dev[i];
        if (pet_reg_device_alive(d, now_ms, lost_timeout_ms) &&
            (d->flags & PET_FLAG_LOW_BATTERY) != 0u) {
            return true;
        }
    }
    return false;
}

bool pet_reg_any_sensor_error(const pet_registry_t *r, uint32_t now_ms,
                              uint32_t lost_timeout_ms)
{
    if (r == NULL) {
        return false;
    }
    for (uint8_t i = 0; i < r->count; ++i) {
        const pet_device_t *d = &r->dev[i];
        if (pet_reg_device_alive(d, now_ms, lost_timeout_ms) &&
            (d->flags & PET_FLAG_SENSOR_ERROR) != 0u) {
            return true;
        }
    }
    return false;
}

uint64_t pet_reg_lost_age_ms(const pet_registry_t *r, uint32_t now_ms)
{
    if (r == NULL) {
        return UINT64_MAX;
    }
    uint64_t youngest = UINT64_MAX;
    for (uint8_t i = 0; i < r->count; ++i) {
        const pet_device_t *d = &r->dev[i];
        if (!d->bound || !d->heard) {
            continue;
        }
        const uint32_t age = now_ms - d->last_seen_ms;
        if ((uint64_t)age < youngest) {
            youngest = (uint64_t)age;
        }
    }
    return youngest;
}
