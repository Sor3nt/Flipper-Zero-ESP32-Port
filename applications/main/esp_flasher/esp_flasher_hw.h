#pragma once

/* Board mapping for the ESP Flasher.
 *
 * The flasher talks to the target over the board's external UART, the same pads the
 * USB-UART Bridge (or, on boards without it, the RFID reader) uses. Those pads are shared,
 * so every session goes through furi_hal_uart_shared.
 *
 * Optional auto-reset: boards that expose USB-UART RTS/DTR control GPIOs reuse them as
 * target EN (RTS) and IO0 (DTR), the same assignment esptool's auto-reset circuit uses. */

#include <boards/board.h>

#if defined(BOARD_HAS_USB_UART_BRIDGE) && BOARD_HAS_USB_UART_BRIDGE && \
    defined(BOARD_USB_UART_NUM) && defined(BOARD_USB_UART_TX) && defined(BOARD_USB_UART_RX)
#define ESP_FLASHER_HAS_UART 1
#define ESP_FLASHER_UART_NUM BOARD_USB_UART_NUM
#define ESP_FLASHER_UART_TX  BOARD_USB_UART_TX
#define ESP_FLASHER_UART_RX  BOARD_USB_UART_RX
#elif defined(BOARD_RFID_UART_NUM) && defined(BOARD_PIN_RFID_TX) && defined(BOARD_PIN_RFID_RX)
#define ESP_FLASHER_HAS_UART 1
#define ESP_FLASHER_UART_NUM BOARD_RFID_UART_NUM
#define ESP_FLASHER_UART_TX  BOARD_PIN_RFID_TX
#define ESP_FLASHER_UART_RX  BOARD_PIN_RFID_RX
#else
#define ESP_FLASHER_HAS_UART 0
#endif

#if ESP_FLASHER_HAS_UART && defined(BOARD_USB_UART_RTS_GPIO) && \
    defined(BOARD_USB_UART_DTR_GPIO)
#define ESP_FLASHER_HAS_RESET_PINS 1
#define ESP_FLASHER_EN_GPIO        BOARD_USB_UART_RTS_GPIO
#define ESP_FLASHER_IO0_GPIO       BOARD_USB_UART_DTR_GPIO
#else
#define ESP_FLASHER_HAS_RESET_PINS 0
#endif

#define ESP_FLASHER_STR_(x) #x
#define ESP_FLASHER_STR(x)  ESP_FLASHER_STR_(x)
