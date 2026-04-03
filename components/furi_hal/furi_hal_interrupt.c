#include "furi_hal_interrupt.h"
#include <stddef.h>

const char* furi_hal_interrupt_get_name(uint8_t exception_number) {
    (void)exception_number;
    return NULL;
}

uint32_t furi_hal_interrupt_get_time_in_isr_total(void) {
    return 0;
}
