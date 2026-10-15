#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define FuriWaitForever UINT32_MAX
typedef struct FuriThread FuriThread;
typedef struct FuriMessageQueue FuriMessageQueue;
typedef struct FuriMutex FuriMutex;
typedef enum { FuriStatusOk, FuriStatusError } FuriStatus;
typedef enum { FuriMutexTypeNormal } FuriMutexType;
typedef enum { FuriThreadPriorityHigh } FuriThreadPriority;
uint32_t furi_get_tick(void);
uint32_t furi_kernel_get_tick_frequency(void);
void furi_delay_tick(uint32_t ticks);
FuriThread *furi_thread_alloc_ex(const char *, uint32_t, int32_t (*)(void *), void *);
void furi_thread_set_priority(FuriThread *, FuriThreadPriority);
void furi_thread_start(FuriThread *);
void furi_thread_join(FuriThread *);
void furi_thread_free(FuriThread *);
FuriMessageQueue *furi_message_queue_alloc(uint32_t, uint32_t);
FuriStatus furi_message_queue_put(FuriMessageQueue *, const void *, uint32_t);
FuriStatus furi_message_queue_get(FuriMessageQueue *, void *, uint32_t);
void furi_message_queue_free(FuriMessageQueue *);
FuriMutex *furi_mutex_alloc(FuriMutexType);
void furi_mutex_acquire(FuriMutex *, uint32_t);
void furi_mutex_release(FuriMutex *);
void furi_mutex_free(FuriMutex *);
