#include "gpio_items.h"

#include <furi.h>
#include <furi_hal_resources.h>
#include <stdlib.h>

struct GPIOItems {
    GpioPinRecord* pins;
    size_t count;
};

GPIOItems* gpio_items_alloc(void) {
    GPIOItems* items = malloc(sizeof(GPIOItems));
    furi_check(items);

    items->count = 0;
    for(size_t i = 0; i < gpio_pins_count; i++) {
        if(!gpio_pins[i].debug) {
            items->count++;
        }
    }

    items->pins = malloc(sizeof(GpioPinRecord) * items->count);
    furi_check(items->pins || items->count == 0);
    size_t index = 0;
    for(size_t i = 0; i < gpio_pins_count; i++) {
        if(!gpio_pins[i].debug) {
            items->pins[index].pin = gpio_pins[i].pin;
            items->pins[index].name = gpio_pins[i].name;
            index++;
        }
    }
    return items;
}

void gpio_items_free(GPIOItems* items) {
    free(items->pins);
    free(items);
}

uint8_t gpio_items_get_count(GPIOItems* items) {
    return items->count;
}

/* A pin another subsystem has reserved (e.g. an attached external CC1101's CS)
 * must not be driven from here: init is refused by the HAL, but a raw write is not. */
static bool gpio_items_pin_is_reserved(GPIOItems* items, uint8_t index) {
    return furi_hal_gpio_get_reservation(items->pins[index].pin) != NULL;
}

void gpio_items_configure_pin(GPIOItems* items, uint8_t index, GpioMode mode) {
    furi_assert(index < items->count);
    if(gpio_items_pin_is_reserved(items, index)) return;
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
    if(gpio_items_pin_is_reserved(items, index)) return;
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
