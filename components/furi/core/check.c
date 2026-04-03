#include "check.h"
#include "common_defines.h"

#include <furi_hal_power.h>
#include <furi_hal_rtc.h>

#include <stdio.h>
#include <stdlib.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_system.h>

// Forward declaration
void furi_log_puts(const char* data);

static void __furi_put_uint32_as_text(uint32_t data) {
    char tmp_str[] = "-2147483648";
    itoa(data, tmp_str, 10);
    furi_log_puts(tmp_str);
}

static void __furi_print_stack_info(void) {
    furi_log_puts("\r\n\tstack watermark: ");
    __furi_put_uint32_as_text(uxTaskGetStackHighWaterMark(NULL) * 4);
}

static void __furi_print_heap_info(void) {
    furi_log_puts("\r\n\t heap free: ");
    __furi_put_uint32_as_text(xPortGetFreeHeapSize());
    furi_log_puts("\r\n\t heap watermark: ");
    __furi_put_uint32_as_text(xPortGetMinimumEverFreeHeapSize());
}

static void __furi_print_name(bool isr) {
    if(isr) {
        furi_log_puts("[ISR] ");
    } else {
        const char* name = pcTaskGetName(NULL);
        if(name == NULL) {
            furi_log_puts("[main] ");
        } else {
            furi_log_puts("[");
            furi_log_puts(name);
            furi_log_puts("] ");
        }
    }
}

FURI_NORETURN void __furi_crash_implementation(const char* message) {
    portDISABLE_INTERRUPTS();

    bool isr = FURI_IS_IRQ_MODE();

    if(message == NULL) {
        message = "Fatal Error";
    } else if(message == (const char*)__FURI_ASSERT_MESSAGE_FLAG) {
        message = "furi_assert failed";
    } else if(message == (const char*)__FURI_CHECK_MESSAGE_FLAG) {
        message = "furi_check failed";
    }

    furi_log_puts("\r\n\033[0;31m[CRASH]");
    __furi_print_name(isr);
    furi_log_puts(message);

    if(!isr) {
        __furi_print_stack_info();
    }
    __furi_print_heap_info();

    furi_log_puts("\r\n\033[0m\r\n");

    // On ESP32, use abort() which triggers core dump if configured
    abort();
    __builtin_unreachable();
}

FURI_NORETURN void __furi_halt_implementation(const char* message) {
    portDISABLE_INTERRUPTS();

    if(message == NULL) {
        message = "System halt requested.";
    }

    furi_log_puts("\r\n\033[0;31m[HALT]");
    __furi_print_name(FURI_IS_IRQ_MODE());
    furi_log_puts(message);
    furi_log_puts("\r\nSystem halted. Bye-bye!\r\n");
    furi_log_puts("\033[0m\r\n");

    // Halt: infinite loop
    while(1) {
        __asm__ volatile("nop");
    }
    __builtin_unreachable();
}
