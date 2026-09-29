#pragma once
#include <driver/uart.h>
#define pdTRUE 1
unsigned uxQueueMessagesWaiting(QueueHandle_t queue);
int xQueueReceive(QueueHandle_t queue,void* value,unsigned timeout);
