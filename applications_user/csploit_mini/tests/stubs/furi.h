#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct FuriThread FuriThread;typedef struct FuriMessageQueue FuriMessageQueue;
typedef enum{FuriStatusOk,FuriStatusError}FuriStatus;
uint32_t furi_get_tick(void);uint32_t furi_kernel_get_tick_frequency(void);void furi_delay_ms(uint32_t ms);
FuriThread *furi_thread_alloc_ex(const char *name,uint32_t stack,int32_t(*fn)(void*),void *ctx);
void furi_thread_start(FuriThread *t);void furi_thread_join(FuriThread *t);void furi_thread_free(FuriThread *t);
FuriMessageQueue *furi_message_queue_alloc(uint32_t count,uint32_t size);
FuriStatus furi_message_queue_put(FuriMessageQueue *q,const void *item,uint32_t timeout);
FuriStatus furi_message_queue_get(FuriMessageQueue *q,void *item,uint32_t timeout);
void furi_message_queue_free(FuriMessageQueue *q);
