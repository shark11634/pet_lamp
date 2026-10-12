/*
 * host_registry_test.c — 多狗设备表测试
 *
 * 覆盖（对应决策 D6/D7/O5 与需求 4.6）：
 *   1. 容量：默认 8、运行期 1–16、越界钳位、表满拒绝
 *   2. 绑定/解绑/清空/焦点锁定
 *   3. 序号去重、回绕、佩戴端重启后重新同步（连续 3 包）
 *   4. **每只狗独立计时**：一只走远不触发 Lost，全部走远才 Lost
 *   5. 最活跃者优先合并；同状态取分数高者
 *   6. 焦点锁定语义（O5）
 *   7. 命名（含 UTF-8 截断边界）与默认名 Pet-N
 *   8. lost_age 取"最年轻的"设备
 */
#include <stdio.h>
#include <string.h>

#include "device_registry.h"
#include "pet_protocol.h"
#include "pet_test.h"

#define T0 1000000u

static pet_packet_t mk_pkt(uint32_t id, uint16_t seq, uint8_t state, uint8_t score)
{
    pet_packet_t p;
    memset(&p, 0, sizeof(p));
    p.device_id = id;
    p.sequence = seq;
    p.activity_state = state;
    p.activity_score = score;
    p.battery_percent = 80u;
    p.flags = PET_FLAG_WORN;
    return p;
}

/* ------------------------------------------------------------------ */

static void test_capacity(void)
{
    pet_registry_t r;

    pet_reg_init(&r, PET_DEV_MAX_DEFAULT);
    t_eq(r.cap, PET_DEV_MAX_DEFAULT, "默认容量 = 8");
    t_eq(pet_reg_count(&r), 0, "初始为空");

    pet_reg_set_cap(&r, 0);
    t_eq(r.cap, 1, "容量 0 被钳到 1");
    pet_reg_set_cap(&r, 99);
    t_eq(r.cap, (long)PET_DEV_MAX_LIMIT, "容量 99 被钳到上限 16");
    pet_reg_set_cap(&r, 3);
    t_eq(r.cap, 3, "容量可设为 3");

    /* 填到容量上限，第 4 个应被拒绝 */
    t_check(pet_reg_add(&r, 0x1001u, T0) >= 0, "加入第 1 只");
    t_check(pet_reg_add(&r, 0x1002u, T0) >= 0, "加入第 2 只");
    t_check(pet_reg_add(&r, 0x1003u, T0) >= 0, "加入第 3 只");
    t_eq(pet_reg_add(&r, 0x1004u, T0), -1, "容量 3 时第 4 只被拒绝");
    t_eq(pet_reg_count(&r), 3, "设备数仍为 3");

    /* 已存在的设备重复 add 应返回原下标而不是失败 */
    t_eq(pet_reg_add(&r, 0x1002u, T0), 1, "重复 add 返回已有下标");

    /* 扩大容量后可继续加入 */
    pet_reg_set_cap(&r, 16);
    t_check(pet_reg_add(&r, 0x1004u, T0) >= 0, "扩容后第 4 只可加入");
    t_eq(pet_reg_count(&r), 4, "设备数 = 4");

    /* 缩小容量不踢已有设备 */
    pet_reg_set_cap(&r, 2);
    t_eq(pet_reg_count(&r), 4, "缩小容量不踢已有设备");
    t_eq(pet_reg_add(&r, 0x1005u, T0), -1, "缩小容量后不能再加新设备");

    /* DeviceID 0 视为无效 */
    pet_reg_set_cap(&r, 16);
    t_eq(pet_reg_add(&r, 0u, T0), -1, "DeviceID 0 被拒绝");
}

static void test_bind_remove_clear(void)
{
    pet_registry_t r;
    pet_reg_init(&r, 8);

    t_check(pet_reg_add(&r, 0xAAAAu, T0) >= 0, "加入 A");
    t_check(pet_reg_add(&r, 0xBBBBu, T0) >= 0, "加入 B");
    t_check(pet_reg_add(&r, 0xCCCCu, T0) >= 0, "加入 C");
    t_check(pet_reg_is_bound(&r, 0xBBBBu), "B 已绑定");
    t_check(!pet_reg_is_bound(&r, 0xDDDDu), "D 未绑定");
    t_eq(pet_reg_find(&r, 0xCCCCu), 2, "C 在下标 2");

    /* 删除中间一个，后续前移 */
    t_check(pet_reg_remove(&r, 0xBBBBu), "移除 B");
    t_eq(pet_reg_count(&r), 2, "移除后设备数 = 2");
    t_eq(pet_reg_find(&r, 0xCCCCu), 1, "C 前移到下标 1");
    t_check(!pet_reg_remove(&r, 0xBBBBu), "重复移除返回 false");

    /* 焦点随之前移 */
    pet_reg_set_focus(&r, 1);            /* 指向 C */
    t_eq(r.focus_index, 1, "焦点设为 C");
    t_check(pet_reg_add(&r, 0xDDDDu, T0) >= 0, "加入 D");
    t_check(pet_reg_remove(&r, 0xAAAAu), "移除 A（下标 0）");
    t_eq(r.focus_index, 0, "焦点随 C 前移到 0");

    pet_reg_clear(&r);
    t_eq(pet_reg_count(&r), 0, "清空后为空");
    t_eq(r.focus_index, (long)PET_FOCUS_MERGE, "清空后焦点回到合并");
    t_eq(r.cap, 8, "清空后容量保留");
}

static void test_sequence_handling(void)
{
    pet_registry_t r;
    pet_reg_init(&r, 8);
    pet_reg_add(&r, 0x1001u, T0);

    pet_packet_t p = mk_pkt(0x1001u, 100u, PET_STATE_QUIET, 10u);
    t_eq(pet_reg_on_packet(&r, &p, T0, false), PET_RX_ACCEPTED, "首包接受");

    p = mk_pkt(0x1001u, 101u, PET_STATE_QUIET, 10u);
    t_eq(pet_reg_on_packet(&r, &p, T0 + 1000u, false), PET_RX_ACCEPTED, "新序号接受");

    /* 旧序号拒绝 */
    p = mk_pkt(0x1001u, 100u, PET_STATE_QUIET, 10u);
    t_eq(pet_reg_on_packet(&r, &p, T0 + 2000u, false), PET_RX_ERR_STALE, "旧序号拒绝");

    /* 相同序号：连续 3 个合法包后才放宽一次（需求 4.6 重启重新同步） */
    p = mk_pkt(0x1001u, 101u, PET_STATE_QUIET, 10u);
    t_eq(pet_reg_on_packet(&r, &p, T0 + 3000u, false), PET_RX_ERR_STALE,
         "重复序号第 1 次拒绝");
    t_eq(pet_reg_on_packet(&r, &p, T0 + 4000u, false), PET_RX_ERR_STALE,
         "重复序号第 2 次拒绝");
    t_eq(pet_reg_on_packet(&r, &p, T0 + 5000u, false), PET_RX_ACCEPTED,
         "重复序号第 3 次放宽接受（重启重新同步）");

    /* 回绕：0xFFFF → 0x0000 视为更新 */
    pet_registry_t r2;
    pet_reg_init(&r2, 4);
    pet_reg_add(&r2, 0x2002u, T0);
    pet_packet_t q = mk_pkt(0x2002u, 0xFFFFu, PET_STATE_QUIET, 5u);
    t_eq(pet_reg_on_packet(&r2, &q, T0, false), PET_RX_ACCEPTED, "seq=0xFFFF 接受");
    q = mk_pkt(0x2002u, 0x0000u, PET_STATE_QUIET, 5u);
    t_eq(pet_reg_on_packet(&r2, &q, T0 + 1000u, false), PET_RX_ACCEPTED,
         "回绕 seq=0x0000 接受");

    t_info("重复/过旧累计 = %u", (unsigned)r.total_dup);
    t_eq(r.total_dup, 3, "重复/过旧计数 = 3");
}

static void test_unbound_rejected(void)
{
    pet_registry_t r;
    pet_reg_init(&r, 8);
    pet_reg_add(&r, 0x1001u, T0);

    /* 正常监听：未绑定的 DeviceID 必须被拒（需求 4.6） */
    pet_packet_t p = mk_pkt(0x9999u, 1u, PET_STATE_ACTIVE, 200u);
    t_eq(pet_reg_on_packet(&r, &p, T0, false), PET_RX_ERR_UNBOUND,
         "未绑定设备被拒（allow_new=false）");
    t_eq(pet_reg_count(&r), 1, "设备表未增长");
    t_info("未绑定丢弃累计 = %u", (unsigned)r.total_unbound);
    t_eq(r.total_unbound, 1, "unbound 计数 = 1");

    /* 配对模式：允许自动加入 */
    t_eq(pet_reg_on_packet(&r, &p, T0 + 1000u, true), PET_RX_ACCEPTED,
         "配对模式接受新设备（allow_new=true）");
    t_eq(pet_reg_count(&r), 2, "设备表增长到 2");
}

static void test_per_device_timeout(void)
{
    pet_registry_t r;
    pet_reg_init(&r, 8);
    pet_reg_add(&r, 0x1001u, T0);   /* 狗 A */
    pet_reg_add(&r, 0x1002u, T0);   /* 狗 B */

    t_section("两狗都在册");
    pet_packet_t a = mk_pkt(0x1001u, 10u, PET_STATE_QUIET, 5u);
    pet_packet_t b = mk_pkt(0x1002u, 20u, PET_STATE_ACTIVE, 220u);
    pet_reg_on_packet(&r, &a, T0, false);
    pet_reg_on_packet(&r, &b, T0, false);

    uint8_t src = 0xFFu;
    pet_lamp_state_t st = pet_reg_merge(&r, T0 + 1000u, LOST_TIMEOUT_MS, &src);
    t_eq(st, PET_LAMP_ACTIVE, "合并结果 = Active（B 更活跃）");
    t_eq(src, 1, "来源为下标 1（狗 B）");

    t_section("A 走远 70 秒（超过 lost_timeout），B 仍在");
    /* 只给 B 更新，A 停更 → A 超时，但灯不应进 Lost */
    b = mk_pkt(0x1002u, 21u, PET_STATE_LIGHT, 120u);
    pet_reg_on_packet(&r, &b, T0 + 70000u, false);
    src = 0xFFu;
    st = pet_reg_merge(&r, T0 + 70000u, LOST_TIMEOUT_MS, &src);
    t_eq(st, PET_LAMP_LIGHT, "仍显示 B 的状态，不因 A 超时而 Lost");
    t_eq(src, 1, "来源仍为 B");
    t_info("A 的 last_seen 龄 = %u ms（超时阈值 %u ms）",
           (unsigned)(T0 + 70000u - r.dev[0].last_seen_ms), (unsigned)LOST_TIMEOUT_MS);
    t_check(!pet_reg_device_alive(&r.dev[0], T0 + 70000u, LOST_TIMEOUT_MS),
            "A 已超时（不在册）");
    t_check(pet_reg_device_alive(&r.dev[1], T0 + 70000u, LOST_TIMEOUT_MS),
            "B 仍在册");

    /* lost_age 应取最年轻的设备（B） */
    const uint64_t age = pet_reg_lost_age_ms(&r, T0 + 71000u);
    t_eq((long)age, 1000, "lost_age 取最年轻设备（B 的 1000 ms）");

    t_section("两狗都走远 -> 全部 Lost");
    st = pet_reg_merge(&r, T0 + 70000u + LOST_TIMEOUT_MS + 1u, LOST_TIMEOUT_MS, &src);
    t_eq(st, PET_LAMP_LOST, "全部超时 -> Lost");
    t_eq(src, (long)PET_FOCUS_MERGE, "全部 Lost 时来源 = 合并标记");

    /* 空表也应是 Lost */
    pet_registry_t empty;
    pet_reg_init(&empty, 8);
    t_eq(pet_reg_merge(&empty, T0, LOST_TIMEOUT_MS, NULL), PET_LAMP_LOST,
         "空设备表 -> Lost");
}

static void test_merge_priority(void)
{
    pet_registry_t r;
    pet_reg_init(&r, 8);
    pet_reg_add(&r, 0x1u, T0);
    pet_reg_add(&r, 0x2u, T0);
    pet_reg_add(&r, 0x3u, T0);

    pet_packet_t p;
    p = mk_pkt(0x1u, 1u, PET_STATE_QUIET, 200u);   /* 分高但状态低 */
    pet_reg_on_packet(&r, &p, T0, false);
    p = mk_pkt(0x2u, 1u, PET_STATE_LIGHT, 50u);
    pet_reg_on_packet(&r, &p, T0, false);
    p = mk_pkt(0x3u, 1u, PET_STATE_QUIET, 250u);
    pet_reg_on_packet(&r, &p, T0, false);

    uint8_t src = 0xFFu;
    t_eq(pet_reg_merge(&r, T0 + 1000u, LOST_TIMEOUT_MS, &src), PET_LAMP_LIGHT,
         "状态优先级高于分数（Light 胜 Quiet）");
    t_eq(src, 1, "来源为下标 1");

    /* 同状态时取分数更高者 */
    p = mk_pkt(0x1u, 2u, PET_STATE_ACTIVE, 100u);
    pet_reg_on_packet(&r, &p, T0 + 2000u, false);
    p = mk_pkt(0x3u, 2u, PET_STATE_ACTIVE, 240u);
    pet_reg_on_packet(&r, &p, T0 + 2000u, false);
    src = 0xFFu;
    t_eq(pet_reg_merge(&r, T0 + 3000u, LOST_TIMEOUT_MS, &src), PET_LAMP_ACTIVE,
         "同为 Active 时取合并状态 Active");
    t_eq(src, 2, "同状态取 activity_score 更高者（狗 3）");
    t_eq(pet_reg_merged_score(&r, src), 240, "合并分数 = 240");
}

static void test_focus_lock(void)
{
    pet_registry_t r;
    pet_reg_init(&r, 8);
    pet_reg_add(&r, 0x1001u, T0);
    pet_reg_add(&r, 0x1002u, T0);

    pet_packet_t a = mk_pkt(0x1001u, 1u, PET_STATE_QUIET, 5u);
    pet_packet_t b = mk_pkt(0x1002u, 1u, PET_STATE_ACTIVE, 220u);
    pet_reg_on_packet(&r, &a, T0, false);
    pet_reg_on_packet(&r, &b, T0, false);

    uint8_t src = 0xFFu;
    t_eq(pet_reg_merge(&r, T0 + 1000u, LOST_TIMEOUT_MS, &src), PET_LAMP_ACTIVE,
         "合并模式显示最活跃（B）");

    /* 锁定只看狗 A（Quiet） */
    t_check(pet_reg_set_focus(&r, 0), "锁定到狗 A");
    src = 0xFFu;
    t_eq(pet_reg_merge(&r, T0 + 1000u, LOST_TIMEOUT_MS, &src), PET_LAMP_QUIET,
         "锁定后只显示 A 的 Quiet");
    t_eq(src, 0, "来源为 A 的下标 0");

    /* 越界焦点被拒 */
    t_check(!pet_reg_set_focus(&r, 5), "越界焦点被拒");
    t_check(pet_reg_set_focus(&r, PET_FOCUS_MERGE), "可切回合并模式");
    t_eq(pet_reg_merge(&r, T0 + 1000u, LOST_TIMEOUT_MS, &src), PET_LAMP_ACTIVE,
         "切回合并模式后显示最活跃");

    /* 决策 O5：锁定的狗失联时返回 Lost（不显示其他狗），
     * 但"整体是否 Lost"由 lost_age + 其他设备在册决定 */
    t_check(pet_reg_set_focus(&r, 0), "重新锁定狗 A");
    src = 0xFFu;
    /* 只给 B 更新，A 超时 */
    b = mk_pkt(0x1002u, 2u, PET_STATE_ACTIVE, 220u);
    pet_reg_on_packet(&r, &b, T0 + 70000u, false);
    t_eq(pet_reg_merge(&r, T0 + 70000u, LOST_TIMEOUT_MS, &src), PET_LAMP_LOST,
         "锁定的狗失联 -> 显示 Lost（不串到别的狗）");
    t_eq(src, 0, "来源仍是锁定的下标 0");
    const uint64_t age = pet_reg_lost_age_ms(&r, T0 + 71000u);
    t_eq((long)age, 1000, "lost_age 仍反映 B 在册（1000 ms），可据此区分'全丢'与'焦点丢'");
}

static void test_names(void)
{
    pet_registry_t r;
    pet_reg_init(&r, 8);
    pet_reg_add(&r, 0x1001u, T0);
    pet_reg_add(&r, 0x1002u, T0);

    char buf[PET_DEV_NAME_LEN + 8];

    /* 未命名时给默认名 Pet-N（决策 O3） */
    pet_reg_display_name(&r, 0, buf, sizeof(buf));
    t_check(strcmp(buf, "Pet-1") == 0, "默认名 Pet-1");
    pet_reg_display_name(&r, 1, buf, sizeof(buf));
    t_check(strcmp(buf, "Pet-2") == 0, "默认名 Pet-2");

    /* 命名 */
    t_check(pet_reg_set_name(&r, 0x1002u, "豆豆"), "设置中文名");
    pet_reg_display_name(&r, 1, buf, sizeof(buf));
    t_check(strcmp(buf, "豆豆") == 0, "中文名往返一致");

    /* 空串恢复未命名 */
    t_check(pet_reg_set_name(&r, 0x1002u, ""), "清空名字");
    pet_reg_display_name(&r, 1, buf, sizeof(buf));
    t_check(strcmp(buf, "Pet-2") == 0, "清空后回到默认名");

    /* 超长名截断，且不在 UTF-8 字符中间截断 */
    t_check(pet_reg_set_name(&r, 0x1002u, "ABCDEFGHIJKLMNOPQRSTUVWXYZ"), "设置超长 ASCII 名");
    pet_reg_display_name(&r, 1, buf, sizeof(buf));
    t_eq((long)strlen(buf), (long)(PET_DEV_NAME_LEN - 1u), "ASCII 名截断到 16 字节");

    t_check(pet_reg_set_name(&r, 0x1002u, "一二三四五六七八九十"), "设置超长中文名");
    const size_t n = strlen(buf);
    pet_reg_display_name(&r, 1, buf, sizeof(buf));
    t_check(strlen(buf) <= PET_DEV_NAME_LEN - 1u, "中文名截断后不超长");
    /* 校验没有落在 UTF-8 续接字节上导致半个字符 */
    int valid_utf8_tail = 1;
    {
        const unsigned char *p = (const unsigned char *)buf;
        size_t i = 0;
        while (i < strlen(buf)) {
            unsigned char c = p[i];
            size_t len = (c < 0x80u) ? 1u : ((c & 0xE0u) == 0xC0u) ? 2u
                       : ((c & 0xF0u) == 0xE0u) ? 3u
                       : ((c & 0xF8u) == 0xF0u) ? 4u : 0u;
            if (len == 0u || i + len > strlen(buf)) {
                valid_utf8_tail = 0;
                break;
            }
            for (size_t k = 1; k < len; ++k) {
                if (((unsigned char)p[i + k] & 0xC0u) != 0x80u) {
                    valid_utf8_tail = 0;
                }
            }
            i += len;
        }
    }
    t_check(valid_utf8_tail, "UTF-8 截断未切断多字节字符");

    /* 未绑定的设备不能命名 */
    t_check(!pet_reg_set_name(&r, 0xDEADu, "x"), "未绑定设备不能命名");
    (void)n;
}

static void test_flags_aggregation(void)
{
    pet_registry_t r;
    pet_reg_init(&r, 8);
    pet_reg_add(&r, 0x1001u, T0);
    pet_reg_add(&r, 0x1002u, T0);

    pet_packet_t a = mk_pkt(0x1001u, 1u, PET_STATE_QUIET, 5u);
    a.flags = PET_FLAG_WORN;                      /* A 电量正常 */
    pet_reg_on_packet(&r, &a, T0, false);
    pet_packet_t b = mk_pkt(0x1002u, 1u, PET_STATE_QUIET, 5u);
    b.flags = PET_FLAG_WORN | PET_FLAG_LOW_BATTERY | PET_FLAG_SENSOR_ERROR;
    pet_reg_on_packet(&r, &b, T0, false);

    t_check(pet_reg_any_low_battery(&r, T0 + 1000u, LOST_TIMEOUT_MS),
            "任一设备低电量 -> 聚合为真");
    t_check(pet_reg_any_sensor_error(&r, T0 + 1000u, LOST_TIMEOUT_MS),
            "任一设备传感器异常 -> 聚合为真");

    /* 超时后不再计入聚合 */
    t_check(!pet_reg_any_low_battery(&r, T0 + LOST_TIMEOUT_MS + 1u, LOST_TIMEOUT_MS),
            "全部超时后不再报低电量");
    t_check(!pet_reg_any_sensor_error(&r, T0 + LOST_TIMEOUT_MS + 1u, LOST_TIMEOUT_MS),
            "全部超时后不再报传感器异常");
}

static void test_stats_accumulate(void)
{
    pet_registry_t r;
    pet_reg_init(&r, 4);
    pet_reg_add(&r, 0x1001u, T0);

    pet_packet_t p = mk_pkt(0x1001u, 1u, PET_STATE_ACTIVE, 200u);
    pet_reg_on_packet(&r, &p, T0, false);

    /* 以 1 Hz 推进 10 次，应累计 10 秒 Active 时长 */
    for (int i = 0; i < 10; ++i) {
        pet_reg_tick(&r, T0 + (uint32_t)(i * TASK_STATE_INTERVAL_MS));
    }
    t_eq(r.dev[0].active_ms_total, 10u * TASK_STATE_INTERVAL_MS,
         "Active 累计时长 = 10 × 1 s");
    t_eq(r.dev[0].light_ms_total, 0, "Light 时长仍为 0");
    t_eq(r.dev[0].packets_total, 1, "包计数 = 1");

    /* 切到 Quiet 再推进 5 次 */
    p = mk_pkt(0x1001u, 2u, PET_STATE_QUIET, 3u);
    pet_reg_on_packet(&r, &p, T0 + 10000u, false);
    for (int i = 0; i < 5; ++i) {
        pet_reg_tick(&r, T0 + 10000u + (uint32_t)(i * TASK_STATE_INTERVAL_MS));
    }
    t_eq(r.dev[0].quiet_ms_total, 5u * TASK_STATE_INTERVAL_MS, "Quiet 累计 = 5 s");
    t_eq(r.total_valid, 2, "有效包总数 = 2");
}

/* ------------------------------------------------------------------ */

int main(void)
{
    static const pet_test_case_t cases[] = {
        { "容量与钳位",       test_capacity },
        { "绑定/解绑/清空",   test_bind_remove_clear },
        { "序号去重与回绕",   test_sequence_handling },
        { "未绑定拒绝",       test_unbound_rejected },
        { "每狗独立超时",     test_per_device_timeout },
        { "最活跃者优先合并", test_merge_priority },
        { "焦点锁定语义",     test_focus_lock },
        { "命名与默认名",     test_names },
        { "Flags 聚合",       test_flags_aggregation },
        { "统计累计",         test_stats_accumulate },
    };

    printf("=====================================================================\n");
    printf("宠物活动状态灯 V1 — 多狗设备表主机测试\n");
    printf("=====================================================================\n");

    const int fails = t_run("device_registry", cases, sizeof(cases) / sizeof(cases[0]));

    printf("\n=====================================================================\n");
    printf("%s（失败 %d 项）\n", fails == 0 ? "设备表测试全部通过" : "设备表测试存在失败", fails);
    printf("=====================================================================\n");
    return (fails == 0) ? 0 : 1;
}
