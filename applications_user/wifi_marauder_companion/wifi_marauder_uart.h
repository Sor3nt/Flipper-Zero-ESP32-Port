#pragma once

#include "furi_hal.h"

#define RX_BUF_SIZE (2048)

typedef struct WifiMarauderUart WifiMarauderUart;

void wifi_marauder_uart_set_handle_rx_data_cb(
    WifiMarauderUart* uart,
    void (*handle_rx_data_cb)(uint8_t* buf, size_t len, void* context));
void wifi_marauder_uart_set_handle_rx_pcap_cb(
    WifiMarauderUart* uart,
    void (*handle_rx_pcap_cb)(uint8_t* buf, size_t len, void* context));
void wifi_marauder_uart_tx(WifiMarauderUart* uart, uint8_t* data, size_t len);

/** Take the board's external UART header and start the receiver thread. NULL if the UART
 *  cannot be had: wifi_marauder_uart_last_error() then says why. */
WifiMarauderUart* wifi_marauder_usart_init(WifiMarauderApp* app);

/** Why the last wifi_marauder_usart_init() failed (never NULL). */
const char* wifi_marauder_uart_last_error(void);

/** Stop the receiver and give the UART header back. Retries a release that fails; what it still
 *  cannot give back stays reserved (and is logged). */
void wifi_marauder_uart_free(WifiMarauderUart* uart);
