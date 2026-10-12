/*
 * panel_protocol.h — bslamp2 触控面板 I²C 协议（平台无关）
 *
 * ⚠️ 出处与许可：协议**不是**本工程逆向出来的，而是移植自已验证的社区实现
 *
 *     esphome-xiaomi_bslamp2  (mmakaay)
 *     https://github.com/mmakaay/esphome-xiaomi_bslamp2
 *     文件：components/xiaomi_bslamp2/front_panel_hal.h
 *
 *   本文件是它的 C 语言等价物（纯函数，可在主机上单元测试）。
 *   按需求 §5.6 的合规要求保留出处；若再次分发请一并保留本声明与
 *   上游仓库的 LICENSE。
 *
 * ── 协议要点（均为 7 字节定长消息） ─────────────────────────────────
 *
 * 读事件：先 WRITE `READY_FOR_EV`（7 字节），再 READ 7 字节事件。
 * 事件字节布局：
 *   m[0..3] = 0x04 0x04 0x01 0x00     固定前缀
 *   m[4]    = 部位：0x01 电源键 / 0x02 色彩键 / 0x03 滑条触摸 / 0x04 滑条松开
 *   m[5]    = 事件类型码或滑条原始值
 *   m[6]    = 按键时为 `类型码 + m[4]` 的校验字节；滑条时为 `m[5]+m[4]+1` 的校验
 *
 * 按键：m[4]=0x01|0x02，m[5]=0x01 且 m[6]=0x02+m[4] → 触摸
 *                      m[5]=0x02 且 m[6]=0x03+m[4] → 松开
 * 滑条：m[4]=0x03 → 触摸，0x04 → 松开；校验 m[6] == m[5]+m[4]+1；
 *       原始值 m[5] 必须落在 0x01..0x16（1..22）
 *
 * ── 事件码位域（与上游一致，便于逐位对照） ──────────────────────────
 *   bit0     OK          解析成功
 *   bit1-3   部位        与上面 m[4] 同值
 *   bit4-5   类型        0x1 触摸 / 0x2 松开
 *   bit6-10  滑条档位    0..22（已换算）
 *
 * 写 LED：WRITE `{0x02, 0x03, hi, lo, 0x64, 0x00, 0x00}`，其中
 *   state 15 bit：bit14 电源灯 / bit12 色彩灯 / bit9..0 滑条 10 颗灯
 *   （LED_1 是靠电源键的那颗，LED_10 是靠色彩键的那颗）
 */
#ifndef PANEL_PROTOCOL_H
#define PANEL_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 面板 I²C 消息长度（读写都是 7 字节） */
#define PANEL_MSG_LEN 7u

/* ---- 事件码位域 ---- */
#define PANEL_EV_OK          (1u << 0)

#define PANEL_EV_PART_SHIFT  1u
#define PANEL_EV_PART_MASK   (0x7u << PANEL_EV_PART_SHIFT)
#define PANEL_EV_PART_UNKNOWN 0x0u
#define PANEL_EV_PART_POWER   0x1u
#define PANEL_EV_PART_COLOR   0x2u
#define PANEL_EV_PART_SLIDER  0x4u

#define PANEL_EV_TYPE_SHIFT  4u
#define PANEL_EV_TYPE_MASK   (0x3u << PANEL_EV_TYPE_SHIFT)
#define PANEL_EV_TYPE_UNKNOWN 0x0u
#define PANEL_EV_TYPE_TOUCH   0x1u
#define PANEL_EV_TYPE_RELEASE 0x2u

#define PANEL_EV_LEVEL_SHIFT 6u
#define PANEL_EV_LEVEL_MASK  (0x1Fu << PANEL_EV_LEVEL_SHIFT)

/** 滑条最大档位（协议原始值 1..22 → 档位 = 23 - 原始值） */
#define PANEL_SLIDER_MAX_LEVEL 22u
#define PANEL_SLIDER_MIN_RAW   1u
#define PANEL_SLIDER_MAX_RAW   0x16u
#define PANEL_SLIDER_LEVEL_BASE 0x17u

/* ---- 面板 LED 位（与上游 enum FrontPanelLEDs 一一对应） ---- */
#define PANEL_LED_NONE        0x0000u
#define PANEL_LED_POWER       0x4000u   /* bit14 */
#define PANEL_LED_COLOR       0x1000u   /* bit12 */
#define PANEL_LED_1           0x0200u   /* 靠电源键的滑条灯 */
#define PANEL_LED_2           0x0100u
#define PANEL_LED_3           0x0080u
#define PANEL_LED_4           0x0040u
#define PANEL_LED_5           0x0020u
#define PANEL_LED_6           0x0010u
#define PANEL_LED_7           0x0008u
#define PANEL_LED_8           0x0004u
#define PANEL_LED_9           0x0002u
#define PANEL_LED_10          0x0001u   /* 靠色彩键的滑条灯 */
#define PANEL_LED_ALL_SLIDER  0x03FFu
#define PANEL_LED_ALL         (PANEL_LED_POWER | PANEL_LED_COLOR | PANEL_LED_ALL_SLIDER)

/**
 * 上游 `turn_on_leds()` / `turn_off_leds()` 会一并设置的固定位。
 *
 * 上游原值：`0b0000110000000000` = **0x0C00**
 * （曾把它误读为 0x1800 而把电源灯位写错 —— 见 host_panel_test.c 的回归断言）
 *
 * 语义：这两个 bit 是"协议固定要带上的位"。由于 turn_on/turn_off 都是
 * **按位或**，真实写出的状态恒为 `0x0C00 | 请求的 LED 位`，即使请求
 * LED_NONE 也一样。
 */
#define PANEL_LED_TX_BITS     0x0C00u

/* ------------------------------------------------------------------ */
/* 解析                                                                */
/* ------------------------------------------------------------------ */

/** 解析结果：既返回合成事件码，也让调用方用下面的取值函数读取字段 */
typedef struct {
    uint16_t event;      /**< 完整事件码；未通过校验时为 0（即无 PANEL_EV_OK） */
    bool     ok;         /**< 与 PANEL_EV_OK 等价，便于直接判断 */
    uint8_t  part;       /**< PANEL_EV_PART_* */
    uint8_t  type;       /**< PANEL_EV_TYPE_* */
    uint8_t  level;      /**< 滑条档位 0..22；非滑条事件为 0 */
    uint8_t  error;      /**< 0 = 通过；否则为 PANEL_PARSE_ERR_* */
} panel_event_t;

typedef enum {
    PANEL_PARSE_OK = 0,
    PANEL_PARSE_ERR_PREFIX,      /**< 固定前缀不匹配 */
    PANEL_PARSE_ERR_PART,        /**< 部位码非法 */
    PANEL_PARSE_ERR_BUTTON_TYPE, /**< 按键的类型/校验字节不匹配 */
    PANEL_PARSE_ERR_SLIDER_CRC,  /**< 滑条校验字节不匹配 */
    PANEL_PARSE_ERR_SLIDER_RANGE,/**< 滑条原始值越界 */
} panel_parse_err_t;

/**
 * 解析一条 7 字节事件消息。
 *
 * 纯函数，不碰硬件 —— 因此可以用合成的字节序列做完整单元测试。
 *
 * @param msg  恰好 PANEL_MSG_LEN 字节
 * @param out  解析结果（即使失败也会写入 error 与已识别到的字段）
 */
void panel_parse_event(const uint8_t msg[PANEL_MSG_LEN], panel_event_t *out);

/** 错误码可读名（日志用） */
const char *panel_parse_err_name(uint8_t err);

/** 部位可读名（日志用） */
const char *panel_part_name(uint8_t part);

/** 是否一次"触摸"（按下）事件，而非松开 */
static inline bool panel_event_is_touch(const panel_event_t *ev)
{
    return ev->ok && ((ev->event & PANEL_EV_TYPE_MASK) >> PANEL_EV_TYPE_SHIFT) ==
                         PANEL_EV_TYPE_TOUCH;
}

/** 是否滑条事件 */
static inline bool panel_event_is_slider(const panel_event_t *ev)
{
    return ev->ok && ((ev->event & PANEL_EV_PART_MASK) >> PANEL_EV_PART_SHIFT) ==
                         PANEL_EV_PART_SLIDER;
}

/** 类型码是否为"触摸"（按下） */
static inline bool panel_is_touch_type(uint8_t type)
{
    return type == PANEL_EV_TYPE_TOUCH;
}

/** 事件码里的部位字段 */
static inline uint8_t panel_event_part(uint16_t event)
{
    return (uint8_t)((event & PANEL_EV_PART_MASK) >> PANEL_EV_PART_SHIFT);
}

/* ------------------------------------------------------------------ */
/* 编码（写面板）                                                      */
/* ------------------------------------------------------------------ */

/** 读事件请求：先写这 7 字节，再读 7 字节事件 */
extern const uint8_t PANEL_READY_FOR_EV[PANEL_MSG_LEN];

/**
 * 编码 LED 状态消息。
 *
 * @param leds  PANEL_LED_* 的按位或
 * @param out   输出缓冲，至少 PANEL_MSG_LEN 字节
 */
void panel_encode_leds(uint16_t leds, uint8_t out[PANEL_MSG_LEN]);

/**
 * 亮度比例 → 滑条 LED 位（复刻上游 `set_slider_level` 的阈值）。
 *
 * @param ratio 0.0–1.0；0 表示全灭
 * @return PANEL_LED_* 位组合（只含滑条那 10 颗）
 */
uint16_t panel_slider_leds(float ratio);

#ifdef __cplusplus
}
#endif

#endif /* PANEL_PROTOCOL_H */
