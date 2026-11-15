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

/* Upper bound on the IRQ-acknowledge loop in furi_hal_nfc_get_irq(). A healthy
 * chip needs one or two rounds; the cap only exists so a wedged chip cannot
 * trap the worker in an infinite loop. */
#define FURI_HAL_NFC_IRQ_CLEAR_MAX 16

static void furi_hal_nfc_int_callback(void* context) {
    UNUSED(context);
    furi_hal_nfc_event_set(FuriHalNfcEventInternalTypeIrq);
}

uint32_t furi_hal_nfc_get_irq(const FuriHalSpiBusHandle* handle) {
    uint32_t irq = 0;
    /* The ST25R3916 releases its IRQ line only after every pending interrupt
     * has been acknowledged, hence the loop.
     *
     * The loop is bounded: a wedged chip (this board has no reset line, and a
     * previous run can leave the front-end with every register reading back
     * 0x00) can hold the line asserted without ever clearing a latched bit.
     * An unbounded loop here spins the highest-priority NFC worker inside a
     * transaction and freezes the whole system, so give up after a few tries
     * and let the caller's transaction fail and retry. */
    for(uint32_t guard = 0; guard < FURI_HAL_NFC_IRQ_CLEAR_MAX; guard++) {
        if(!furi_hal_gpio_read(&gpio_nfc_irq)) break;
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