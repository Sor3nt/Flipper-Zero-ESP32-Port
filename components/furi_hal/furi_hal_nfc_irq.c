/**
 * @file furi_hal_nfc_irq.c
 * @brief NFC IRQ handling for the ST25R3916 front-end.
 *
 * Ported from the Flipper Zero firmware with ESP-IDF specifics: the IRQ line
 * is a plain GPIO (gpio_nfc_irq) and its level is polled with
 * furi_hal_gpio_read(); the interrupt itself just wakes the waiting thread.
 */

#include "furi_hal_nfc_i.h"

#include <furi_hal_resources.h>

static void furi_hal_nfc_int_callback(void* context) {
    UNUSED(context);
    furi_hal_nfc_event_set(FuriHalNfcEventInternalTypeIrq);
}

uint32_t furi_hal_nfc_get_irq(const FuriHalSpiBusHandle* handle) {
    uint32_t irq = 0;
    /* The ST25R3916 releases its IRQ line only after every pending interrupt
     * has been acknowledged, hence the loop. */
    while(furi_hal_gpio_read(&gpio_nfc_irq)) {
        irq |= st25r3916_get_irq(handle);
    }
    return irq;
}

void furi_hal_nfc_init_gpio_isr(void) {
    furi_hal_gpio_init(
        &gpio_nfc_irq, GpioModeInterruptRise, GpioPullDown, GpioSpeedVeryHigh);
    furi_hal_gpio_add_int_callback(&gpio_nfc_irq, furi_hal_nfc_int_callback, NULL);
    furi_hal_gpio_enable_int_callback(&gpio_nfc_irq);
}

void furi_hal_nfc_deinit_gpio_isr(void) {
    furi_hal_gpio_remove_int_callback(&gpio_nfc_irq);
    furi_hal_gpio_init(&gpio_nfc_irq, GpioModeAnalog, GpioPullNo, GpioSpeedLow);
}