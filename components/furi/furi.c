#include "furi.h"

#include "core/thread_i.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

void furi_init(void) {
    furi_check(!furi_kernel_is_irq_or_masked());
    // ESP32: Scheduler is already running, don't check for NOT_STARTED
    // furi_check(xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED);

    furi_thread_init();
    furi_log_init();
    furi_record_init();
}

void furi_run(void) {
    furi_check(!furi_kernel_is_irq_or_masked());
    // ESP32: Scheduler is already running, nothing to start
    // vTaskStartScheduler() is not needed on ESP32
}

void furi_background(void) {
    furi_thread_scrub();
}
