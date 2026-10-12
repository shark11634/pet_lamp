/*
 * host_panel_test.c — bslamp2 触控面板协议单元测试
 *
 * 被测对象是 main/panel_protocol.c 的**纯函数**（无任何硬件访问），
 * 因此可以用合成字节序列做完整覆盖率测试。
 *
 * 协议出处：esphome-xiaomi_bslamp2 (mmakaay) 的 front_panel_hal.h，
 * 移植为 C 见 main/panel_protocol.h 的说明。
 *
 * 测试策略：
 *   1. 固定前缀与部位码的负例（每一种错误都要被拒）
 *   2. 按键触摸/松开的**逐位**事件码比对（照抄上游位域定义手工算出期望值，
 *      而不是调用同一个函数算 —— 否则是自我验证）
 *   3. 滑条全部 22 个档位逐个验证换算正确
 *   4. 滑条校验字节与范围的负例
 *   5. LED 消息编码与固定位
 *   6. 滑条 LED 阈值（0.0 / 边界值 / 1.0）
 */
#include <stdio.h>
#include <string.h>

#include "pet_test.h"

#include "panel_protocol.h"
#include "app_config.h"

/* ------------------------------------------------------------------ */
/* 手工构造期望事件码（照上游位域定义，独立于被测实现）                 */
/* ------------------------------------------------------------------ */

#define EXPECT(ok, part, type, level)                            \
    ((uint16_t)(((ok) ? 1u : 0u) |                               \
                (((uint16_t)(part) & 0x7u) << 1) |               \
                (((uint16_t)(type) & 0x3u) << 4) |               \
                (((uint16_t)(level) & 0x1Fu) << 6)))

/* ------------------------------------------------------------------ */
/* 辅助：构造一条合法的按键消息                                        */
/* ------------------------------------------------------------------ */

static void make_button(uint8_t msg[PANEL_MSG_LEN], uint8_t part, uint8_t type)
{
    memset(msg, 0, PANEL_MSG_LEN);
    msg[0] = 0x04;
    msg[1] = 0x04;
    msg[2] = 0x01;
    msg[3] = 0x00;
    msg[4] = part;
    msg[5] = type;                       /* 0x01 touch / 0x02 release */
    msg[6] = (uint8_t)(type + 0x01u + part);
}

static void make_slider(uint8_t msg[PANEL_MSG_LEN], uint8_t part_raw, uint8_t raw)
{
    memset(msg, 0, PANEL_MSG_LEN);
    msg[0] = 0x04;
    msg[1] = 0x04;
    msg[2] = 0x01;
    msg[3] = 0x00;
    msg[4] = part_raw;                   /* 0x03 touch / 0x04 release */
    msg[5] = raw;                        /* 1..22 */
    msg[6] = (uint8_t)(0x01u + raw + part_raw);
}

/* ================================================================== */
/* 用例                                                                */
/* ================================================================== */

/* 1. 固定前缀 */
static void test_prefix(void)
{
    uint8_t msg[PANEL_MSG_LEN];
    panel_event_t ev;

    make_button(msg, 0x01, 0x01);
    panel_parse_event(msg, &ev);
    t_check(ev.ok, "合法电源键触摸被接受");
    t_eq(ev.error, PANEL_PARSE_OK, "无错误码");

    /* 逐个字节破坏前缀，必须全部被拒 */
    for (int i = 0; i < 4; ++i) {
        make_button(msg, 0x01, 0x01);
        msg[i] ^= 0xFFu;
        panel_parse_event(msg, &ev);
        char buf[64];
        snprintf(buf, sizeof(buf), "前缀第 %d 字节被破坏时拒收", i);
        t_check(!ev.ok, buf);
        t_eq(ev.error, PANEL_PARSE_ERR_PREFIX, "错误码 = 前缀不匹配");
    }
}

/* 2. 部位码 */
static void test_part_codes(void)
{
    uint8_t msg[PANEL_MSG_LEN];
    panel_event_t ev;

    /* 0x00 / 0x05 都不是合法部位 */
    const uint8_t bad_parts[] = { 0x00u, 0x05u, 0x06u, 0xFFu };
    for (size_t i = 0; i < sizeof(bad_parts); ++i) {
        make_button(msg, bad_parts[i], 0x01);
        panel_parse_event(msg, &ev);
        char buf[64];
        snprintf(buf, sizeof(buf), "部位码 0x%02X 被拒", bad_parts[i]);
        t_check(!ev.ok, buf);
        t_eq(ev.error, PANEL_PARSE_ERR_PART, "错误码 = 部位码非法");
    }

    /* 四个合法部位 */
    struct { uint8_t raw; uint8_t part; const char *name; } ok[] = {
        { 0x01u, PANEL_EV_PART_POWER,  "电源键" },
        { 0x02u, PANEL_EV_PART_COLOR,  "色彩键" },
    };
    for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); ++i) {
        make_button(msg, ok[i].raw, 0x01);
        panel_parse_event(msg, &ev);
        char buf[64];
        snprintf(buf, sizeof(buf), "%s 被正确识别", ok[i].name);
        t_check(ev.ok, buf);
        t_eq(ev.part, ok[i].part, buf);
    }
}

/* 3. 按键：触摸与松开，逐位比对事件码 */
static void test_buttons(void)
{
    uint8_t msg[PANEL_MSG_LEN];
    panel_event_t ev;

    struct { uint8_t raw; uint8_t part; const char *name; } parts[] = {
        { 0x01u, PANEL_EV_PART_POWER, "电源键" },
        { 0x02u, PANEL_EV_PART_COLOR, "色彩键" },
    };

    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); ++i) {
        for (int touch = 0; touch < 2; ++touch) {
            const uint8_t type = touch ? 0x01u : 0x02u;
            make_button(msg, parts[i].raw, type);
            panel_parse_event(msg, &ev);

            char buf[96];
            snprintf(buf, sizeof(buf), "%s %s 事件码逐位正确",
                     parts[i].name, touch ? "触摸" : "松开");
            t_eq(ev.event,
                 EXPECT(1, parts[i].part,
                        touch ? PANEL_EV_TYPE_TOUCH : PANEL_EV_TYPE_RELEASE, 0),
                 buf);
            t_eq(ev.type,
                 touch ? PANEL_EV_TYPE_TOUCH : PANEL_EV_TYPE_RELEASE, buf);
            t_check(panel_event_is_touch(&ev) == (touch != 0), "触摸判定一致");
            t_check(!panel_event_is_slider(&ev), "按键不是滑条事件");
        }
    }

    /* 校验字节错误必须被拒 */
    make_button(msg, 0x01, 0x01);
    msg[6] = 0x00u;                       /* 正确值应为 0x02+0x01 = 0x03 */
    panel_parse_event(msg, &ev);
    t_check(!ev.ok, "按键校验字节错误被拒");
    t_eq(ev.error, PANEL_PARSE_ERR_BUTTON_TYPE, "错误码 = 按键类型不匹配");

    /* 类型码错误 */
    make_button(msg, 0x01, 0x03);         /* 0x03 不是合法类型码 */
    panel_parse_event(msg, &ev);
    t_check(!ev.ok, "非法按键类型码被拒");

    /* 把电源键的校验字节套到色彩键上（交叉污染） */
    make_button(msg, 0x02, 0x01);
    msg[6] = (uint8_t)(0x02u + 0x01u);    /* 应按 0x02+0x02 = 0x04 */
    panel_parse_event(msg, &ev);
    t_check(!ev.ok, "色彩键用了电源键的校验字节 -> 被拒");
}

/* 4. 滑条：全部 22 档逐个验证 */
static void test_slider_levels(void)
{
    uint8_t msg[PANEL_MSG_LEN];
    panel_event_t ev;
    int all_ok = 1;
    uint8_t first_bad_raw = 0u;
    uint8_t first_bad_level = 0u;
    uint8_t first_bad_expect = 0u;

    for (uint8_t raw = PANEL_SLIDER_MIN_RAW; raw <= PANEL_SLIDER_MAX_RAW; ++raw) {
        make_slider(msg, 0x03u, raw);
        panel_parse_event(msg, &ev);
        const uint8_t expect = (uint8_t)(PANEL_SLIDER_LEVEL_BASE - raw);
        if (!ev.ok || ev.level != expect) {
            all_ok = 0;
            first_bad_raw = raw;
            first_bad_level = ev.level;
            first_bad_expect = expect;
            break;
        }
    }
    if (!all_ok) {
        t_info("原始值 %u 期望档位 %u 实际 %u", first_bad_raw,
               first_bad_expect, first_bad_level);
    }
    t_check(all_ok, "滑条 1..22 全部换算正确（档位 = 23 - 原始值）");

    /* 档位单调性：原始值越大 → 档位越小 */
    uint8_t prev = 0xFFu;
    int mono = 1;
    for (uint8_t raw = PANEL_SLIDER_MIN_RAW; raw <= PANEL_SLIDER_MAX_RAW; ++raw) {
        make_slider(msg, 0x03u, raw);
        panel_parse_event(msg, &ev);
        if (ev.level >= prev) {
            mono = 0;
            break;
        }
        prev = ev.level;
    }
    t_check(mono, "滑条档位随原始值单调递减");

    /* 触摸与松开 */
    for (int touch = 0; touch < 2; ++touch) {
        const uint8_t part_raw = touch ? 0x03u : 0x04u;
        make_slider(msg, part_raw, 0x0Cu);
        panel_parse_event(msg, &ev);
        t_check(ev.ok, touch ? "滑条触摸被接受" : "滑条松开被接受");
        t_eq(ev.part, PANEL_EV_PART_SLIDER, "部位 = 滑条");
        t_check(panel_event_is_slider(&ev), "滑条判定一致");
        t_eq(ev.event,
             EXPECT(1, PANEL_EV_PART_SLIDER,
                    touch ? PANEL_EV_TYPE_TOUCH : PANEL_EV_TYPE_RELEASE,
                    (uint8_t)(PANEL_SLIDER_LEVEL_BASE - 0x0Cu)),
             "滑条事件码逐位正确");
    }
}

/* 5. 滑条负例：校验字节与范围 */
static void test_slider_errors(void)
{
    uint8_t msg[PANEL_MSG_LEN];
    panel_event_t ev;

    /* 校验字节错 */
    make_slider(msg, 0x03u, 0x10u);
    msg[6] ^= 0x01u;
    panel_parse_event(msg, &ev);
    t_check(!ev.ok, "滑条校验字节错误被拒");
    t_eq(ev.error, PANEL_PARSE_ERR_SLIDER_CRC, "错误码 = 滑条校验不匹配");

    /* 原始值越界：0 与 23 */
    const uint8_t bad[] = { 0x00u, 0x17u, 0xFFu };
    for (size_t i = 0; i < sizeof(bad); ++i) {
        make_slider(msg, 0x03u, bad[i]);
        panel_parse_event(msg, &ev);
        char buf[64];
        snprintf(buf, sizeof(buf), "滑条原始值 0x%02X 越界被拒", bad[i]);
        t_check(!ev.ok, buf);
        /* 注意：0x00 会先撞校验（0x01+0+3 = 0x04，我们填的正是 0x04，所以
         * 校验能过），因此这里应当报范围错；0x17/0xFF 也应为范围错 */
        t_eq(ev.error, PANEL_PARSE_ERR_SLIDER_RANGE, "错误码 = 滑条越界");
    }
}

/* 6. LED 消息编码 */
static void test_led_encoding(void)
{
    uint8_t msg[PANEL_MSG_LEN];

    panel_encode_leds(PANEL_LED_NONE, msg);
    t_eq(msg[0], 0x02, "LED 消息 byte0 = 0x02");
    t_eq(msg[1], 0x03, "LED 消息 byte1 = 0x03");
    t_eq(msg[4], 0x64, "LED 消息 byte4 = 0x64");
    t_eq(msg[5], 0x00, "LED 消息 byte5 = 0x00");
    t_eq(msg[6], 0x00, "LED 消息 byte6 = 0x00");

    /*
     * 回归：固定位必须是 0x0C00（上游 `0b0000110000000000`）。
     * 本工程在移植时曾把它误读为 0x1800，导致**电源灯位写错**。
     * 这条断言就是为了钉死它 —— 别改成你觉得"更整齐"的数。
     */
    t_eq(PANEL_LED_TX_BITS, 0x0C00u, "固定位 = 0x0C00（上游 0b0000110000000000）");
    t_eq(((uint16_t)msg[2] << 8) | msg[3], 0x0C00u,
         "LED_NONE 仍写出固定位 0x0C00");

    /* 电源灯：与固定位按位或（固定位本来就含电源灯位，属于幂等） */
    panel_encode_leds(PANEL_LED_POWER, msg);
    t_eq(((uint16_t)msg[2] << 8) | msg[3],
         (uint16_t)(PANEL_LED_TX_BITS | PANEL_LED_POWER),
         "电源灯位（bit14）正确");
    t_check((((uint16_t)msg[2] << 8) | msg[3]) & PANEL_LED_POWER,
            "电源灯确实被置位");

    /* 色彩灯 */
    panel_encode_leds(PANEL_LED_COLOR, msg);
    t_eq(((uint16_t)msg[2] << 8) | msg[3],
         (uint16_t)(PANEL_LED_TX_BITS | PANEL_LED_COLOR),
         "色彩灯位（bit12）正确");
    t_check((((uint16_t)msg[2] << 8) | msg[3]) & PANEL_LED_COLOR,
            "色彩灯确实被置位");

    /* 10 颗滑条灯全亮 */
    panel_encode_leds(PANEL_LED_ALL_SLIDER, msg);
    t_eq(((uint16_t)msg[2] << 8) | msg[3],
         (uint16_t)(PANEL_LED_TX_BITS | PANEL_LED_ALL_SLIDER),
         "10 颗滑条灯全亮");
    t_eq(PANEL_LED_ALL_SLIDER, 0x03FFu, "LED_ALL_SLIDER = 0x03FF");
    t_eq(PANEL_LED_ALL,
         (uint16_t)(PANEL_LED_POWER | PANEL_LED_COLOR | PANEL_LED_ALL_SLIDER),
         "LED_ALL = 电源 + 色彩 + 10 颗滑条");
    t_eq(PANEL_LED_ALL, 21503u, "LED_ALL = 0x53FF（上游 16384+4096+1023）");

    /* 与上游 enum 的十进制值逐项核对 */
    t_eq(PANEL_LED_POWER, 16384u, "LED_POWER 与上游数值一致");
    t_eq(PANEL_LED_COLOR, 4096u, "LED_COLOR 与上游数值一致");
    t_eq(PANEL_LED_1, 512u, "LED_1 与上游数值一致");
    t_eq(PANEL_LED_10, 1u, "LED_10 与上游数值一致");
    t_eq(PANEL_LED_NONE, 0u, "LED_NONE = 0");

    /* LED_1 在电源键一侧、LED_10 在色彩键一侧（上游注释） */
    t_check(PANEL_LED_1 > PANEL_LED_10, "LED_1 的位高于 LED_10");
    t_eq(PANEL_LED_1, 0x0200u, "LED_1 = bit9");
    t_eq(PANEL_LED_10, 0x0001u, "LED_10 = bit0");

    /* bit13 必须为空：固定位是 0x0C00，不是 0x1800 */
    t_check((PANEL_LED_TX_BITS & 0x2000u) == 0u,
            "固定位不含 bit13（防止把 0x0C00 写成 0x1800）");
}

/* 7. 滑条 LED 阈值（复刻上游 set_slider_level） */
static void test_slider_led_thresholds(void)
{
    t_eq(panel_slider_leds(0.0f), 0, "比例 0 -> 滑条灯全灭");
    t_eq(panel_slider_leds(-0.5f), 0, "负比例 -> 全灭");

    /* 刚好越过第一档 */
    t_eq(panel_slider_leds(0.001f), PANEL_LED_1, "刚过 0 -> 只有 LED_1");
    t_eq(panel_slider_leds(0.15f), PANEL_LED_1, "0.15 -> 仍只有 LED_1（严格大于才亮）");
    t_eq(panel_slider_leds(0.16f), (uint16_t)(PANEL_LED_1 | PANEL_LED_2),
         "0.16 -> LED_1 + LED_2");

    /* 满档 */
    const uint16_t full = panel_slider_leds(1.0f);
    t_eq(full, PANEL_LED_ALL_SLIDER, "1.0 -> 10 颗全亮");

    /* 单调不减 */
    int mono = 1;
    uint16_t prev = 0u;
    for (int i = 0; i <= 100; ++i) {
        const uint16_t cur = panel_slider_leds((float)i / 100.0f);
        if (cur < prev) {
            mono = 0;
            break;
        }
        prev = cur;
    }
    t_check(mono, "滑条灯数量随比例单调不减");

    /* 亮的灯数应当落在 1..10 */
    int count_ok = 1;
    for (int i = 0; i <= 100; ++i) {
        const uint16_t leds = panel_slider_leds((float)i / 100.0f);
        int n = 0;
        for (int b = 0; b < 16; ++b) {
            if (leds & (1u << b)) {
                ++n;
            }
        }
        if (n > 10) {
            count_ok = 0;
            break;
        }
    }
    t_check(count_ok, "最多只会点亮 10 颗滑条灯");
}

/* 8. 读事件请求常量 */
static void test_ready_for_event(void)
{
    t_eq(PANEL_READY_FOR_EV[0], 0x01, "READY_FOR_EV byte0 = 0x01");
    t_eq(PANEL_READY_FOR_EV[1], 0x00, "READY_FOR_EV byte1 = 0x00");
    t_eq(PANEL_READY_FOR_EV[5], 0x00, "READY_FOR_EV byte5 = 0x00");
    t_eq(PANEL_READY_FOR_EV[6], 0x01, "READY_FOR_EV byte6 = 0x01");
    t_eq(PANEL_MSG_LEN, 7, "消息长度 = 7 字节");
}

/* ================================================================== */

int main(void)
{
    static const pet_test_case_t cases[] = {
        { "固定前缀与拒收",       test_prefix },
        { "部位码校验",           test_part_codes },
        { "按键事件（触摸/松开）", test_buttons },
        { "滑条 22 档换算",       test_slider_levels },
        { "滑条负例",             test_slider_errors },
        { "LED 消息编码",         test_led_encoding },
        { "滑条 LED 阈值",        test_slider_led_thresholds },
        { "读事件请求常量",       test_ready_for_event },
    };

    printf("=====================================================================\n");
    printf("宠物活动状态灯 V1 — 触控面板协议单元测试\n");
    printf("协议出处：esphome-xiaomi_bslamp2 / front_panel_hal.h\n");
    printf("被测：main/panel_protocol.c（纯函数，无硬件依赖）\n");
    printf("=====================================================================\n");

    const int fails = t_run("panel", cases, sizeof(cases) / sizeof(cases[0]));

    printf("\n=====================================================================\n");
    printf("%s（失败 %d 项）\n",
           fails == 0 ? "面板协议测试全部通过" : "面板协议测试存在失败", fails);
    printf("=====================================================================\n");
    return (fails == 0) ? 0 : 1;
}
