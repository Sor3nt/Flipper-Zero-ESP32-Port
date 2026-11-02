#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BAUDRATE (115200)

/* Size of one console chunk handed to the RX callback. */
#define RX_BUF_SIZE (1024)

typedef struct EspFlasherUart EspFlasherUart;

typedef void (*EspFlasherUartRxCallback)(uint8_t* buf, size_t len, void* context);

/* Claims the shared external UART and starts the console RX thread.
 * Returns NULL when the board has no UART or the pads are busy. */
EspFlasherUart* esp_flasher_uart_init(void* context);
void esp_flasher_uart_free(EspFlasherUart* uart);

void esp_flasher_uart_set_handle_rx_data_cb(EspFlasherUart* uart, EspFlasherUartRxCallback cb);

bool esp_flasher_uart_tx(EspFlasherUart* uart, const uint8_t* data, size_t len);
bool esp_flasher_uart_set_br(EspFlasherUart* uart, uint32_t baud);

/* Exclusive raw RX for the flasher protocol: parks the console RX thread so it cannot
 * steal protocol bytes. Every claim must be released. */
void esp_flasher_uart_claim_rx(EspFlasherUart* uart);
void esp_flasher_uart_release_rx(EspFlasherUart* uart);
/* Only valid while RX is claimed. Returns the number of bytes read (<= size). */
size_t esp_flasher_uart_read(EspFlasherUart* uart, uint8_t* data, size_t size, uint32_t timeout_ms);
void esp_flasher_uart_flush_rx(EspFlasherUart* uart);
