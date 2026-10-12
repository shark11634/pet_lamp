/*
 * pet_types.h — 平台无关的基础类型（core 层共用）
 *
 * 本文件与 pet_config.h 一起构成 core/ 的公共基础：
 *   core/ 内所有 .c/.h **不得** include ESP-IDF / ESPHome / Arduino 头文件。
 *   任何平台交互都必须通过入参或函数指针回调传进来。
 */
#ifndef PET_TYPES_H
#define PET_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 宠物活动状态（佩戴端上报，协议 §2 偏移 7）                          */
/* ------------------------------------------------------------------ */

typedef enum {
    PET_STATE_QUIET  = 0,
    PET_STATE_LIGHT  = 1,
    PET_STATE_ACTIVE = 2,
} pet_activity_state_t;

/* ------------------------------------------------------------------ */
/* 灯端本地状态（需求 4.3 / 4.4）                                      */
/* Lost 与 Boot 只存在于灯端，不出现在广播中                            */
/* ------------------------------------------------------------------ */

typedef enum {
    PET_LAMP_BOOT   = 0,
    PET_LAMP_QUIET  = 1,
    PET_LAMP_LIGHT  = 2,
    PET_LAMP_ACTIVE = 3,
    PET_LAMP_LOST   = 4,
} pet_lamp_state_t;

/* ------------------------------------------------------------------ */
/* 逻辑灯光颜色                                                        */
/* ------------------------------------------------------------------ */

/** 逻辑输出颜色/亮度。四个通道均为 0–255 的**逻辑**亮度（线性），
 *  Gamma 校正与限流由平台输出层负责。 */
typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t w;   /**< 白光通道 */
} lamp_color_t;

/*
 * 定点平滑状态（Q8：0 .. 255*256）。
 *
 * 为什么需要它：过渡是 50 Hz 的一阶低通，当 alpha 较小时每帧增量可能
 * **小于 1 个色阶**（例如 5 × 0.015 = 0.074）。若把中间状态存成 8-bit，
 * 取整会把这个增量完全抹掉，输出被永久钉在原地 —— 低亮度下表现为
 * "呼吸完全看不见"。Q8 保留 1/256 色阶的精度，跨过阈值时占空比才 +1。
 */
typedef struct {
    uint32_t r;
    uint32_t g;
    uint32_t b;
    uint32_t w;
} lamp_color_q8_t;

#define LAMP_Q8_SHIFT 8u
#define LAMP_Q8_ONE   (1u << LAMP_Q8_SHIFT)
#define LAMP_Q8_MAX   (255u * LAMP_Q8_ONE)

/** Q8 -> 常规颜色（四舍五入到最近色阶） */
static inline void lamp_q8_to_color(const lamp_color_q8_t *q, lamp_color_t *out)
{
    out->r = (uint8_t)((q->r + (LAMP_Q8_ONE / 2u)) >> LAMP_Q8_SHIFT);
    out->g = (uint8_t)((q->g + (LAMP_Q8_ONE / 2u)) >> LAMP_Q8_SHIFT);
    out->b = (uint8_t)((q->b + (LAMP_Q8_ONE / 2u)) >> LAMP_Q8_SHIFT);
    out->w = (uint8_t)((q->w + (LAMP_Q8_ONE / 2u)) >> LAMP_Q8_SHIFT);
}

/** 常规颜色 -> Q8（带钳位，防止外部传入越界值） */
static inline void lamp_color_to_q8(const lamp_color_t *c, lamp_color_q8_t *q)
{
    q->r = (uint32_t)c->r << LAMP_Q8_SHIFT;
    q->g = (uint32_t)c->g << LAMP_Q8_SHIFT;
    q->b = (uint32_t)c->b << LAMP_Q8_SHIFT;
    q->w = (uint32_t)c->w << LAMP_Q8_SHIFT;
}

/* ------------------------------------------------------------------ */
/* 平台输出回调                                                        */
/* ------------------------------------------------------------------ */

/*
 * 灯效引擎把算好的结果交给平台输出层。
 *
 * @param color   逻辑颜色（已含状态色、呼吸、恢复动画、低电量提示叠加）
 * @param layer   0.0–1.0 的**层系数**（呼吸 / 失联降亮）
 *                必须在 Gamma 之后相乘，否则 2%–6% 亮度下波形会被量化吃掉
 *                （见 docs/最终可执行方案.md §4 WP1 与已修缺陷 #4）
 * @param floor_duty 层系数为 0 时仍要保留的最小占空比（0 = 允许完全熄灭）
 * @param user    平台注册时传入的上下文指针
 */
typedef void (*lamp_output_fn)(const lamp_color_t *color, float layer,
                               uint16_t floor_duty, void *user);

/** 手动模式下的固定输出（网页 / MQTT / 面板设定） */
typedef struct {
    bool     on;
    uint8_t  brightness_pct;   /**< 1–100，对齐米家属性 */
    uint32_t color_temp_k;     /**< 1700–6500，Day 模式用 */
    uint32_t rgb;              /**< 1–16777215，Color 模式用 */
    uint8_t  mode;             /**< 1 = Color，2 = Day */
} pet_manual_t;

#ifdef __cplusplus
}
#endif

#endif /* PET_TYPES_H */
