#include "gpio_usb_uart.h"
#include "../usb_uart_bridge.h"
#include <furi_hal.h>
#include <gui/elements.h>
#include <assets_icons.h>
#include <boards/board.h>

struct GpioUsbUart {
    View* view;
    GpioUsbUartCallback callback;
    void* context;
};

typedef struct {
    uint32_t baudrate;
    uint32_t tx_cnt;
    uint32_t rx_cnt;
    int tx_pin;
    int rx_pin;
    uint8_t usb_channel;
    bool tx_active;
    bool rx_active;
    UsbUartStatus status;
    bool usb_connected;
} GpioUsbUartModel;

static void gpio_usb_uart_draw_callback(Canvas* canvas, void* _model) {
    GpioUsbUartModel* model = _model;
    char temp_str[18];

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 9, "USB-UART");
    canvas_draw_line(canvas, 2, 12, 125, 12);

    canvas_set_font(canvas, FontSecondary);
    if(model->status != UsbUartStatusActive) {
        canvas_draw_str(canvas, 2, 27, "Unavailable:");
        canvas_draw_str(canvas, 2, 40, usb_uart_get_status_text(model->status));
        canvas_draw_str(canvas, 2, 60, "Back to exit");
        return;
    }

    snprintf(
        temp_str,
        sizeof(temp_str),
        "CDC%u %s",
        (unsigned)model->usb_channel,
        model->usb_connected ? "OPEN" : "WAIT");
    canvas_draw_str_aligned(canvas, 126, 9, AlignRight, AlignBottom, temp_str);
#if defined(BOARD_HAS_USB_UART_BRIDGE) && BOARD_HAS_USB_UART_BRIDGE
    snprintf(temp_str, sizeof(temp_str), "TX GPIO %d", model->tx_pin);
    canvas_draw_str(canvas, 3, 27, temp_str);
    snprintf(temp_str, sizeof(temp_str), "RX GPIO %d", model->rx_pin);
    canvas_draw_str(canvas, 3, 41, temp_str);
#else
    canvas_draw_str(canvas, 3, 27, "TX --");
    canvas_draw_str(canvas, 3, 41, "RX --");
#endif

    if(model->tx_cnt < 100000000) {
        if(model->tx_cnt < 1000) {
            snprintf(temp_str, sizeof(temp_str), "%lu B", (unsigned long)model->tx_cnt);
        } else {
            snprintf(temp_str, sizeof(temp_str), "%lu KB", (unsigned long)(model->tx_cnt / 1000));
        }
    } else {
        snprintf(temp_str, sizeof(temp_str), "%lu MB", (unsigned long)(model->tx_cnt / 1000000));
    }
    canvas_draw_str_aligned(canvas, 126, 27, AlignRight, AlignBottom, temp_str);
    if(model->rx_cnt < 100000000) {
        if(model->rx_cnt < 1000) {
            snprintf(temp_str, sizeof(temp_str), "%lu B", (unsigned long)model->rx_cnt);
        } else {
            snprintf(temp_str, sizeof(temp_str), "%lu KB", (unsigned long)(model->rx_cnt / 1000));
        }
    } else {
        snprintf(temp_str, sizeof(temp_str), "%lu MB", (unsigned long)(model->rx_cnt / 1000000));
    }
    canvas_draw_str_aligned(canvas, 126, 41, AlignRight, AlignBottom, temp_str);
    if(model->tx_active)
        canvas_draw_icon(canvas, 56, 14, &I_ArrowUpFilled_14x15);
    else
        canvas_draw_icon(canvas, 56, 14, &I_ArrowUpEmpty_14x15);

    if(model->rx_active)
        canvas_draw_icon_ex(canvas, 56, 28, &I_ArrowUpFilled_14x15, IconRotation180);
    else
        canvas_draw_icon_ex(canvas, 56, 28, &I_ArrowUpEmpty_14x15, IconRotation180);

    canvas_draw_line(canvas, 2, 47, 125, 47);
    elements_button_left(canvas, "Config");
    if(model->baudrate == 0)
        snprintf(temp_str, sizeof(temp_str), "Baud: Host");
    else
        snprintf(temp_str, sizeof(temp_str), "Baud: %lu", (unsigned long)model->baudrate);
    canvas_draw_str_aligned(canvas, 126, 62, AlignRight, AlignBottom, temp_str);
}

static bool gpio_usb_uart_input_callback(InputEvent* event, void* context) {
    furi_assert(context);
    GpioUsbUart* usb_uart = context;
    bool consumed = false;

    if(event->type == InputTypeShort) {
        if(event->key == InputKeyLeft) {
            consumed = true;
            furi_assert(usb_uart->callback);
            usb_uart->callback(GpioUsbUartEventConfig, usb_uart->context);
        }
    }

    return consumed;
}

GpioUsbUart* gpio_usb_uart_alloc(void) {
    GpioUsbUart* usb_uart = calloc(1, sizeof(GpioUsbUart));
    furi_check(usb_uart);

    usb_uart->view = view_alloc();
    furi_check(usb_uart->view);
    view_allocate_model(usb_uart->view, ViewModelTypeLocking, sizeof(GpioUsbUartModel));
    view_set_context(usb_uart->view, usb_uart);
    view_set_draw_callback(usb_uart->view, gpio_usb_uart_draw_callback);
    view_set_input_callback(usb_uart->view, gpio_usb_uart_input_callback);
    view_set_input_mode(usb_uart->view, ViewInputModeLeftRight);

    return usb_uart;
}

void gpio_usb_uart_free(GpioUsbUart* usb_uart) {
    furi_assert(usb_uart);
    view_free(usb_uart->view);
    free(usb_uart);
}

View* gpio_usb_uart_get_view(GpioUsbUart* usb_uart) {
    furi_assert(usb_uart);
    return usb_uart->view;
}

void gpio_usb_uart_set_callback(GpioUsbUart* usb_uart, GpioUsbUartCallback callback, void* context) {
    furi_assert(usb_uart);
    furi_assert(callback);

    with_view_model(
        usb_uart->view,
        GpioUsbUartModel * model,
        {
            UNUSED(model);
            usb_uart->callback = callback;
            usb_uart->context = context;
        },
        false);
}

void gpio_usb_uart_update_state(GpioUsbUart* instance, UsbUartConfig* cfg, UsbUartState* st) {
    furi_assert(instance);
    furi_assert(cfg);
    furi_assert(st);

    with_view_model(
        instance->view,
        GpioUsbUartModel * model,
        {
            model->baudrate = st->baudrate_cur;
            model->status = st->status;
            model->usb_connected = st->usb_connected;
#if defined(BOARD_HAS_USB_UART_BRIDGE) && BOARD_HAS_USB_UART_BRIDGE
            model->usb_channel = cfg->usb_channel;
            model->tx_pin = usb_uart_get_uart_tx_pin(cfg->uart_pin_pair);
            model->rx_pin = usb_uart_get_uart_rx_pin(cfg->uart_pin_pair);
#else
            model->usb_channel = cfg->usb_channel;
            model->tx_pin = 0;
            model->rx_pin = 0;
#endif
            model->tx_active = (model->tx_cnt != st->tx_cnt);
            model->rx_active = (model->rx_cnt != st->rx_cnt);
            model->tx_cnt = st->tx_cnt;
            model->rx_cnt = st->rx_cnt;
        },
        true);
}
