/*
 * app_config.h — 灯端（米家床头灯 2 / bslamp2 / ESP32-WROOM-32D）**平台相关**配置
 *
 * 这里只放 GPIO、PWM 频率、任务栈、NVS 命名空间这类"跟板子绑死"的东西。
 *
 * 所有**行为参数**（亮度区间、过渡时间、失联分级、夜间上限、手动豁免、
 * Gamma、限流、多设备容量……）都在 core/pet_config.h，那里是唯一事实来源，
 * 并且有 300 项主机断言覆盖。改行为请改 core/ 并跑
 * test/run_all_host_tests.ps1。
 */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <stdint.h>

/* 行为参数统一从 core 引入 */
#include "pet_config.h"

/* ================================================================== */
/* 1. 硬件引脚（需求 4.2 表格，务必与实物核对后再烧录）                 */
/* ================================================================== */

/** RGB 通道 PWM */
#define PIN_LED_RED        13
#define PIN_LED_GREEN      14
#define PIN_LED_BLUE        5
/** 白光通道 PWM */
#define PIN_LED_WHITE      12

/** LED 主开关 1 / 2：灯光电路控制，两路都要在初始化时拉高 */
#define PIN_LED_SWITCH_1   33
#define PIN_LED_SWITCH_2    4

/** 触摸面板 I²C（面板通讯；注意这是 ESP32 与面板 MCU 之间的 I²C） */
#define PIN_PANEL_I2C_SDA  21
#define PIN_PANEL_I2C_SCL  19
/** 触摸面板中断输入 */
#define PIN_PANEL_INT      16

/** 调试 / 烧录 UART */
#define PIN_UART_TX         1
#define PIN_UART_RX         3
/** Boot 下载模式：上电时拉低进入下载模式 */
#define PIN_BOOT_BUTTON     0

/* ================================================================== */
/* 2. LEDC（占空比尺度与 core 必须一致）                                */
/* ================================================================== */

/**
 * LEDC 通道分配。
 *
 * ⚠️ 刻意与上游（esphome-xiaomi_bslamp2 / packages/core.yaml）**逐项一致**：
 *     红 → 通道 0，绿 → 通道 1，蓝 → 通道 2，白 → **通道 4**
 *   上游把白色放在通道 4 而不是 3，是因为它有独立的定时器（频率不同）。
 *   功能等价的定义包含"同样的颜色输出"，所以这里不图整齐改成 0–3。
 */
#define LAMP_LEDC_SPEED_MODE   LEDC_LOW_SPEED_MODE
#define LAMP_LEDC_CH_RED       0
#define LAMP_LEDC_CH_GREEN     1
#define LAMP_LEDC_CH_BLUE      2
#define LAMP_LEDC_CH_WHITE     4

/**
 * LEDC 定时器：RGB 一个、白光一个（两者频率不同，必须分开）。
 *
 * 频率与上游一致：
 *   RGB  = 3000 Hz
 *   白光 = 10000 Hz
 *
 * 本工程原本用单定时器 20 kHz，理由是"20 kHz 人耳不可闻"。
 * 但 RGB 灯板是恒流驱动，3 kHz 不会啸叫（上游量产验证过），
 * 而频率会影响开关损耗与有效导通时间 → **影响实际亮度与色温**。
 * 既然目标是"与原厂用户可见功能等价"，就采用原厂参数。
 *
 * ── ⚠️ 分辨率与频率是耦合的，不能各自随便填 ────────────────────────
 *
 * ESP32 LEDC（APB 80 MHz 时钟）：
 *      f = 80 MHz / (2^bits × div)，  整数分频 div ∈ [1, 1024]
 *
 * 代入 div = 1 得到**某个位数下可达的最高频率**：
 *
 *   | 分辨率 | 最高频率 | 3 kHz | 10 kHz | 20 kHz |
 *   |--------|----------|-------|--------|--------|
 *   | 10 bit | 78.1 kHz |  ✔    |  ✔     |  ✔     |
 *   | 12 bit | 19.5 kHz |  ✔    |  ✔     |  ✘     |
 *   | 13 bit | 9.77 kHz |  ✔    |  ~     |  ✘     |
 *   | 14 bit | 4.88 kHz |  ✔    |  ✘     |  ✘     |
 *
 * 也就是说：
 *   · **3 kHz @ 14 bit** 完全可行（div = 1.63 → 2，实际约 2.44 kHz）
 *   · **10 kHz @ 14 bit 不可行**（需要 div < 1）。
 *     ESP-IDF 的 LEDC 驱动支持小数分频（LEDC_USE_APB_CLK + 分数分频器），
 *     所以实际固件里能工作；万一某个 IDF 版本把白色通道下调到 13 bit，
 *     也只是白色通道的量化粒度略有损失，不影响功能与观感。
 *   · **20 kHz @ 14 bit 不可能** —— 这就是之前把分辨率定在 14 bit 之后
 *     必须放弃 20 kHz 的原因（1 LSB 会变成 12–25% 亮度跳变）。
 *
 * 下面有编译期检查会在"不可能"的组合上报错，避免改了数字却没人发现。
 */
#define LAMP_LEDC_TIMER_RGB    LEDC_TIMER_0
#define LAMP_LEDC_TIMER_WHITE  LEDC_TIMER_1

/* 用 #ifndef 守卫：允许用 -D 按板子覆盖，也方便测试直接注入不同组合
 * 来验证下面的编译期检查确实会拦住不可达的配置。 */
#ifndef LAMP_LEDC_FREQ_RGB
#define LAMP_LEDC_FREQ_RGB     3000u
#endif
#ifndef LAMP_LEDC_FREQ_WHITE
/*
 * 9765 Hz 而不是上游的 10000 Hz：这是 13 bit 在 ESP32 LEDC 上的**上限**
 * （80 MHz / 2^13 / 1 = 9765.6 Hz）。差 2.35%，人眼看色温无法分辨。
 * 想精确 10000 Hz 就得降到 12 bit（上限 19531 Hz），
 * 但那样白色通道的量化会粗一档，夜间 Quiet 呼吸会受影响 —— 不划算。
 */
#define LAMP_LEDC_FREQ_WHITE   9765u
#endif

/**
 * 各通道的 LEDC 分辨率位数。
 *
 * ⚠️ 这两者**必须不同**，原因见上面的分辨率×频率表：
 *
 *   RGB  3 kHz  @ 14 bit → 需要 div = 80e6/(2^14 × 3000) = 1.63
 *                          整数分频只能取 2 → 实际约 2441 Hz。
 *                          这一路用 14 bit，是为了低亮呼吸有足够量化档位。
 *
 *   白光 10 kHz @ 14 bit → 需要 div = 0.49 **不可达**
 *                          （ESP-IDF 在 div < 1 时会直接报
 *                           "Can not find a proper pre-divider number"）。
 *                          要 ~10 kHz 就只能 13 bit（上限 9766 Hz，
 *                          与原厂 10 kHz 差 2.3%，肉眼看色温无法分辨）。
 *
 * 白光的 14→13 bit 折算在 lamp_esp32.c 里用一次右移完成：
 * core 输出的占空比与亮度**单调对应**，所以只是量化更粗一档。
 */
#define LAMP_LEDC_BITS_RGB     LAMP_PWM_RES_BITS   /* 14 */
#define LAMP_LEDC_BITS_WHITE   13u

/* ------------------------------------------------------------------ */
/* 编译期检查：LEDC 分辨率 × 频率必须物理可达                          */
/* ------------------------------------------------------------------ */
/*
 * ESP32 LEDC：f = 80 MHz / (2^bits × div)，整数分频 div ∈ [1, 1024]。
 * 代入 div = 1 得到某个位数下的**最高可达频率**：
 *     15 bit → 2441 Hz    14 bit → 4882 Hz    13 bit → 9765 Hz
 *     12 bit → 19531 Hz   11 bit → 39062 Hz   10 bit → 78125 Hz
 *
 * 下面按"最贴合的一个位数"取值（即 p 取 ≥ 需求位数的最小可用档），
 * 再要求频率不超过该档的上限。这样既能拦住 20 kHz @ 14 bit 这种
 * 物理上不可能的配置，也不会因为取错档而误报。
 *
 * 注：真实固件里 ESP-IDF 自己的 `ledc_timer_config` 也会在不可达时报错，
 * 但那是**运行时**错误；这里的编译期检查让问题在构建时就暴露。
 */
#define LAMP_LEDC_FMAX_FOR_BITS(bits)                                          \
    (((bits) >= 15u) ? 2441u : ((bits) >= 14u) ? 4882u                          \
     : ((bits) >= 13u) ? 9765u : ((bits) >= 12u) ? 19531u                       \
     : ((bits) >= 11u) ? 39062u : 78125u)

#define LAMP_LEDC_ASSERT_REACHABLE(bits, freq)                                 \
    ((void)sizeof(char[(((uint32_t)(freq)) <= LAMP_LEDC_FMAX_FOR_BITS(bits))   \
                           ? 1 : -1]))

#if !defined(__cplusplus)
/* C99 下用 typedef 做静态断言（负数数组长度会编译失败） */
typedef char lamp_ledc_rgb_ok[
    (LAMP_LEDC_FREQ_RGB <= LAMP_LEDC_FMAX_FOR_BITS(LAMP_LEDC_BITS_RGB)) ? 1 : -1];
typedef char lamp_ledc_white_ok[
    (LAMP_LEDC_FREQ_WHITE <= LAMP_LEDC_FMAX_FOR_BITS(LAMP_LEDC_BITS_WHITE)) ? 1 : -1];
#else
static_assert(LAMP_LEDC_FREQ_RGB <= LAMP_LEDC_FMAX_FOR_BITS(LAMP_LEDC_BITS_RGB),
              "LEDC RGB 分辨率×频率不可达，见 app_config.h 的表");
static_assert(LAMP_LEDC_FREQ_WHITE <= LAMP_LEDC_FMAX_FOR_BITS(LAMP_LEDC_BITS_WHITE),
              "LEDC 白光分辨率×频率不可达，见 app_config.h 的表");
#endif

/* 人类可读的失败提示（静态断言只给行号，这两条给原因） */
#if (LAMP_LEDC_FREQ_RGB > LAMP_LEDC_FMAX_FOR_BITS(LAMP_LEDC_BITS_RGB))
#error "LEDC：RGB 载波频率过高 —— 14 bit 上限 4882 Hz，见 app_config.h 的分辨率×频率表"
#endif
#if (LAMP_LEDC_FREQ_WHITE > LAMP_LEDC_FMAX_FOR_BITS(LAMP_LEDC_BITS_WHITE))
#error "LEDC：白光载波频率过高 —— 13 bit 上限 9765 Hz，见 app_config.h 的分辨率×频率表"
#endif

/* ================================================================== */
/* 3. BLE 扫描队列                                                     */
/* ================================================================== */

/** 广播队列长度（NimBLE 回调入队，主任务出队解析） */
#define BLE_QUEUE_LEN              16

/* ================================================================== */
/* 4. 任务栈与优先级                                                   */
/* ================================================================== */

#define TASK_STACK_BLE             4096
#define TASK_STACK_STATE           4096
#define TASK_STACK_FX              4096

#define TASK_PRIO_BLE                 6
#define TASK_PRIO_STATE               5
#define TASK_PRIO_FX                  4

/* ================================================================== */
/* 5. NVS                                                              */
/* ================================================================== */

#define NVS_NAMESPACE              "petlamp"

/* ================================================================== */
/* 6. 诊断                                                             */
/* ================================================================== */

#define FIRMWARE_VERSION           "pet-status-lamp-idf/1.0.0"

#endif /* APP_CONFIG_H */
