#include "kernel.h"
#include "base.h"
#include "check.h"
#include "common_defines.h"
#include "thread.h"

#include <furi_hal_cortex.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

bool furi_kernel_is_irq_or_masked(void) {
    bool irq = false;
    BaseType_t state;

    if(FURI_IS_IRQ_MODE()) {
        irq = true;
    } else {
        state = xTaskGetSchedulerState();

        if(state != taskSCHEDULER_NOT_STARTED) {
            if(FURI_IS_IRQ_MASKED()) {
                irq = true;
            }
        }
    }

    return irq;
}

bool furi_kernel_is_running(void) {
    return xTaskGetSchedulerState() == taskSCHEDULER_RUNNING;
}

int32_t furi_kernel_lock(void) {
    furi_check(!furi_kernel_is_irq_or_masked());

    int32_t lock;

    switch(xTaskGetSchedulerState()) {
    case taskSCHEDULER_SUSPENDED:
        lock = 1;
        break;

    case taskSCHEDULER_RUNNING:
        vTaskSuspendAll();
        lock = 0;
        break;

    case taskSCHEDULER_NOT_STARTED:
    default:
        lock = (int32_t)FuriStatusError;
        break;
    }

    return lock;
}

int32_t furi_kernel_unlock(void) {
    furi_check(!furi_kernel_is_irq_or_masked());

    int32_t lock;

    switch(xTaskGetSchedulerState()) {
    case taskSCHEDULER_SUSPENDED:
        lock = 1;

        if(xTaskResumeAll() != pdTRUE) {
            if(xTaskGetSchedulerState() == taskSCHEDULER_SUSPENDED) {
                lock = (int32_t)FuriStatusError;
            }
        }
        break;

    case taskSCHEDULER_RUNNING:
        lock = 0;
        break;

    case taskSCHEDULER_NOT_STARTED:
    default:
        lock = (int32_t)FuriStatusError;
        break;
    }

    return lock;
}

int32_t furi_kernel_restore_lock(int32_t lock) {
    furi_check(!furi_kernel_is_irq_or_masked());

    switch(xTaskGetSchedulerState()) {
    case taskSCHEDULER_SUSPENDED:
    case taskSCHEDULER_RUNNING:
        if(lock == 1) {
            vTaskSuspendAll();
        } else {
            if(lock != 0) {
                lock = (int32_t)FuriStatusError;
            } else {
                if(xTaskResumeAll() != pdTRUE) {
                    if(xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
                        lock = (int32_t)FuriStatusError;
                    }
                }
            }
        }
        break;

    case taskSCHEDULER_NOT_STARTED:
    default:
        lock = (int32_t)FuriStatusError;
        break;
    }

    return lock;
}

uint32_t furi_kernel_get_tick_frequency(void) {
    return configTICK_RATE_HZ;
}

void furi_delay_tick(uint32_t ticks) {
    furi_check(!furi_kernel_is_irq_or_masked());
    furi_check(furi_thread_get_current_id() != xTaskGetIdleTaskHandle());

    if(ticks == 0U) {
        taskYIELD();
    } else {
        vTaskDelay(ticks);
    }
}

FuriStatus furi_delay_until_tick(uint32_t tick) {
    furi_check(!furi_kernel_is_irq_or_masked());
    furi_check(furi_thread_get_current_id() != xTaskGetIdleTaskHandle());

    TickType_t tcnt, delay;
    FuriStatus stat;

    stat = FuriStatusOk;
    tcnt = xTaskGetTickCount();

    delay = (TickType_t)tick - tcnt;

    if((delay != 0U) && (0 == (delay >> (8 * sizeof(TickType_t) - 1)))) {
        if(xTaskDelayUntil(&tcnt, delay) == pdFALSE) {
            stat = FuriStatusError;
        }
    } else {
        stat = FuriStatusErrorParameter;
    }

    return stat;
}

uint32_t furi_get_tick(void) {
    TickType_t ticks;

    if(furi_kernel_is_irq_or_masked() != 0U) {
        ticks = xTaskGetTickCountFromISR();
    } else {
        ticks = xTaskGetTickCount();
    }

    return ticks;
}

uint32_t furi_ms_to_ticks(uint32_t milliseconds) {
#if configTICK_RATE_HZ == 1000
    return milliseconds;
#else
    return (uint32_t)((float)configTICK_RATE_HZ) / 1000.0f * (float)milliseconds;
#endif
}

void furi_delay_ms(uint32_t milliseconds) {
    if(!FURI_IS_ISR() && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
        if(milliseconds > 0 && milliseconds < portMAX_DELAY - 1) {
            milliseconds += 1;
        }
#if configTICK_RATE_HZ == 1000
        furi_delay_tick(milliseconds);
#else
        furi_delay_tick(furi_ms_to_ticks(milliseconds));
#endif
    } else if(milliseconds > 0) {
        furi_delay_us(milliseconds * 1000);
    }
}

void furi_delay_us(uint32_t microseconds) {
    furi_hal_cortex_delay_us(microseconds);
}
