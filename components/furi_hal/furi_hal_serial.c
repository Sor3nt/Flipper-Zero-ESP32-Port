/**
 * @file furi_hal_serial.c
 * ESP32 implementation of furi_hal_serial / furi_hal_serial_control.
 *
 * Wraps the shared external UART (furi_hal_uart_shared, GPIO43/44) and the ESP-IDF UART
 * driver. Only FuriHalSerialIdUsart is wired; FuriHalSerialIdLpuart is unavailable.
 */
#include "furi_hal_serial.h"
#include "furi_hal_serial_control.h"

#include <furi.h>
#include <furi_hal_power.h>
#include "furi_hal_uart_shared.h"
#include "boards/board.h"

#define TAG "FuriHalSerial"

#if defined(BOARD_RFID_UART_NUM) && defined(BOARD_PIN_RFID_TX) && defined(BOARD_PIN_RFID_RX)

#include <driver/gpio.h>
#include <driver/uart.h>

#define SERIAL_HAS_UART   1
#define SERIAL_UART_NUM   BOARD_RFID_UART_NUM
#define SERIAL_UART_TX    BOARD_PIN_RFID_TX
#define SERIAL_UART_RX    BOARD_PIN_RFID_RX

#define SERIAL_DRIVER_RX_BUF (4096)
#define SERIAL_READ_CHUNK    (64)
#define SERIAL_READ_SLICE_MS (20)
#define SERIAL_ACQUIRE_MS    (1000)

struct FuriHalSerialHandle {
    FuriHalSerialId id;
    bool acquired;
    bool driver_installed;
    bool inhibit_held;
    bool shared_held;

    FuriThread* rx_thread;
    volatile bool rx_stop;
    FuriHalSerialAsyncRxCallback rx_callback;
    void* rx_context;
    bool report_errors;

    uint8_t rx_buf[SERIAL_READ_CHUNK];
    size_t rx_len; // bytes currently in rx_buf
    size_t rx_pos; // index of the byte delivered to the current callback
};

static FuriHalSerialHandle s_handles[FuriHalSerialIdMax];

/* ---------------- control ---------------- */

FuriHalSerialHandle* furi_hal_serial_control_acquire(FuriHalSerialId serial_id) {
    if(serial_id >= FuriHalSerialIdMax) return NULL;
    if(serial_id != FuriHalSerialIdUsart) {
        FURI_LOG_W(TAG, "Only USART is wired on this board");
        return NULL;
    }
    FuriHalSerialHandle* handle = &s_handles[serial_id];
    if(handle->acquired) {
        FURI_LOG_W(TAG, "USART already acquired");
        return NULL;
    }
    if(furi_hal_uart_shared_acquire(SERIAL_ACQUIRE_MS) != FuriHalUartSharedAcquireAcquired) {
        FURI_LOG_W(TAG, "Shared UART is busy");
        return NULL;
    }
    memset(handle, 0, sizeof(*handle));
    handle->id = serial_id;
    handle->acquired = true;
    handle->shared_held = true;
    return handle;
}

void furi_hal_serial_control_release(FuriHalSerialHandle* handle) {
    furi_check(handle);
    furi_check(handle->acquired);
    if(handle->driver_installed) furi_hal_serial_deinit(handle);
    if(handle->shared_held) {
        if(!furi_hal_uart_shared_release()) FURI_LOG_E(TAG, "Shared UART release failed");
        handle->shared_held = false;
    }
    handle->acquired = false;
}

void furi_hal_serial_control_set_expansion_callback(
    FuriHalSerialId serial_id,
    FuriHalSerialControlExpansionCallback callback,
    void* context) {
    UNUSED(serial_id);
    UNUSED(callback);
    UNUSED(context);
    // Expansion detection is handled by the expansion service directly on this port.
}

/* ---------------- init / framing ---------------- */

void furi_hal_serial_init(FuriHalSerialHandle* handle, uint32_t baud_rate) {
    furi_check(handle);
    furi_check(handle->acquired);
    if(handle->driver_installed) return;

    if(uart_is_driver_installed(SERIAL_UART_NUM)) {
        FURI_LOG_E(TAG, "UART%d already installed", (int)SERIAL_UART_NUM);
        return;
    }

    esp_err_t err =
        uart_driver_install(SERIAL_UART_NUM, SERIAL_DRIVER_RX_BUF, 0, 0, NULL, 0);
    if(err == ESP_OK) {
        err = uart_param_config(
            SERIAL_UART_NUM,
            &(uart_config_t){
                .baud_rate = (int)baud_rate,
                .data_bits = UART_DATA_8_BITS,
                .parity = UART_PARITY_DISABLE,
                .stop_bits = UART_STOP_BITS_1,
                .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
                .source_clk = UART_SCLK_DEFAULT,
            });
    }
    if(err == ESP_OK) {
        err = uart_set_pin(
            SERIAL_UART_NUM, SERIAL_UART_TX, SERIAL_UART_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if(err == ESP_OK) {
        // An unconnected target must read as idle, not noise.
        gpio_set_pull_mode((gpio_num_t)SERIAL_UART_RX, GPIO_PULLUP_ONLY);
    }
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "UART init failed: %s", esp_err_to_name(err));
        if(uart_is_driver_installed(SERIAL_UART_NUM)) uart_driver_delete(SERIAL_UART_NUM);
        return;
    }

    uart_flush_input(SERIAL_UART_NUM);
    handle->driver_installed = true;
    // UART RX cannot wake the SoC; keep it awake for the session.
    furi_hal_power_light_sleep_inhibit_enter();
    handle->inhibit_held = true;
}

void furi_hal_serial_deinit(FuriHalSerialHandle* handle) {
    furi_check(handle);
    if(handle->rx_thread) furi_hal_serial_async_rx_stop(handle);
    if(handle->driver_installed) {
        uart_wait_tx_done(SERIAL_UART_NUM, pdMS_TO_TICKS(100));
        esp_err_t err = uart_driver_delete(SERIAL_UART_NUM);
        if(err != ESP_OK) FURI_LOG_E(TAG, "UART delete failed: %s", esp_err_to_name(err));
        handle->driver_installed = false;
    }
    if(handle->inhibit_held) {
        furi_hal_power_light_sleep_inhibit_exit();
        handle->inhibit_held = false;
    }
}

void furi_hal_serial_suspend(FuriHalSerialHandle* handle) {
    furi_check(handle);
    if(handle->rx_thread) furi_hal_serial_async_rx_stop(handle);
}

void furi_hal_serial_resume(FuriHalSerialHandle* handle) {
    UNUSED(handle);
    // RX is restarted by the caller via furi_hal_serial_async_rx_start().
}

void furi_hal_serial_set_br(FuriHalSerialHandle* handle, uint32_t baud_rate) {
    furi_check(handle);
    if(!handle->driver_installed) return;
    uart_wait_tx_done(SERIAL_UART_NUM, pdMS_TO_TICKS(50));
    esp_err_t err = uart_set_baudrate(SERIAL_UART_NUM, baud_rate);
    if(err != ESP_OK) FURI_LOG_E(TAG, "set_br %lu failed: %s", baud_rate, esp_err_to_name(err));
}

void furi_hal_serial_configure_framing(
    FuriHalSerialHandle* handle,
    FuriHalSerialDataBits data_bits,
    FuriHalSerialParity parity,
    FuriHalSerialStopBits stop_bits) {
    furi_check(handle);
    if(!handle->driver_installed) return;

    uart_word_length_t wl = UART_DATA_8_BITS;
    switch(data_bits) {
    case FuriHalSerialDataBits6:
        wl = UART_DATA_6_BITS;
        break;
    case FuriHalSerialDataBits7:
        wl = UART_DATA_7_BITS;
        break;
    default:
        wl = UART_DATA_8_BITS;
        break;
    }
    uart_parity_t par = (parity == FuriHalSerialParityEven) ? UART_PARITY_EVEN :
                        (parity == FuriHalSerialParityOdd)  ? UART_PARITY_ODD :
                                                              UART_PARITY_DISABLE;
    uart_stop_bits_t sb = (stop_bits == FuriHalSerialStopBits2) ? UART_STOP_BITS_2 :
                                                                  UART_STOP_BITS_1;
    uart_set_word_length(SERIAL_UART_NUM, wl);
    uart_set_parity(SERIAL_UART_NUM, par);
    uart_set_stop_bits(SERIAL_UART_NUM, sb);
}

bool furi_hal_serial_is_baud_rate_supported(FuriHalSerialHandle* handle, uint32_t baud_rate) {
    UNUSED(handle);
    // ESP-IDF programs a 12-bit integer divider off the 80 MHz APB clock.
    return baud_rate >= 77 && baud_rate <= 5000000;
}

/* ---------------- tx ---------------- */

void furi_hal_serial_tx(FuriHalSerialHandle* handle, const uint8_t* buffer, size_t buffer_size) {
    furi_check(handle);
    if(!handle->driver_installed || buffer_size == 0) return;
    uart_write_bytes(SERIAL_UART_NUM, buffer, buffer_size);
}

void furi_hal_serial_tx_wait_complete(FuriHalSerialHandle* handle) {
    furi_check(handle);
    if(handle->driver_installed) uart_wait_tx_done(SERIAL_UART_NUM, pdMS_TO_TICKS(1000));
}

/* ---------------- async rx ---------------- */

static int32_t furi_hal_serial_rx_thread(void* context) {
    FuriHalSerialHandle* handle = context;
    while(!handle->rx_stop) {
        const int len = uart_read_bytes(
            SERIAL_UART_NUM, handle->rx_buf, SERIAL_READ_CHUNK, pdMS_TO_TICKS(SERIAL_READ_SLICE_MS));
        if(len <= 0) continue;
        handle->rx_len = (size_t)len;
        for(handle->rx_pos = 0; handle->rx_pos < handle->rx_len && !handle->rx_stop;
            handle->rx_pos++) {
            if(handle->rx_callback) {
                handle->rx_callback(handle, FuriHalSerialRxEventData, handle->rx_context);
            }
        }
        handle->rx_len = 0;
    }
    return 0;
}

void furi_hal_serial_async_rx_start(
    FuriHalSerialHandle* handle,
    FuriHalSerialAsyncRxCallback callback,
    void* context,
    bool report_errors) {
    furi_check(handle);
    furi_check(handle->driver_installed);
    furi_check(!handle->rx_thread);

    handle->rx_callback = callback;
    handle->rx_context = context;
    handle->report_errors = report_errors;
    handle->rx_stop = false;
    handle->rx_thread =
        furi_thread_alloc_ex("FuriHalSerialRx", 2048, furi_hal_serial_rx_thread, handle);
    furi_thread_start(handle->rx_thread);
}

void furi_hal_serial_async_rx_stop(FuriHalSerialHandle* handle) {
    furi_check(handle);
    if(!handle->rx_thread) return;
    handle->rx_stop = true;
    furi_thread_join(handle->rx_thread);
    furi_thread_free(handle->rx_thread);
    handle->rx_thread = NULL;
    handle->rx_callback = NULL;
    handle->rx_context = NULL;
}

uint8_t furi_hal_serial_async_rx(FuriHalSerialHandle* handle) {
    furi_check(handle);
    if(handle->rx_pos < handle->rx_len) return handle->rx_buf[handle->rx_pos];
    return 0;
}

bool furi_hal_serial_async_rx_available(FuriHalSerialHandle* handle) {
    furi_check(handle);
    return (handle->rx_pos + 1) < handle->rx_len;
}

#else /* board without an external UART */

struct FuriHalSerialHandle {
    int dummy;
};

FuriHalSerialHandle* furi_hal_serial_control_acquire(FuriHalSerialId serial_id) {
    UNUSED(serial_id);
    return NULL;
}
void furi_hal_serial_control_release(FuriHalSerialHandle* handle) {
    UNUSED(handle);
}
void furi_hal_serial_control_set_expansion_callback(
    FuriHalSerialId serial_id,
    FuriHalSerialControlExpansionCallback callback,
    void* context) {
    UNUSED(serial_id);
    UNUSED(callback);
    UNUSED(context);
}
void furi_hal_serial_init(FuriHalSerialHandle* handle, uint32_t baud_rate) {
    UNUSED(handle);
    UNUSED(baud_rate);
}
void furi_hal_serial_deinit(FuriHalSerialHandle* handle) {
    UNUSED(handle);
}
void furi_hal_serial_suspend(FuriHalSerialHandle* handle) {
    UNUSED(handle);
}
void furi_hal_serial_resume(FuriHalSerialHandle* handle) {
    UNUSED(handle);
}
void furi_hal_serial_set_br(FuriHalSerialHandle* handle, uint32_t baud_rate) {
    UNUSED(handle);
    UNUSED(baud_rate);
}
void furi_hal_serial_configure_framing(
    FuriHalSerialHandle* handle,
    FuriHalSerialDataBits data_bits,
    FuriHalSerialParity parity,
    FuriHalSerialStopBits stop_bits) {
    UNUSED(handle);
    UNUSED(data_bits);
    UNUSED(parity);
    UNUSED(stop_bits);
}
bool furi_hal_serial_is_baud_rate_supported(FuriHalSerialHandle* handle, uint32_t baud_rate) {
    UNUSED(handle);
    UNUSED(baud_rate);
    return false;
}
void furi_hal_serial_tx(FuriHalSerialHandle* handle, const uint8_t* buffer, size_t buffer_size) {
    UNUSED(handle);
    UNUSED(buffer);
    UNUSED(buffer_size);
}
void furi_hal_serial_tx_wait_complete(FuriHalSerialHandle* handle) {
    UNUSED(handle);
}
void furi_hal_serial_async_rx_start(
    FuriHalSerialHandle* handle,
    FuriHalSerialAsyncRxCallback callback,
    void* context,
    bool report_errors) {
    UNUSED(handle);
    UNUSED(callback);
    UNUSED(context);
    UNUSED(report_errors);
}
void furi_hal_serial_async_rx_stop(FuriHalSerialHandle* handle) {
    UNUSED(handle);
}
uint8_t furi_hal_serial_async_rx(FuriHalSerialHandle* handle) {
    UNUSED(handle);
    return 0;
}
bool furi_hal_serial_async_rx_available(FuriHalSerialHandle* handle) {
    UNUSED(handle);
    return false;
}

#endif
