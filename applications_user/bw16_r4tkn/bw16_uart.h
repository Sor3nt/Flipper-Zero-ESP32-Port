#pragma once
#include <driver/uart.h>
#include <stdbool.h>
#include <furi_hal_bw16_guard.h>

typedef struct {
    bool leased, saved, installed, awake;
    QueueHandle_t events;
    const char* stage;
    esp_err_t error;
    FuriHalBw16Guard guard;
} Bw16Uart;
bool bw16_uart_open(Bw16Uart* io);
bool bw16_uart_scan(Bw16Uart* io);
esp_err_t bw16_uart_close(Bw16Uart* io);
