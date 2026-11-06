#pragma once

#include <furi_hal_gpio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GPIOItems GPIOItems;

GPIOItems* gpio_items_alloc(void);

void gpio_items_free(GPIOItems* items);

uint8_t gpio_items_get_count(GPIOItems* items);

/** True on boards with an external GPIO header (BOARD_PIN_EXT_GPIO_1..5, today Paradoxt).
 *  "GPIO Manual Control" is offered only there. */
bool gpio_items_manual_control_supported(void);

void gpio_items_configure_pin(GPIOItems* items, uint8_t index, GpioMode mode);

void gpio_items_configure_all_pins(GPIOItems* items, GpioMode mode);

void gpio_items_set_pin(GPIOItems* items, uint8_t index, bool level);

void gpio_items_set_all_pins(GPIOItems* items, bool level);

const char* gpio_items_get_pin_name(GPIOItems* items, uint8_t index);

#ifdef __cplusplus
}
#endif
