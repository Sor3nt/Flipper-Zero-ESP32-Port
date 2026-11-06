#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct UsbUartBridge UsbUartBridge;

typedef struct {
    uint8_t vcp_ch;
    uint8_t uart_ch;
    uint8_t flow_pins;
    uint8_t baudrate_mode;
    uint32_t baudrate;
    uint8_t software_de_re;
} UsbUartConfig;

typedef struct {
    uint32_t rx_cnt;
    uint32_t tx_cnt;
    uint32_t baudrate_cur;
    bool active;
    bool error;
} UsbUartState;

UsbUartBridge* usb_uart_enable(UsbUartConfig* cfg);

/* Returns false when cleanup could not safely release all resources.
 * The bridge object remains valid so the caller may retry. */
bool usb_uart_disable(UsbUartBridge* usb_uart);

void usb_uart_set_config(UsbUartBridge* usb_uart, UsbUartConfig* cfg);

void usb_uart_get_config(UsbUartBridge* usb_uart, UsbUartConfig* cfg);

void usb_uart_get_state(UsbUartBridge* usb_uart, UsbUartState* st);
