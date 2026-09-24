#pragma once
#include <driver/uart.h>
#define GPIO_MODE_INPUT 1
#define GPIO_MODE_INPUT_OUTPUT 3
#define GPIO_PULLUP_ENABLE 1
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0
typedef struct { uint64_t pin_bit_mask; int mode,pull_up_en,pull_down_en,intr_type; } gpio_config_t;
esp_err_t gpio_config(const gpio_config_t* cfg);
esp_err_t gpio_set_level(unsigned pin, unsigned level);
int gpio_get_level(unsigned pin);
