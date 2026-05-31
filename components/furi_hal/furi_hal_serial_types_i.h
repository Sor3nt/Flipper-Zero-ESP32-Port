#pragma once

#include "furi_hal_serial.h"
#include <driver/uart.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

struct FuriHalSerialHandle {
    FuriHalSerialId id;
    bool in_use;
    bool is_init;
    uart_port_t uart_num;
    int tx_pin;
    int rx_pin;
    uint32_t baudrate;
    FuriHalSerialAsyncRxCallback async_rx_cb;
    void* async_rx_ctx;
    TaskHandle_t reader_task;
    QueueHandle_t event_queue;
    uint8_t pending_byte;
    bool byte_valid;
    bool async_rx_running;
};
