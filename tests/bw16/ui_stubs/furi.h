#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#define UNUSED(x) (void)(x)
#define FuriWaitForever UINT32_MAX
#define RECORD_GUI "gui"
#define FURI_LOG_E(...) ((void)0)
typedef struct FuriMutex FuriMutex;
typedef struct FuriMessageQueue FuriMessageQueue;
typedef enum { FuriMutexTypeNormal } FuriMutexType;
typedef enum { FuriStatusOk, FuriStatusError } FuriStatus;
FuriMutex* furi_mutex_alloc(FuriMutexType type);
void furi_mutex_free(FuriMutex* mutex);
FuriStatus furi_mutex_acquire(FuriMutex* mutex,uint32_t timeout);
FuriStatus furi_mutex_release(FuriMutex* mutex);
FuriMessageQueue* furi_message_queue_alloc(uint32_t count,uint32_t size);
void furi_message_queue_free(FuriMessageQueue* queue);
FuriStatus furi_message_queue_put(FuriMessageQueue* queue,const void* value,uint32_t timeout);
FuriStatus furi_message_queue_get(FuriMessageQueue* queue,void* value,uint32_t timeout);
uint32_t furi_get_tick(void);
void* furi_record_open(const char* name);
void furi_record_close(const char* name);
void furi_delay_ms(uint32_t ms);
void furi_delay_us(uint32_t us);
