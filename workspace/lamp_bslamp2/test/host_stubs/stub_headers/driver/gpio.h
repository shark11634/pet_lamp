/*
 * host_stubs/stub_headers/driver/gpio.h — 主机测试替身
 *
 * 只提供 lamp_driver.c 用到的部分，行为与 ESP-IDF 一致（gpio_set_level 返回 void）。
 */
#ifndef STUB_DRIVER_GPIO_H
#define STUB_DRIVER_GPIO_H

#include <stdint.h>

#include "esp_err.h"

typedef enum {
    GPIO_MODE_DISABLE = 0,
    GPIO_MODE_INPUT,
    GPIO_MODE_OUTPUT,
    GPIO_MODE_OUTPUT_OD,
    GPIO_MODE_INPUT_OUTPUT_OD,
    GPIO_MODE_INPUT_OUTPUT,
} gpio_mode_t;

typedef enum {
    GPIO_PULLUP_DISABLE = 0,
    GPIO_PULLUP_ENABLE,
} gpio_pullup_t;

typedef enum {
    GPIO_PULLDOWN_DISABLE = 0,
    GPIO_PULLDOWN_ENABLE,
} gpio_pulldown_t;

typedef enum {
    GPIO_INTR_DISABLE = 0,
    GPIO_INTR_POSEDGE,
    GPIO_INTR_NEGEDGE,
    GPIO_INTR_ANYEDGE,
    GPIO_INTR_LOW_LEVEL,
    GPIO_INTR_HIGH_LEVEL,
} gpio_int_type_t;

typedef struct {
    uint64_t pin_bit_mask;
    gpio_mode_t mode;
    gpio_pullup_t pull_up_en;
    gpio_pulldown_t pull_down_en;
    gpio_int_type_t intr_type;
} gpio_config_t;

esp_err_t gpio_config(const gpio_config_t *cfg);

/* ESP-IDF 各版本中 gpio_set_level 的返回类型为 void */
void gpio_set_level(int gpio_num, uint32_t level);

int gpio_get_level(int gpio_num);

esp_err_t gpio_install_isr_service(int intr_alloc_flags);
esp_err_t gpio_isr_handler_add(int gpio_num, void (*isr_handler)(void *), void *args);

#endif /* STUB_DRIVER_GPIO_H */
