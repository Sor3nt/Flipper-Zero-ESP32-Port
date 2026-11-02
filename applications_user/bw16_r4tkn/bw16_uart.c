#include "bw16_app_i.h"
#include "bw16_uart.h"

/* ESP32-S3 port: the BW16 is wired to the shared expansion USART (Flipper pin 13 = TX,
 * pin 14 = RX), exposed to FAPs through furi_hal_serial as FuriHalSerialIdUsart. This is
 * the same channel the WiFi Marauder companion uses, so the two cannot run at once. */
#define BW16_UART_CH  FuriHalSerialIdUsart
#define BW16_BAUDRATE (115200)
/* RX stream between the serial callback and the worker. At 115200 baud ~11.5 KB/s arrive,
 * so a worker busy parsing/rendering for a few hundred ms (mutex held by the GUI) would
 * overflow the old 1 KiB buffer and silently drop bytes, truncating list-station lines.
 * It is larger than the worker's per-wake chunk (BW16_UART_RX_BUF_SIZE) on purpose. */
#define BW16_UART_STREAM_SIZE (4096)

struct Bw16Uart {
    Bw16App* app;
    FuriThread* rx_thread;
    FuriStreamBuffer* rx_stream;
    FuriHalSerialHandle* serial_handle;
    uint8_t rx_buf[BW16_UART_RX_BUF_SIZE + 1];
    volatile uint32_t rx_dropped; /* bytes that did not fit in rx_stream */
    void (*handle_rx_data_cb)(uint8_t* buf, size_t len, void* context);
};

typedef enum {
    WorkerEvtStop = (1 << 0),
    WorkerEvtRxDone = (1 << 1),
} WorkerEvtFlags;

#define WORKER_ALL_RX_EVENTS (WorkerEvtStop | WorkerEvtRxDone)

void bw16_uart_set_handle_rx_data_cb(
    Bw16Uart* uart,
    void (*handle_rx_data_cb)(uint8_t* buf, size_t len, void* context)) {
    furi_assert(uart);
    uart->handle_rx_data_cb = handle_rx_data_cb;
}

static void
    bw16_uart_on_irq_cb(FuriHalSerialHandle* handle, FuriHalSerialRxEvent event, void* context) {
    Bw16Uart* uart = (Bw16Uart*)context;

    if(event == FuriHalSerialRxEventData) {
        uint8_t data = furi_hal_serial_async_rx(handle);
        if(furi_stream_buffer_send(uart->rx_stream, &data, 1, 0) != 1) {
            uart->rx_dropped++;
        }
        furi_thread_flags_set(furi_thread_get_id(uart->rx_thread), WorkerEvtRxDone);
    }
}

static int32_t bw16_uart_worker(void* context) {
    Bw16Uart* uart = (Bw16Uart*)context;

    while(1) {
        uint32_t events =
            furi_thread_flags_wait(WORKER_ALL_RX_EVENTS, FuriFlagWaitAny, FuriWaitForever);
        furi_check((events & FuriFlagError) == 0);
        if(events & WorkerEvtStop) break;
        if(events & WorkerEvtRxDone) {
            uint32_t dropped = uart->rx_dropped;
            if(dropped) {
                uart->rx_dropped -= dropped;
                FURI_LOG_W("Bw16Uart", "RX overflow: %lu byte(s) dropped", (unsigned long)dropped);
            }
            /* Thread flags coalesce, so one wake can represent many bytes.
             * Drain the entire stream here; otherwise a burst larger than one
             * rx_buf could leave its tail waiting forever for another byte. */
            size_t len;
            while((len = furi_stream_buffer_receive(
                       uart->rx_stream, uart->rx_buf, BW16_UART_RX_BUF_SIZE, 0)) > 0) {
                if(uart->handle_rx_data_cb) {
                    uart->handle_rx_data_cb(uart->rx_buf, len, uart->app);
                }
            }
        }
    }

    return 0;
}

void bw16_uart_tx(Bw16Uart* uart, const uint8_t* data, size_t len) {
    furi_assert(uart);
    furi_hal_serial_tx(uart->serial_handle, data, len);
}

void bw16_uart_send_line(Bw16Uart* uart, const char* line) {
    furi_assert(uart);
    furi_assert(line);
    bw16_uart_tx(uart, (const uint8_t*)line, strlen(line));
    bw16_uart_tx(uart, (const uint8_t*)"\n", 1);
}

Bw16Uart* bw16_uart_init(Bw16App* app) {
    /* Acquire the shared USART FIRST. It returns NULL if another app (WiFi Marauder,
     * USB-UART bridge, ESP-Flasher) holds it, or if the shared pads are busy. Return NULL
     * so the caller can show a message instead of crashing on furi_check. Nothing is
     * allocated or started until the handle is ours. */
    FuriHalSerialHandle* handle = furi_hal_serial_control_acquire(BW16_UART_CH);
    if(!handle) return NULL;

    Bw16Uart* uart = malloc(sizeof(Bw16Uart));
    if(!uart) {
        furi_hal_serial_control_release(handle);
        return NULL;
    }
    uart->app = app;
    uart->handle_rx_data_cb = NULL;
    uart->serial_handle = handle;
    uart->rx_stream = furi_stream_buffer_alloc(BW16_UART_STREAM_SIZE, 1);

    /* The worker runs furi_string_cat_printf()/snprintf() (printf-family needs a healthy
     * margin on Xtensa), takes the mutex and parses lines, so give it room. The stack is
     * allocated from PSRAM, so 4 KiB is cheap.
     * furi_thread_alloc_ex() returns NULL when internal RAM for the TCB or the stack is
     * exhausted; the caller already shows the "UART busy" dialog for a NULL return. The
     * handle was only acquired, never initialised, so releasing it is enough to back out. */
    uart->rx_thread =
        furi_thread_alloc_ex("Bw16UartRxThread", 4096, bw16_uart_worker, uart);
    if(!uart->rx_thread) {
        furi_stream_buffer_free(uart->rx_stream);
        free(uart);
        furi_hal_serial_control_release(handle);
        return NULL;
    }
    furi_thread_start(uart->rx_thread);

    furi_hal_serial_init(handle, BW16_BAUDRATE);
    furi_hal_serial_async_rx_start(handle, bw16_uart_on_irq_cb, uart, false);

    return uart;
}

void bw16_uart_free(Bw16Uart* uart) {
    furi_assert(uart);

    furi_hal_serial_async_rx_stop(uart->serial_handle);
    furi_hal_serial_deinit(uart->serial_handle);
    furi_hal_serial_control_release(uart->serial_handle);

    furi_thread_flags_set(furi_thread_get_id(uart->rx_thread), WorkerEvtStop);
    furi_thread_join(uart->rx_thread);
    furi_thread_free(uart->rx_thread);

    furi_stream_buffer_free(uart->rx_stream);

    free(uart);
}
