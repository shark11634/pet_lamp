/*
 * host_stubs/esp_host_stub.c — ESP-IDF 外设在主机上的替身实现
 *
 * 目的：让真实的 main/lamp_driver.c 与 main/lamp_effects.c 能在主机上编译运行，
 * 从而验证 Gamma 曲线、全通道限流、灯效区间的**真实实现**（而不是复制品）。
 *
 * 只实现被测代码实际调用的那部分 API。
 */
#include "driver/gpio.h"
#include "driver/ledc.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* GPIO                                                              */
/* ------------------------------------------------------------------ */

#define HOST_GPIO_MAX 64
static int s_gpio_level[HOST_GPIO_MAX];

esp_err_t gpio_config(const gpio_config_t *cfg)
{
    return (cfg == NULL) ? ESP_ERR_INVALID_ARG : ESP_OK;
}

void gpio_set_level(int gpio_num, uint32_t level)
{
    if (gpio_num >= 0 && gpio_num < HOST_GPIO_MAX) {
        s_gpio_level[gpio_num] = (int)(level & 1u);
    }
}

int gpio_get_level(int gpio_num)
{
    if (gpio_num >= 0 && gpio_num < HOST_GPIO_MAX) {
        return s_gpio_level[gpio_num];
    }
    return 0;
}

esp_err_t gpio_install_isr_service(int intr_alloc_flags)
{
    (void)intr_alloc_flags;
    return ESP_OK;
}

esp_err_t gpio_isr_handler_add(int gpio_num, void (*isr_handler)(void *), void *args)
{
    (void)gpio_num;
    (void)isr_handler;
    (void)args;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* LEDC                                                              */
/* ------------------------------------------------------------------ */

static uint32_t s_ledc_duty[LEDC_CHANNEL_MAX];
static int      s_ledc_gpio[LEDC_CHANNEL_MAX];
static int      s_ledc_timer[LEDC_CHANNEL_MAX];
static uint32_t s_timer_freq[LEDC_TIMER_MAX];

esp_err_t ledc_timer_config(const ledc_timer_config_t *cfg)
{
    if (cfg == NULL || cfg->timer_num >= LEDC_TIMER_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    s_timer_freq[cfg->timer_num] = cfg->freq_hz;
    return ESP_OK;
}

esp_err_t ledc_channel_config(const ledc_channel_config_t *cfg)
{
    if (cfg == NULL || cfg->channel >= LEDC_CHANNEL_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    s_ledc_gpio[cfg->channel] = cfg->gpio_num;
    s_ledc_timer[cfg->channel] = (int)cfg->timer_sel;
    s_ledc_duty[cfg->channel] = cfg->duty;
    return ESP_OK;
}

esp_err_t ledc_set_duty(ledc_mode_t speed_mode, ledc_channel_t channel, uint32_t duty)
{
    (void)speed_mode;
    if (channel >= LEDC_CHANNEL_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    s_ledc_duty[channel] = duty;
    return ESP_OK;
}

esp_err_t ledc_update_duty(ledc_mode_t speed_mode, ledc_channel_t channel)
{
    (void)speed_mode;
    (void)channel;
    return ESP_OK;
}

uint32_t ledc_get_freq(ledc_mode_t speed_mode, ledc_timer_t timer)
{
    (void)speed_mode;
    if (timer >= LEDC_TIMER_MAX) {
        return 0u;
    }
    return s_timer_freq[timer];
}

uint32_t host_ledc_duty(ledc_channel_t channel)
{
    return (channel < LEDC_CHANNEL_MAX) ? s_ledc_duty[channel] : 0u;
}

int host_ledc_gpio(ledc_channel_t channel)
{
    return (channel < LEDC_CHANNEL_MAX) ? s_ledc_gpio[channel] : -1;
}

int host_ledc_timer(ledc_channel_t channel)
{
    return (channel < LEDC_CHANNEL_MAX) ? s_ledc_timer[channel] : -1;
}

uint32_t host_ledc_timer_freq(ledc_timer_t timer)
{
    return (timer < LEDC_TIMER_MAX) ? s_timer_freq[timer] : 0u;
}

void host_ledc_reset(void)
{
    memset(s_ledc_duty, 0, sizeof(s_ledc_duty));
    memset(s_ledc_gpio, 0, sizeof(s_ledc_gpio));
    memset(s_ledc_timer, 0, sizeof(s_ledc_timer));
    memset(s_timer_freq, 0, sizeof(s_timer_freq));
}
