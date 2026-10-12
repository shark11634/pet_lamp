/*
 * lamp_duty.c — 限流与占空比换算（纯函数、无状态、平台无关）
 *
 * 从 lamp_driver.c 拆出来的理由：
 *   lamp_driver.c 是**有状态封装**（持有 platform 回调与最近一次占空比），
 *   而这两个函数是纯计算。分开之后：
 *     · 测试可以直接对着真实实现验证，不必链接整个驱动
 *     · 集成测试可以用真实实现，不需要"替身驱动"这种东西
 *
 * 职责：
 *   lamp_apply_current_limit()  全通道限流（需求 4.5：不得超过电源能力）
 *   lamp_compute_duty()         逻辑颜色 + 层系数 → 4 通道占空比
 */
#include "lamp_driver.h"

#include "lamp_gamma.h"

void lamp_apply_current_limit(lamp_color_t *c)
{
    if (c == NULL) {
        return;
    }
    /* 以"逻辑亮度和"为口径：四通道满额 = 4 × 255 */
    const uint32_t full_scale = (uint32_t)LAMP_CH_COUNT * 255u;
    const uint32_t budget = (uint32_t)(LAMP_GLOBAL_CURRENT_LIMIT * (float)full_scale);

    const uint32_t sum = (uint32_t)c->r + c->g + c->b + c->w;
    if (sum <= budget || sum == 0u) {
        return;
    }

    const float scale = (float)budget / (float)sum;
    c->r = (uint8_t)((float)c->r * scale);
    c->g = (uint8_t)((float)c->g * scale);
    c->b = (uint8_t)((float)c->b * scale);
    c->w = (uint8_t)((float)c->w * scale);
}

void lamp_compute_duty(const lamp_color_t *c, float layer, uint16_t floor_duty,
                       uint16_t duty[LAMP_CH_COUNT])
{
    if (c == NULL || duty == NULL) {
        return;
    }
    /*
     * floor_duty 只用于"整体最低亮度"场景（例如恢复动画），
     * **不能**用来保住低亮呼吸 —— 那会把本来就该熄灭的通道（g/b/w = 0）
     * 一并抬到 floor，造成漏色（实测表现为手动红色模式下 g/b/w 都是 1）。
     * 低亮呼吸的防归零由下面 "base 非零且 layer 非零时至少 1" 保证。
     */
    (void)floor_duty;
    if (layer < 0.0f) layer = 0.0f;
    if (layer > 1.0f) layer = 1.0f;

    lamp_color_t lc = *c;
    lamp_apply_current_limit(&lc);

    const uint16_t base[LAMP_CH_COUNT] = {
        lamp_gamma_duty(lc.r, LAMP_RGB_CHANNEL_MAX),
        lamp_gamma_duty(lc.g, LAMP_RGB_CHANNEL_MAX),
        lamp_gamma_duty(lc.b, LAMP_RGB_CHANNEL_MAX),
        lamp_gamma_duty(lc.w, LAMP_WHITE_CHANNEL_MAX),
    };

    for (int i = 0; i < LAMP_CH_COUNT; ++i) {
        /*
         * 层乘法必须**四舍五入**，并在 base 非零时保留最小非零值：
         * 低亮度时 base 只有十几个 LSB，截断会把波形抹平（实测峰谷比 1.00）。
         * 只保证"该亮的通道不被层系数弄灭"，不该亮的通道保持 0。
         */
        uint32_t d = 0u;
        if (base[i] > 0u && layer > 0.0f) {
            d = (uint32_t)((float)base[i] * layer + 0.5f);
            if (d == 0u) {
                d = 1u;
            }
        }
        if (d > (uint32_t)LAMP_PWM_MAX_DUTY) {
            d = (uint32_t)LAMP_PWM_MAX_DUTY;
        }
        duty[i] = (uint16_t)d;
    }
}
