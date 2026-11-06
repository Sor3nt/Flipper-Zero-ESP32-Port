#include "gpio_items.h"

#include <furi.h>
#include <furi_hal_resources.h>

/* The pin list exists only on boards with an external GPIO header
 * (BOARD_PIN_EXT_GPIO_1..5, see furi_hal_resources.h). Everywhere else the list is
 * empty and "GPIO Manual Control" is not offered. */
#if defined(BOARD_PIN_EXT_GPIO_1)
#define GPIO_ITEMS_HAVE_HEADER 1
#else
#define GPIO_ITEMS_HAVE_HEADER 0
#endif

typedef struct {
    const GpioPin* pin;
    const char* name;
} GpioItemPin;

struct GPIOItems {
    GpioItemPin* pins;
    size_t count;
};

bool gpio_items_manual_control_supported(void) {
    return GPIO_ITEMS_HAVE_HEADER;
}

GPIOItems* gpio_items_alloc(void) {
    GPIOItems* items = malloc(sizeof(GPIOItems));
    items->count = 0;
    items->pins = NULL;
#if GPIO_ITEMS_HAVE_HEADER

    for(size_t i = 0; i < gpio_pins_count; i++) {
        if(!gpio_pins[i].debug) {
            items->count++;
        }
    }

    items->pins = malloc(sizeof(GpioItemPin) * items->count);
    size_t index = 0;
    for(size_t i = 0; i < gpio_pins_count; i++) {
        if(!gpio_pins[i].debug) {
            items->pins[index].pin = gpio_pins[i].pin;
            items->pins[index].name = gpio_pins[i].name;
            index++;
        }
    }
#endif
    return items;
}

void gpio_items_free(GPIOItems* items) {
    free(items->pins);
    free(items);
}

uint8_t gpio_items_get_count(GPIOItems* items) {
    return items->count;
}

void gpio_items_configure_pin(GPIOItems* items, uint8_t index, GpioMode mode) {
    furi_assert(index < items->count);
    furi_hal_gpio_write(items->pins[index].pin, false);
    furi_hal_gpio_init(items->pins[index].pin, mode, GpioPullNo, GpioSpeedVeryHigh);
}

void gpio_items_configure_all_pins(GPIOItems* items, GpioMode mode) {
    for(uint8_t i = 0; i < items->count; i++) {
        gpio_items_configure_pin(items, i, mode);
    }
}

void gpio_items_set_pin(GPIOItems* items, uint8_t index, bool level) {
    furi_assert(index < items->count);
    furi_hal_gpio_write(items->pins[index].pin, level);
}

void gpio_items_set_all_pins(GPIOItems* items, bool level) {
    for(uint8_t i = 0; i < items->count; i++) {
        gpio_items_set_pin(items, i, level);
    }
}

const char* gpio_items_get_pin_name(GPIOItems* items, uint8_t index) {
    furi_assert(index < items->count + 1);
    if(index == items->count) {
        return "ALL";
    } else {
        return items->pins[index].name;
    }
}
