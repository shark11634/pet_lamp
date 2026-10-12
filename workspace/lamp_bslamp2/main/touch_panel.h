/*
 * touch_panel.h — 触摸面板事件抽象层（bslamp2 面板 I²C）
 *
 * 硬件（需求 §4.2）：
 *   面板 I²C SDA = GPIO21，SCL = GPIO19，中断 = GPIO16（面板 → ESP32）
 *   → 这是与面板 MCU 之间的 I²C 通讯，**不是** ESP32 内置电容触摸通道。
 *
 * 协议：见 panel_protocol.h。**已接入**——协议移植自已验证的社区实现
 *   esphome-xiaomi_bslamp2 (mmakaay) 的 front_panel_hal.h，
 *   从机地址 0x2C 与该仓库 packages/core.yaml 一致。
 *   纯函数部分由 test/host_panel_test.c 做完整单元测试；
 *   真机上只需确认 0x2C 在线。
 *
 * 手动控制优先级（§7.3）：
 *   1. 长按电源键 ≥ 5 s：进入配对 / 维护模式
 *   2. 短按电源键：临时熄灯或恢复自动显示
 *   3. 滑条：只影响全局亮度上限，不改变活动状态颜色规则
 *   4. 自动状态：除非用户手动关闭，否则持续更新
 *
 * 多狗扩展（决策 D15）：色彩键在**单狗**时保持原厂色相循环行为；
 * 在**多狗**时改为"合并 / 只看某只"的循环切换。
 */
#ifndef TOUCH_PANEL_H
#define TOUCH_PANEL_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TOUCH_EVENT_NONE = 0,
    TOUCH_EVENT_POWER_SHORT,      /**< 短按：临时熄灯 / 恢复自动显示 */
    TOUCH_EVENT_POWER_LONG,       /**< 长按 ≥ PAIR_HOLD_MS：配对 / 维护模式 */
    TOUCH_EVENT_COLOR_SHORT,      /**< 色彩键短按 */
    TOUCH_EVENT_COLOR_LONG,       /**< 色彩键长按（多狗时用于色相切换） */
    TOUCH_EVENT_BRIGHTNESS,       /**< 滑条：全局亮度上限（value = 0–100） */
} touch_panel_event_type_t;

typedef struct {
    touch_panel_event_type_t type;
    uint8_t  value;               /**< TOUCH_EVENT_BRIGHTNESS 时有效：0–100 */
    uint32_t time_ms;
} touch_panel_event_t;

/** 面板事件回调；在面板任务上下文调用，实现要短（不要在里面写 Flash） */
typedef void (*touch_panel_cb_t)(const touch_panel_event_t *ev);

/**
 * 初始化面板：配置 I²C 总线（GPIO21/19）、中断脚（GPIO16），
 * 探测面板是否在 0x2C 应答，然后就绪后启动事件任务。
 *
 * 面板不在线时返回 ESP_ERR_NOT_FOUND 且**不启动事件任务**：
 * 核心链路（BLE → 设备表 → 夜间模式 → 灯效）不受影响。
 *
 * @param cb 事件回调；NULL 表示只入队，由 touch_panel_poll() 取
 */
esp_err_t touch_panel_init(touch_panel_cb_t cb);

/**
 * 非阻塞读取一个面板事件（供不使用回调的调用方）。
 * @return true 取到事件；false 当前无事件
 */
bool touch_panel_poll(touch_panel_event_t *out);

/** 协议已启用**且**面板在线 */
bool touch_panel_protocol_confirmed(void);

/** 扫描 I²C 总线并打印所有应答的从机地址（定位面板地址用，§5.6） */
esp_err_t touch_panel_scan_bus(void);

/* ------------------------------------------------------------------ */
/* 面板指示灯（12 颗：电源灯 + 色彩灯 + 10 颗滑条灯）                   */
/* ------------------------------------------------------------------ */

/**
 * 设置面板指示灯。
 *
 * @param led_bits PANEL_LED_* 的按位或（见 panel_protocol.h）
 *
 * 内部只在状态变化时写 I²C。用于显示"当前焦点是第几只狗"等状态。
 */
esp_err_t touch_panel_set_leds(uint16_t led_bits);

/** 亮度比例 0.0–1.0 → 滑条 10 颗灯位（复刻原厂行为） */
uint16_t touch_panel_slider_leds_for_ratio(float ratio);

/** 面板中断累计次数（诊断用；可判断中断线是否接好） */
uint32_t touch_panel_irq_count(void);

#ifdef __cplusplus
}
#endif

#endif /* TOUCH_PANEL_H */
