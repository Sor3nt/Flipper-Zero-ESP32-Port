#include "furi_hal_serial.h"
#include "furi_hal_serial_types_i.h"
#include "board.h"
#include <furi.h>
#include <driver/uart.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#define TAG "FuriHalSerial"
#define RX_BUF_SIZE 256
#define EVENT_QUEUE_SIZE 10

typedef struct {
    FuriHalSerialHandle* handle;
} ReaderTaskArg;

static void serial_reader_task(void* arg) {
    ReaderTaskArg* reader_arg = (ReaderTaskArg*)arg;
    FuriHalSerialHandle* handle = reader_arg->handle;
    free(reader_arg);

    uart_event_t event;
    uint8_t byte;

    while(handle->async_rx_running) {
        if(xQueueReceive(handle->event_queue, &event, portMAX_DELAY) == pdTRUE) {
            switch(event.type) {
            case UART_DATA:
                while(uart_read_bytes(handle->uart_num, &byte, 1, 0) > 0) {
                    handle->pending_byte = byte;
                    handle->byte_valid = true;
                    if(handle->async_rx_cb) {
                        handle->async_rx_cb(
                            handle, FuriHalSerialRxEventData, handle->async_rx_ctx);
                    }
                }
                break;
            case UART_FRAME_ERR:
                if(handle->async_rx_cb) {
                    handle->async_rx_cb(
                        handle, FuriHalSerialRxEventFrameError, handle->async_rx_ctx);
                }
                break;
            case UART_PARITY_ERR:
                if(handle->async_rx_cb) {
                    handle->async_rx_cb(
                        handle, FuriHalSerialRxEventParityError, handle->async_rx_ctx);
                }
                break;
            case UART_BUFFER_FULL:
            case UART_FIFO_OVF:
                if(handle->async_rx_cb) {
                    handle->async_rx_cb(
                        handle, FuriHalSerialRxEventOverrunError, handle->async_rx_ctx);
                }
                break;
            case UART_BREAK:
            default:
                break;
            }
        }
    }

    vTaskDelete(NULL);
}

void furi_hal_serial_init(FuriHalSerialHandle* handle, uint32_t baud) {
    furi_check(handle);
    furi_check(!handle->is_init);

    uart_config_t uart_config = {
        .baud_rate = baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_param_config(handle->uart_num, &uart_config));
    ESP_ERROR_CHECK(
        uart_set_pin(
            handle->uart_num,
            handle->tx_pin,
            handle->rx_pin,
            UART_PIN_NO_CHANGE,
            UART_PIN_NO_CHANGE));

    handle->event_queue = NULL;
    ESP_ERROR_CHECK(
        uart_driver_install(
            handle->uart_num, RX_BUF_SIZE, 0, EVENT_QUEUE_SIZE, &handle->event_queue, 0));

    handle->baudrate = baud;
    handle->is_init = true;

    FURI_LOG_D(TAG, "UART%zu init @ %lu baud", (size_t)(handle->uart_num), (unsigned long)baud);
}

void furi_hal_serial_deinit(FuriHalSerialHandle* handle) {
    furi_check(handle);
    furi_check(handle->is_init);

    if(handle->async_rx_running) {
        furi_hal_serial_async_rx_stop(handle);
    }

    uart_driver_delete(handle->uart_num);
    handle->event_queue = NULL;
    handle->is_init = false;
    handle->baudrate = 0;

    FURI_LOG_D(TAG, "UART%zu deinit", (size_t)(handle->uart_num));
}

void furi_hal_serial_suspend(FuriHalSerialHandle* handle) {
    UNUSED(handle);
}

void furi_hal_serial_resume(FuriHalSerialHandle* handle) {
    UNUSED(handle);
}

bool furi_hal_serial_is_baud_rate_supported(FuriHalSerialHandle* handle, uint32_t baud) {
    UNUSED(handle);
    return baud >= 300 && baud <= 5000000;
}

void furi_hal_serial_set_br(FuriHalSerialHandle* handle, uint32_t baud) {
    furi_check(handle);
    furi_check(handle->is_init);

    ESP_ERROR_CHECK(uart_set_baudrate(handle->uart_num, baud));
    handle->baudrate = baud;
}

void furi_hal_serial_configure_framing(
    FuriHalSerialHandle* handle,
    FuriHalSerialDataBits data_bits,
    FuriHalSerialParity parity,
    FuriHalSerialStopBits stop_bits) {
    furi_check(handle);
    furi_check(handle->is_init);

    uart_word_length_t word_len = UART_DATA_8_BITS;
    uart_parity_t parity_cfg = UART_PARITY_DISABLE;
    uart_stop_bits_t stop = UART_STOP_BITS_1;

    switch(data_bits) {
    case FuriHalSerialDataBits6:
        word_len = UART_DATA_6_BITS;
        break;
    case FuriHalSerialDataBits7:
        word_len = UART_DATA_7_BITS;
        break;
    case FuriHalSerialDataBits8:
        word_len = UART_DATA_8_BITS;
        break;
    case FuriHalSerialDataBits9:
        word_len = UART_DATA_8_BITS;
        break;
    default:
        break;
    }

    switch(parity) {
    case FuriHalSerialParityEven:
        parity_cfg = UART_PARITY_EVEN;
        break;
    case FuriHalSerialParityOdd:
        parity_cfg = UART_PARITY_ODD;
        break;
    default:
        parity_cfg = UART_PARITY_DISABLE;
        break;
    }

    switch(stop_bits) {
    case FuriHalSerialStopBits0_5:
    case FuriHalSerialStopBits1:
        stop = UART_STOP_BITS_1;
        break;
    case FuriHalSerialStopBits1_5:
        stop = UART_STOP_BITS_1_5;
        break;
    case FuriHalSerialStopBits2:
        stop = UART_STOP_BITS_2;
        break;
    default:
        break;
    }

    uart_config_t uart_config = {
        .baud_rate = handle->baudrate,
        .data_bits = word_len,
        .parity = parity_cfg,
        .stop_bits = stop,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_param_config(handle->uart_num, &uart_config));
}

void furi_hal_serial_tx(FuriHalSerialHandle* handle, const uint8_t* buffer, size_t buffer_size) {
    furi_check(handle);
    furi_check(handle->is_init);

    uart_write_bytes(handle->uart_num, buffer, buffer_size);
}

void furi_hal_serial_tx_wait_complete(FuriHalSerialHandle* handle) {
    furi_check(handle);
    furi_check(handle->is_init);

    uart_wait_tx_done(handle->uart_num, portMAX_DELAY);
}

void furi_hal_serial_async_rx_start(
    FuriHalSerialHandle* handle,
    FuriHalSerialAsyncRxCallback callback,
    void* context,
    bool report_errors) {
    furi_check(handle);
    furi_check(handle->is_init);
    furi_check(!handle->async_rx_running);

    UNUSED(report_errors);

    handle->async_rx_cb = callback;
    handle->async_rx_ctx = context;
    handle->byte_valid = false;
    handle->async_rx_running = true;

    uart_flush_input(handle->uart_num);

    ReaderTaskArg* arg = malloc(sizeof(ReaderTaskArg));
    furi_check(arg);
    arg->handle = handle;

    BaseType_t task_created = xTaskCreate(
        serial_reader_task,
        "uart_reader",
        2048,
        arg,
        configMAX_PRIORITIES - 2,
        &handle->reader_task);

    if(task_created != pdPASS) {
        FURI_LOG_E(TAG, "Failed to create reader task for UART%zu", (size_t)(handle->uart_num));
        handle->async_rx_running = false;
        free(arg);
    }
}

void furi_hal_serial_async_rx_stop(FuriHalSerialHandle* handle) {
    furi_check(handle);
    furi_check(handle->async_rx_running);

    handle->async_rx_running = false;

    if(handle->reader_task) {
        vTaskDelete(handle->reader_task);
        handle->reader_task = NULL;
    }

    handle->async_rx_cb = NULL;
    handle->async_rx_ctx = NULL;
    handle->byte_valid = false;
}

bool furi_hal_serial_async_rx_available(FuriHalSerialHandle* handle) {
    furi_check(handle);

    size_t available = 0;
    uart_get_buffered_data_len(handle->uart_num, &available);
    return available > 0;
}

uint8_t furi_hal_serial_async_rx(FuriHalSerialHandle* handle) {
    furi_check(handle);
    furi_check(handle->byte_valid);

    handle->byte_valid = false;
    return handle->pending_byte;
}

void furi_hal_serial_enable_direction(
    FuriHalSerialHandle* handle,
    FuriHalSerialDirection direction) {
    UNUSED(handle);
    UNUSED(direction);
}

void furi_hal_serial_disable_direction(
    FuriHalSerialHandle* handle,
    FuriHalSerialDirection direction) {
    UNUSED(handle);
    UNUSED(direction);
}
