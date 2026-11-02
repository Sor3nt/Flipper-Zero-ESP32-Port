#pragma once

#include "furi_hal.h"
#include "bw16_app.h"

#define BW16_UART_RX_BUF_SIZE (1024)

typedef struct Bw16Uart Bw16Uart;

/* The callback runs in the UART worker thread, NOT the GUI thread. */
void bw16_uart_set_handle_rx_data_cb(
    Bw16Uart* uart,
    void (*handle_rx_data_cb)(uint8_t* buf, size_t len, void* context));

void bw16_uart_tx(Bw16Uart* uart, const uint8_t* data, size_t len);

/* Convenience: send a command string followed by '\n'. */
void bw16_uart_send_line(Bw16Uart* uart, const char* line);

Bw16Uart* bw16_uart_init(Bw16App* app);
void bw16_uart_free(Bw16Uart* uart);
