#pragma once

#include <stdint.h>
#include <stdbool.h>

#define USB_UART_BAUDRATE_PRESET_COUNT 12U

extern const uint32_t usb_uart_baudrate_presets[USB_UART_BAUDRATE_PRESET_COUNT];

typedef struct UsbUartBridge UsbUartBridge;

typedef struct {
    uint8_t usb_channel;
    uint8_t baudrate_mode;
    uint8_t uart_pin_pair;
    uint8_t control_pin_pair;
    uint8_t de_re_pin;
    uint32_t baudrate;
} UsbUartConfig;

typedef enum {
    UsbUartStatusStarting,
    UsbUartStatusActive,
    UsbUartStatusMemoryError,
    UsbUartStatusUnsupported,
    UsbUartStatusUsbBusy,
    UsbUartStatusUartBusy,
    UsbUartStatusUsbError,
    UsbUartStatusUartError,
    UsbUartStatusCleanupError,
} UsbUartStatus;

typedef struct {
    uint32_t rx_cnt;
    uint32_t tx_cnt;
    uint32_t baudrate_cur;
    UsbUartStatus status;
    bool usb_connected;
} UsbUartState;

UsbUartBridge* usb_uart_enable(UsbUartConfig* cfg);

bool usb_uart_disable(UsbUartBridge* usb_uart);

void usb_uart_set_config(UsbUartBridge* usb_uart, UsbUartConfig* cfg);

void usb_uart_get_config(UsbUartBridge* usb_uart, UsbUartConfig* cfg);

void usb_uart_get_state(UsbUartBridge* usb_uart, UsbUartState* st);

const char* usb_uart_get_status_text(UsbUartStatus status);

void usb_uart_config_init(UsbUartConfig* cfg);
uint8_t usb_uart_get_usb_channel_count(void);
uint8_t usb_uart_get_uart_pin_pair_count(void);
int usb_uart_get_uart_tx_pin(uint8_t index);
int usb_uart_get_uart_rx_pin(uint8_t index);
const char* usb_uart_get_uart_pin_pair_text(uint8_t index);
uint8_t usb_uart_get_control_pin_pair_count(void);
const char* usb_uart_get_control_pin_pair_text(uint8_t index);
uint8_t usb_uart_get_de_re_pin_count(void);
const char* usb_uart_get_de_re_pin_text(uint8_t index);
