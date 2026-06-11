#include "furi_hal_serial_control.h"
#include "furi_hal_serial_types_i.h"
#include "board.h"
#include <furi.h>

#ifndef BOARD_PIN_UART1_TX
#define BOARD_PIN_UART1_TX UART_PIN_NO_CHANGE
#endif
#ifndef BOARD_PIN_UART1_RX
#define BOARD_PIN_UART1_RX UART_PIN_NO_CHANGE
#endif
#ifndef BOARD_PIN_UART2_TX
#define BOARD_PIN_UART2_TX UART_PIN_NO_CHANGE
#endif
#ifndef BOARD_PIN_UART2_RX
#define BOARD_PIN_UART2_RX UART_PIN_NO_CHANGE
#endif

#define MAX_HANDLES 2

static FuriHalSerialHandle handles[MAX_HANDLES];
static bool control_initialized = false;

void furi_hal_serial_control_init(void) {
    if(control_initialized) return;

    for(int i = 0; i < MAX_HANDLES; i++) {
        handles[i].id = (FuriHalSerialId)i;
        handles[i].in_use = false;
        handles[i].is_init = false;
        handles[i].baudrate = 0;
        handles[i].async_rx_cb = NULL;
        handles[i].async_rx_ctx = NULL;
        handles[i].reader_task = NULL;
        handles[i].event_queue = NULL;
        handles[i].byte_valid = false;
        handles[i].async_rx_running = false;
    }

    handles[FuriHalSerialIdUsart].uart_num = UART_NUM_1;
    handles[FuriHalSerialIdUsart].tx_pin = BOARD_PIN_UART1_TX;
    handles[FuriHalSerialIdUsart].rx_pin = BOARD_PIN_UART1_RX;

#if SOC_UART_NUM > 1
    handles[FuriHalSerialIdLpuart].uart_num = UART_NUM_2;
    handles[FuriHalSerialIdLpuart].tx_pin = BOARD_PIN_UART2_TX;
    handles[FuriHalSerialIdLpuart].rx_pin = BOARD_PIN_UART2_RX;
#else
    handles[FuriHalSerialIdLpuart].uart_num = UART_NUM_1;
    handles[FuriHalSerialIdLpuart].tx_pin = BOARD_PIN_UART1_TX;
    handles[FuriHalSerialIdLpuart].rx_pin = BOARD_PIN_UART1_RX;
#endif

    control_initialized = true;
}

void furi_hal_serial_control_deinit(void) {
    control_initialized = false;
}

FuriHalSerialHandle* furi_hal_serial_control_acquire(FuriHalSerialId serial_id) {
    furi_check(serial_id < FuriHalSerialIdMax);
    furi_check(serial_id < MAX_HANDLES);

    if(!control_initialized) {
        furi_hal_serial_control_init();
    }

    FuriHalSerialHandle* handle = &handles[serial_id];
    if(handle->in_use) {
        return NULL;
    }

    handle->in_use = true;
    return handle;
}

void furi_hal_serial_control_release(FuriHalSerialHandle* handle) {
    furi_check(handle);
    handle->in_use = false;
}

bool furi_hal_serial_control_is_busy(FuriHalSerialId serial_id) {
    furi_check(serial_id < FuriHalSerialIdMax);
    furi_check(serial_id < MAX_HANDLES);
    return handles[serial_id].in_use;
}
