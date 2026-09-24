#pragma once
/* Host test boundary only. Never included by firmware or FAP builds. */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define UNUSED(x) (void)(x)
#define FuriWaitForever UINT32_MAX
#define RECORD_STORAGE "storage"
typedef struct FuriMutex FuriMutex;
typedef struct FuriMessageQueue FuriMessageQueue;
typedef struct FuriThread FuriThread;
typedef enum { FuriStatusOk } FuriStatus;
void* furi_record_open(const char* name);
void furi_record_close(const char* name);
FuriStatus furi_mutex_acquire(FuriMutex* mutex,uint32_t timeout);
FuriStatus furi_mutex_release(FuriMutex* mutex);
FuriStatus furi_message_queue_put(FuriMessageQueue* queue,const void* message,uint32_t timeout);
void furi_delay_ms(uint32_t ms);
void furi_delay_us(uint32_t us);
