/*
 * host_night_test.c — 夜间模式与手动豁免测试
 *
 * 覆盖（决策 D8/D9/D10/D14/O6 + 需求 4.4「不在夜间把 Active 设为高亮」、
 * 需求 12.4「先保证低亮舒适，这是氛围灯不是报警器」）：
 *   1. 默认配置来自 pet_config.h 且被正确钳位
 *   2. 时间窗判定（含跨午夜、边界、时钟不可信）
 *   3. 无时钟时的空闲推断兜底
 *   4. 手动豁免：用户操作触发、重复操作延长、超时回落、可手动退出
 *   5. 状态变化只重置空闲推断，**不**触发手动豁免（否则夜间会一直亮）
 *   6. 夜间亮度上限按状态取值；日间/手动模式不压制
 *   7. 夜间抑制低电量提示
 *   8. 夜间 Active 响应节流（每分钟最多一次），且日间不节流
 *   9. 模式名（Off / Day / Night / Manual）
 *
 * 时间模拟：所有 API 都以 now_ms 入参推进，因此"10 分钟豁免超时"可以在
 * 微秒内测完，不需要真的等待。
 */
#include <stdio.h>
#include <string.h>

#include "night_mode.h"
#include "pet_test.h"

#define T0 500000u
#define MIN_MS 60000u

static pet_night_t g_n;

static void init_default(void)
{
    memset(&g_n, 0, sizeof(g_n));
    pet_night_init(&g_n, NULL, T0);
}

/* ------------------------------------------------------------------ */

static void test_defaults(void)
{
    pet_night_config_t cfg;
    pet_night_config_defaults(&cfg);

    t_check(cfg.enabled, "默认启用夜间模式");
    t_eq(cfg.start_hour, NIGHT_START_HOUR, "默认起始小时 = 22");
    t_eq(cfg.end_hour, NIGHT_END_HOUR, "默认结束小时 = 7");
    t_eq((long)cfg.override_ms, (long)MANUAL_OVERRIDE_MS_DEFAULT,
         "默认手动豁免 = 10 分钟");
    t_eq((long)MANUAL_OVERRIDE_MS_DEFAULT, 10L * MIN_MS, "10 分钟 = 600000 ms");
    t_eq(cfg.cap_active, NIGHT_CAP_ACTIVE, "默认夜间 Active 上限 = 18（决策 D14）");
    t_eq(cfg.cap_quiet, NIGHT_CAP_QUIET, "默认夜间 Quiet 上限 = 10");
    t_eq(cfg.cap_light, NIGHT_CAP_LIGHT, "默认夜间 Light 上限 = 14");
    t_eq(cfg.cap_lost, NIGHT_CAP_LOST, "默认夜间 Lost 上限 = 6");
    t_check(cfg.suppress_lowbat_hint, "默认夜间抑制低电量提示");
    t_eq((long)cfg.active_throttle_ms, (long)NIGHT_ACTIVE_THROTTLE_MS,
         "默认 Active 节流 = 60 秒");

    /* 越界配置必须被钳位 */
    pet_night_config_t bad;
    pet_night_config_defaults(&bad);
    bad.override_ms = 10u;                 /* 远小于最小值 */
    bad.active_throttle_ms = 1u;
    bad.start_hour = 200u;
    bad.end_hour = 250u;
    pet_night_init(&g_n, &bad, T0);
    t_eq((long)g_n.cfg.override_ms, (long)MANUAL_OVERRIDE_MS_MIN,
         "过小的豁免时长被钳到 1 分钟");
    t_eq((long)g_n.cfg.active_throttle_ms, (long)NIGHT_ACTIVE_THROTTLE_MIN,
         "过小的节流被钳到下限");
    t_eq(g_n.cfg.start_hour, 23, "越界起始小时被钳到 23");
    t_eq(g_n.cfg.end_hour, 23, "越界结束小时被钳到 23");

    pet_night_config_defaults(&bad);
    bad.override_ms = 999u * MIN_MS;
    bad.active_throttle_ms = 9999u * MIN_MS;
    pet_night_init(&g_n, &bad, T0);
    t_eq((long)g_n.cfg.override_ms, (long)MANUAL_OVERRIDE_MS_MAX,
         "过大的豁免时长被钳到 120 分钟");
    t_eq((long)g_n.cfg.active_throttle_ms, (long)NIGHT_ACTIVE_THROTTLE_MAX,
         "过大的节流被钳到上限");
}

static void test_time_window(void)
{
    init_default();

    /* 时钟可信时按小时判定：22:00–07:00 */
    struct { int hour; int want_night; } cases[] = {
        { 21, 0 }, { 22, 1 }, { 23, 1 }, { 0, 1 }, { 3, 1 },
        { 6, 1 },  { 7, 0 },  { 12, 0 }, { 18, 0 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        /* 用同一个"刚刚操作过"的时间点，避免空闲推断干扰 */
        pet_night_t n;
        pet_night_init(&n, NULL, T0);
        pet_night_tick(&n, T0, cases[i].hour, true);
        const int got = pet_night_is_night(&n) ? 1 : 0;
        char buf[64];
        snprintf(buf, sizeof(buf), "%02d:00 -> %s", cases[i].hour,
                 cases[i].want_night ? "夜间" : "日间");
        t_eq(got, cases[i].want_night, buf);
    }

    /* 时钟不可信时时间窗不生效，但空闲推断仍可兜底 */
    {
        pet_night_t n;
        pet_night_init(&n, NULL, T0);
        pet_night_tick(&n, T0 + 60u * 1000u, 3, false);
        t_check(!pet_night_is_night(&n),
                "时钟不可信且未长时间空闲 -> 不算夜间");
    }

    /* 关闭夜间模式后即使时间窗内也不压制 */
    {
        pet_night_config_t cfg;
        pet_night_config_defaults(&cfg);
        cfg.enabled = false;
        pet_night_t n;
        pet_night_init(&n, &cfg, T0);
        pet_night_tick(&n, T0, 23, true);
        t_check(!pet_night_is_night(&n), "夜间模式关闭 -> 不压制");
        t_eq(pet_night_cap_for(&n, PET_LAMP_ACTIVE), 255,
             "关闭后 Active 上限 = 255（不压制）");
    }
}

static void test_idle_inference(void)
{
    init_default();

    /* 时钟不可信，靠空闲推断：超过 NIGHT_IDLE_INFER_MS 无操作 + 无状态变化 */
    pet_night_tick(&g_n, T0, 12, false);
    t_check(!pet_night_is_night(&g_n), "刚启动不算夜间");

    pet_night_tick(&g_n, T0 + NIGHT_IDLE_INFER_MS - 1000u, 12, false);
    t_check(!pet_night_is_night(&g_n), "未到空闲阈值不算夜间");

    pet_night_tick(&g_n, T0 + NIGHT_IDLE_INFER_MS, 12, false);
    t_info("空闲推断阈值 = %u 分钟",
           (unsigned)(NIGHT_IDLE_INFER_MS / MIN_MS));
    t_check(pet_night_is_night(&g_n), "达到空闲阈值 -> 推断为夜间");
    t_eq(pet_night_cap_for(&g_n, PET_LAMP_ACTIVE), NIGHT_CAP_ACTIVE,
         "空闲推断生效后同样应用夜间上限");

    /* 状态变化重置空闲计时（不进入手动模式） */
    pet_night_on_state_change(&g_n, T0 + NIGHT_IDLE_INFER_MS);
    t_check(!g_n.manual_mode, "状态变化不进入手动模式（关键：否则夜间会一直亮）");
    pet_night_tick(&g_n, T0 + NIGHT_IDLE_INFER_MS + 1000u, 12, false);
    t_check(!pet_night_is_night(&g_n), "状态变化后重新计时空闲推断");
}

static void test_manual_override(void)
{
    init_default();
    pet_night_tick(&g_n, T0, 23, true);
    t_check(pet_night_is_night(&g_n), "夜里 23:00 处于夜间");

    /* 用户操作 -> 进入手动模式，夜间压制暂停 */
    pet_night_on_user_action(&g_n, T0 + 1000u);
    t_check(g_n.manual_mode, "用户操作进入手动模式");
    t_check(!pet_night_is_night(&g_n), "手动模式期间不算夜间（压制不生效）");
    t_eq(pet_night_cap_for(&g_n, PET_LAMP_ACTIVE), 255,
         "手动模式不压制亮度");
    t_eq(pet_night_override_remaining_ms(&g_n, T0 + 1000u),
         (long)MANUAL_OVERRIDE_MS_DEFAULT, "剩余豁免 = 10 分钟");

    /* 重复操作延长豁免 */
    pet_night_tick(&g_n, T0 + 5u * MIN_MS, 23, true);
    t_check(g_n.manual_mode, "5 分钟后仍在手动模式");
    pet_night_on_user_action(&g_n, T0 + 5u * MIN_MS);
    pet_night_tick(&g_n, T0 + 14u * MIN_MS, 23, true);
    t_check(g_n.manual_mode, "再次操作后豁免被延长");

    /* 超时回落 */
    pet_night_tick(&g_n, T0 + 5u * MIN_MS + MANUAL_OVERRIDE_MS_DEFAULT, 23, true);
    t_check(!g_n.manual_mode, "无操作满 10 分钟 -> 退出手动模式");
    t_check(pet_night_is_night(&g_n), "回落后台灯恢复夜间压制");
    t_eq(pet_night_cap_for(&g_n, PET_LAMP_ACTIVE), NIGHT_CAP_ACTIVE,
         "回落后重新应用夜间上限");
    t_eq(pet_night_override_remaining_ms(&g_n, T0 + 30u * MIN_MS), 0,
         "退出后剩余豁免 = 0");

    /* 白天超时回落无副作用 */
    {
        pet_night_t n;
        pet_night_init(&n, NULL, T0);
        pet_night_on_user_action(&n, T0);
        pet_night_tick(&n, T0 + 1000u, 14, true);
        t_check(n.manual_mode, "白天操作也进入手动模式");
        t_check(!pet_night_is_night(&n), "白天手动模式不压制（本来就不压制）");
        pet_night_tick(&n, T0 + MANUAL_OVERRIDE_MS_DEFAULT + 1000u, 14, true);
        t_check(!n.manual_mode, "白天同样按时回落");
    }

    /* 主动退出 */
    pet_night_on_user_action(&g_n, T0 + 40u * MIN_MS);
    t_check(g_n.manual_mode, "再次进入手动模式");
    pet_night_exit_manual(&g_n, T0 + 40u * MIN_MS);
    t_check(!g_n.manual_mode, "可主动退出手动模式");
    t_eq(pet_night_override_remaining_ms(&g_n, T0 + 40u * MIN_MS), 0,
         "主动退出后剩余 = 0");

    /* 豁免次数统计：只在"从非手动 -> 手动"的跃迁时计数，
     * 已处于手动模式时重复操作只延长豁免、不重复计数。
     * 本段共 2 次真实跃迁：T0+1s 进入、T0+40min 重新进入。 */
    t_info("累计进入手动模式 %u 次（重复操作只延长、不重复计数）",
           (unsigned)g_n.override_count);
    t_eq(g_n.override_count, 2, "豁免进入次数 = 2（分界操作算延长）");
}

static void test_caps(void)
{
    init_default();
    pet_night_tick(&g_n, T0, 23, true);
    t_check(pet_night_is_night(&g_n), "23:00 夜间生效");

    t_eq(pet_night_cap_for(&g_n, PET_LAMP_QUIET), NIGHT_CAP_QUIET, "Quiet 上限");
    t_eq(pet_night_cap_for(&g_n, PET_LAMP_LIGHT), NIGHT_CAP_LIGHT, "Light 上限");
    t_eq(pet_night_cap_for(&g_n, PET_LAMP_ACTIVE), NIGHT_CAP_ACTIVE, "Active 上限");
    t_eq(pet_night_cap_for(&g_n, PET_LAMP_LOST), NIGHT_CAP_LOST, "Lost 上限");
    t_eq(pet_night_cap_for(&g_n, PET_LAMP_BOOT), NIGHT_CAP_QUIET, "Boot 用 Quiet 上限");

    /* 上限必须显著低于原厂属性上限，否则"夜间不打扰"无从谈起 */
    t_check(NIGHT_CAP_ACTIVE < 40u, "夜间 Active 上限远低于 40% 逻辑亮度");
    t_check(NIGHT_CAP_ACTIVE > 0u, "夜间 Active 仍可见（不是完全熄灯）");
    t_check(NIGHT_CAP_LOST < NIGHT_CAP_ACTIVE, "Lost 比 Active 更暗");

    /* 日间不压制 */
    pet_night_tick(&g_n, T0 + 1000u, 14, true);
    t_eq(pet_night_cap_for(&g_n, PET_LAMP_ACTIVE), 255, "日间 Active 不压制");
    t_eq(pet_night_cap_for(&g_n, PET_LAMP_LOST), 255, "日间 Lost 不压制");
}

static void test_lowbat_suppression(void)
{
    init_default();
    pet_night_tick(&g_n, T0, 23, true);
    t_check(pet_night_suppress_lowbat(&g_n), "夜间抑制低电量提示");

    pet_night_tick(&g_n, T0 + 1000u, 14, true);
    t_check(!pet_night_suppress_lowbat(&g_n), "日间不抑制");

    /* 手动模式期间也不抑制（用户正在主动操作，提示不打扰） */
    pet_night_tick(&g_n, T0 + 2000u, 23, true);
    pet_night_on_user_action(&g_n, T0 + 3000u);
    t_check(!pet_night_suppress_lowbat(&g_n), "手动模式不抑制提示");

    /* 配置关闭抑制时夜间也允许提示 */
    pet_night_config_t cfg;
    pet_night_config_defaults(&cfg);
    cfg.suppress_lowbat_hint = false;
    pet_night_t init;
    pet_night_init(&init, &cfg, T0);
    pet_night_tick(&init, T0, 23, true);
    t_check(!pet_night_suppress_lowbat(&init), "关闭抑制配置后夜间允许提示");
}

static void test_active_throttle(void)
{
    init_default();
    pet_night_tick(&g_n, T0, 23, true);

    /* 夜间：第一次放行，紧接着被节流 */
    t_check(pet_night_allow_active_render(&g_n, T0), "夜间首次 Active 放行");
    t_check(!pet_night_allow_active_render(&g_n, T0 + 1000u),
            "1 秒后仍被节流");
    t_check(!pet_night_allow_active_render(&g_n, T0 + 30u * 1000u),
            "30 秒后仍被节流");
    t_check(pet_night_allow_active_render(&g_n, T0 + NIGHT_ACTIVE_THROTTLE_MS),
            "满 60 秒后放行");
    t_info("节流累计 %u 次", (unsigned)g_n.throttled_count);
    t_check(g_n.throttled_count == 2u, "节流计数 = 2");

    /* 日间不节流 */
    pet_night_tick(&g_n, T0 + NIGHT_ACTIVE_THROTTLE_MS + 1000u, 14, true);
    t_check(pet_night_allow_active_render(&g_n,
            T0 + NIGHT_ACTIVE_THROTTLE_MS + 1000u), "日间首次放行");
    t_check(pet_night_allow_active_render(&g_n,
            T0 + NIGHT_ACTIVE_THROTTLE_MS + 1500u), "日间连续调用也放行");

    /* 晚上 23 点 且狗每隔几秒动一次 -> 每分钟最多亮一次 */
    {
        pet_night_t n;
        pet_night_init(&n, NULL, T0);
        int renders = 0;
        for (uint32_t t = 0; t < 10u * MIN_MS; t += 3000u) {
            pet_night_tick(&n, T0 + t, 23, true);
            if (pet_night_allow_active_render(&n, T0 + t)) {
                ++renders;
            }
        }
        t_info("10 分钟内 Active 渲染 %d 次（每 3 秒尝试一次）", renders);
        t_check(renders >= 10 && renders <= 11,
                "10 分钟约 10–11 次（即每分钟一次，未因频繁状态变化而反复点亮）");
    }
}

static void test_mode_name(void)
{
    init_default();
    t_eq(0, 0, "占位");

    t_check(strcmp(pet_night_mode_name(&g_n, T0), "Day") == 0, "初始为 Day");

    pet_night_tick(&g_n, T0, 23, true);
    t_check(strcmp(pet_night_mode_name(&g_n, T0), "Night") == 0, "23:00 -> Night");

    pet_night_on_user_action(&g_n, T0 + 1000u);
    t_check(strcmp(pet_night_mode_name(&g_n, T0 + 1000u), "Manual") == 0,
            "操作后 -> Manual");

    pet_night_config_t cfg;
    pet_night_config_defaults(&cfg);
    cfg.enabled = false;
    pet_night_t off;
    pet_night_init(&off, &cfg, T0);
    t_check(strcmp(pet_night_mode_name(&off, T0), "Off") == 0, "关闭 -> Off");
}

/* ------------------------------------------------------------------ */

int main(void)
{
    static const pet_test_case_t cases[] = {
        { "默认配置与钳位",       test_defaults },
        { "时间窗判定",           test_time_window },
        { "空闲推断与状态变化",   test_idle_inference },
        { "手动豁免生命周期",     test_manual_override },
        { "夜间亮度上限",         test_caps },
        { "夜间抑制低电量提示",   test_lowbat_suppression },
        { "夜间 Active 节流",     test_active_throttle },
        { "模式名",               test_mode_name },
    };

    printf("=====================================================================\n");
    printf("宠物活动状态灯 V1 — 夜间模式 / 手动豁免主机测试\n");
    printf("=====================================================================\n");

    const int fails = t_run("night_mode", cases, sizeof(cases) / sizeof(cases[0]));

    printf("\n=====================================================================\n");
    printf("%s（失败 %d 项）\n", fails == 0 ? "夜间模式测试全部通过" : "夜间模式测试存在失败", fails);
    printf("=====================================================================\n");
    return (fails == 0) ? 0 : 1;
}
