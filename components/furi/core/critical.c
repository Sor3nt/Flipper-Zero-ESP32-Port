#include "common_defines.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static portMUX_TYPE furi_critical_mutex = portMUX_INITIALIZER_UNLOCKED;

__FuriCriticalInfo __furi_critical_enter(void) {
    __FuriCriticalInfo info;

    info.isrm = 0;
    info.from_isr = FURI_IS_ISR();
    info.kernel_running = (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING);

    /* From an ISR (or with interrupts already masked) take the same spinlock
     * as task context. Masking the local core alone would leave tasks on the
     * other core free to enter the section on a dual-core build. On a
     * single-core build the spinlock reduces to the same interrupt mask. */
    if(info.from_isr) {
        portENTER_CRITICAL_ISR(&furi_critical_mutex);
    } else if(info.kernel_running) {
        taskENTER_CRITICAL(&furi_critical_mutex);
    } else {
        portDISABLE_INTERRUPTS();
    }

    return info;
}

void __furi_critical_exit(__FuriCriticalInfo info) {
    if(info.from_isr) {
        portEXIT_CRITICAL_ISR(&furi_critical_mutex);
    } else if(info.kernel_running) {
        taskEXIT_CRITICAL(&furi_critical_mutex);
    } else {
        portENABLE_INTERRUPTS();
    }
}
