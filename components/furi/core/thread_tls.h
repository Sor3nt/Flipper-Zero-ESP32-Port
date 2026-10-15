#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/* ESP-IDF pthread_local_storage.c owns slot 0, including its destructor.
 * lwIP calls pthread_getspecific() even from tasks created outside pthreads.
 * Keep Furi's thread identity in a distinct, application-owned slot. */
#define FURI_THREAD_TLS_INDEX 1
#if CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS <= FURI_THREAD_TLS_INDEX
#error "Furi requires at least two FreeRTOS TLS pointers; slot 0 belongs to ESP-IDF pthreads"
#endif

static inline void* furi_thread_local_get(TaskHandle_t task) {
    return pvTaskGetThreadLocalStoragePointer(task, FURI_THREAD_TLS_INDEX);
}

static inline void furi_thread_local_set(TaskHandle_t task, void* thread) {
    vTaskSetThreadLocalStoragePointer(task, FURI_THREAD_TLS_INDEX, thread);
}
