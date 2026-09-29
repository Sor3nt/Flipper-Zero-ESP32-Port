#pragma once
#include <stdbool.h>
#include <driver/uart.h>

#define BW16_UART UART_NUM_2
#define BW16_TX 43
#define BW16_RX 44

typedef struct {
    bool installed, awake, tx_failed;
    QueueHandle_t events;
    esp_err_t error;
    const char* stage;
} Bw16Uart;

bool bw16_uart_open(Bw16Uart* io);
esp_err_t bw16_uart_send(Bw16Uart* io, const char* line, size_t size);
esp_err_t bw16_uart_close(Bw16Uart* io);
