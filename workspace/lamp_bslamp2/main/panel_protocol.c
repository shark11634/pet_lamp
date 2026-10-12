/*
 * panel_protocol.c — bslamp2 触控面板 I²C 协议的纯函数实现
 *
 * 出处见 panel_protocol.h（移植自 esphome-xiaomi_bslamp2/front_panel_hal.h）。
 *
 * 本文件刻意**不包含任何硬件访问**：
 *   · 解析是纯函数 → 可以用合成字节做穷举式单元测试
 *   · I²C 读写留在 touch_panel.c
 * 这样协议本身的正确性不依赖真机，而真机只验证"接线与电气"。
 */
#include "panel_protocol.h"

#include <string.h>

/* 读事件请求（上游 MSG READY_FOR_EV） */
const uint8_t PANEL_READY_FOR_EV[PANEL_MSG_LEN] = {
    0x01u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u,
};

/* 写 LED 消息的固定头与常量部分（上游 led_msg_ 初值） */
#define PANEL_LED_MSG_0  0x02u
#define PANEL_LED_MSG_1  0x03u
#define PANEL_LED_MSG_4  0x64u

/** 事件固定前缀 */
#define PANEL_EV_PREFIX_0 0x04u
#define PANEL_EV_PREFIX_1 0x04u
#define PANEL_EV_PREFIX_2 0x01u
#define PANEL_EV_PREFIX_3 0x00u

/* 部位码（协议 m[4]） */
#define PANEL_PART_POWER  0x01u
#define PANEL_PART_COLOR  0x02u
#define PANEL_PART_SLIDER_TOUCH   0x03u
#define PANEL_PART_SLIDER_RELEASE 0x04u

/* 按键事件类型码（协议 m[5]） */
#define PANEL_BTN_TYPE_TOUCH   0x01u
#define PANEL_BTN_TYPE_RELEASE 0x02u

/* ------------------------------------------------------------------ */
/* 可读名                                                              */
/* ------------------------------------------------------------------ */

const char *panel_parse_err_name(uint8_t err)
{
    switch (err) {
        case PANEL_PARSE_OK:                 return "ok";
        case PANEL_PARSE_ERR_PREFIX:         return "前缀不是 04:04:01:00";
        case PANEL_PARSE_ERR_PART:           return "部位码非法";
        case PANEL_PARSE_ERR_BUTTON_TYPE:    return "按键类型/校验字节不匹配";
        case PANEL_PARSE_ERR_SLIDER_CRC:     return "滑条校验字节不匹配";
        case PANEL_PARSE_ERR_SLIDER_RANGE:   return "滑条原始值越界";
        default:                             return "未知";
    }
}

const char *panel_part_name(uint8_t part)
{
    switch (part) {
        case PANEL_EV_PART_POWER:  return "电源键";
        case PANEL_EV_PART_COLOR:  return "色彩键";
        case PANEL_EV_PART_SLIDER: return "滑条";
        default:                   return "未知";
    }
}

/* ------------------------------------------------------------------ */
/* 解析                                                                */
/* ------------------------------------------------------------------ */

void panel_parse_event(const uint8_t msg[PANEL_MSG_LEN], panel_event_t *out)
{
    if (msg == NULL || out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->error = PANEL_PARSE_ERR_PREFIX;

    /* 1) 固定前缀 */
    if (msg[0] != PANEL_EV_PREFIX_0 || msg[1] != PANEL_EV_PREFIX_1 ||
        msg[2] != PANEL_EV_PREFIX_2 || msg[3] != PANEL_EV_PREFIX_3) {
        return;
    }

    /* 2) 部位分派 */
    switch (msg[4]) {
        case PANEL_PART_POWER:
        case PANEL_PART_COLOR: {
            out->part = (msg[4] == PANEL_PART_POWER) ? PANEL_EV_PART_POWER
                                                     : PANEL_EV_PART_COLOR;
            out->event |= (uint16_t)out->part << PANEL_EV_PART_SHIFT;

            if (msg[5] == PANEL_BTN_TYPE_TOUCH &&
                msg[6] == (uint8_t)(0x02u + msg[4])) {
                out->type = PANEL_EV_TYPE_TOUCH;
            } else if (msg[5] == PANEL_BTN_TYPE_RELEASE &&
                       msg[6] == (uint8_t)(0x03u + msg[4])) {
                out->type = PANEL_EV_TYPE_RELEASE;
            } else {
                out->error = PANEL_PARSE_ERR_BUTTON_TYPE;
                return;
            }
            out->event |= (uint16_t)out->type << PANEL_EV_TYPE_SHIFT;
            break;
        }

        case PANEL_PART_SLIDER_TOUCH:
        case PANEL_PART_SLIDER_RELEASE: {
            out->part = PANEL_EV_PART_SLIDER;
            out->type = (msg[4] == PANEL_PART_SLIDER_TOUCH)
                            ? PANEL_EV_TYPE_TOUCH
                            : PANEL_EV_TYPE_RELEASE;
            out->event |= (uint16_t)out->part << PANEL_EV_PART_SHIFT;
            out->event |= (uint16_t)out->type << PANEL_EV_TYPE_SHIFT;

            /* 校验：m[6] == m[5] + m[4] + 1
             * 用无符号减法，避免溢出到负数域（与上游的 int 运算等价） */
            const uint8_t expect =
                (uint8_t)(0x01u + msg[5] + msg[4]);
            if (msg[6] != expect) {
                out->error = PANEL_PARSE_ERR_SLIDER_CRC;
                return;
            }
            if (msg[5] < PANEL_SLIDER_MIN_RAW || msg[5] > PANEL_SLIDER_MAX_RAW) {
                out->error = PANEL_PARSE_ERR_SLIDER_RANGE;
                return;
            }
            /* 档位换算：原始值 1..22 → 档位 22..1（值越大越靠电源键） */
            out->level = (uint8_t)(PANEL_SLIDER_LEVEL_BASE - msg[5]);
            out->event |= (uint16_t)out->level << PANEL_EV_LEVEL_SHIFT;
            break;
        }

        default:
            out->error = PANEL_PARSE_ERR_PART;
            return;
    }

    /* 3) 全部规则通过 */
    out->ok = true;
    out->error = PANEL_PARSE_OK;
    out->event |= PANEL_EV_OK;
}

/* ------------------------------------------------------------------ */
/* 编码                                                                */
/* ------------------------------------------------------------------ */

void panel_encode_leds(uint16_t leds, uint8_t out[PANEL_MSG_LEN])
{
    if (out == NULL) {
        return;
    }
    /* 与上游 `turn_on_leds` 一致：写之前把固定位并进去 */
    const uint16_t state = (uint16_t)(leds | PANEL_LED_TX_BITS);

    out[0] = PANEL_LED_MSG_0;
    out[1] = PANEL_LED_MSG_1;
    out[2] = (uint8_t)(state >> 8);
    out[3] = (uint8_t)(state & 0xFFu);
    out[4] = PANEL_LED_MSG_4;
    out[5] = 0x00u;
    out[6] = 0x00u;
}

uint16_t panel_slider_leds(float ratio)
{
    /* 复刻上游 set_slider_level 的 10 档阈值 */
    uint16_t leds = PANEL_LED_NONE;
    if (ratio <= 0.0f) {
        return leds;
    }
    if (ratio > 0.00f) leds |= PANEL_LED_1;
    if (ratio > 0.15f) leds |= PANEL_LED_2;
    if (ratio > 0.25f) leds |= PANEL_LED_3;
    if (ratio > 0.35f) leds |= PANEL_LED_4;
    if (ratio > 0.45f) leds |= PANEL_LED_5;
    if (ratio > 0.55f) leds |= PANEL_LED_6;
    if (ratio > 0.65f) leds |= PANEL_LED_7;
    if (ratio > 0.75f) leds |= PANEL_LED_8;
    if (ratio > 0.85f) leds |= PANEL_LED_9;
    if (ratio > 0.95f) leds |= PANEL_LED_10;
    return leds;
}
