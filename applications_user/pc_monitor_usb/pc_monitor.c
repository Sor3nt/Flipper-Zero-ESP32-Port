#include "pc_monitor.h"

#include <furi_hal_usb_tinyusb_composite.h>

// ESP32 port notes
//  - A stock Flipper always has its USB CDC port up and the CLI listens on it. Here the USB
//    port is the ESP32-S3's USB-Serial-JTAG (console/flasher) until a TinyUSB composite is
//    installed on demand; only that composite has the CDC port a PC back-end can open (it
//    identifies as a Flipper Zero, VID 0483 / PID 5740). The app therefore installs the
//    composite itself and removes it again on exit, which also puts USB-Serial-JTAG (and
//    flashing) back without a reboot.
//  - There is a single CDC port. If qFlipper (lock menu), the CLI or another app owns it, the
//    app says so instead of hijacking the port.
//  - Both of those happen on the ESP32 only; the data format and screens are unchanged.

#define PC_MONITOR_CDC_IF 0

static void render_callback(Canvas* canvas, void* ctx) {
    furi_assert(ctx);
    PcMonitorApp* app = ctx;

    switch(app->usb_state) {
    case UsbStateWaiting:
        draw_connect_view(canvas);
        break;

    case UsbStateActive:
        draw_bars_view(canvas, app);
        break;

    default:
        draw_status_view(canvas, app);
        break;
    }
}

static void input_callback(InputEvent* input_event, void* ctx) {
    furi_assert(ctx);
    PcMonitorApp* app = ctx;

    // Paradoxt reports Short after Release; restore the stock event order and
    // infer Long for OK/Back (see components/input/input_canonical.h).
    uint32_t now_ms =
        (uint32_t)((uint64_t)furi_get_tick() * 1000ULL / furi_kernel_get_tick_frequency());
    InputEvent events[INPUT_CANONICAL_MAX_OUT];
    size_t count = input_canonical_feed(&app->canon, input_event, now_ms, events);
    for(size_t i = 0; i < count; i++) {
        furi_message_queue_put(app->event_queue, &events[i], 50);
    }
}

static void cdc_rx_callback(void* context) {
    furi_assert(context);
    PcMonitorApp* app = context;
    app->new_data_received = true;
}

static CdcCallbacks pc_monitor_cdc_callbacks = {
    .tx_ep_callback = NULL,
    .rx_ep_callback = cdc_rx_callback,
    .state_callback = NULL,
    .ctrl_line_callback = NULL,
    .config_callback = NULL,
};

// Take the USB serial port. Sets app->usb_state to Busy / Error and returns false if it
// cannot be had; nothing is left half-claimed in that case.
static bool pc_monitor_usb_acquire(PcMonitorApp* app) {
    if(furi_hal_usb_is_locked()) {
        app->usb_state = UsbStateBusy;
        return false;
    }

    // HID (BadUSB, Click Recorder) or U2F is using the USB port in a different mode.
    FuriHalUsbInterface* current = furi_hal_usb_get_config();
    if(current && current != &usb_cdc_single && current != &usb_cdc_dual) {
        app->usb_state = UsbStateBusy;
        return false;
    }

    // The single CDC port already has an owner (qFlipper bridge, CLI shell, USB-UART bridge).
    if(furi_hal_usb_composite_is_installed() && furi_hal_cdc_is_owned(PC_MONITOR_CDC_IF)) {
        app->usb_state = UsbStateBusy;
        return false;
    }

    const bool composite_was_installed = furi_hal_usb_composite_is_installed();
    if(!furi_hal_usb_composite_install(0, 0, NULL, NULL)) {
        app->usb_state = UsbStateError;
        return false;
    }
    app->composite_installed_by_us = !composite_was_installed;

    furi_hal_cdc_set_callbacks(PC_MONITOR_CDC_IF, &pc_monitor_cdc_callbacks, app);
    app->cdc_claimed = true;

    // Light sleep would take the USB peripheral away mid-session.
    furi_hal_power_light_sleep_inhibit_enter();
    app->sleep_inhibited = true;
    return true;
}

static void pc_monitor_usb_release(PcMonitorApp* app) {
    // A stock firmware would restore the CLI's callbacks here; they are not ours to restore.
    if(app->cdc_claimed) {
        furi_hal_cdc_set_callbacks(PC_MONITOR_CDC_IF, NULL, NULL);
        app->cdc_claimed = false;
    }
    if(app->composite_installed_by_us) {
        furi_hal_usb_composite_uninstall();
        app->composite_installed_by_us = false;
    }
    if(app->sleep_inhibited) {
        furi_hal_power_light_sleep_inhibit_exit();
        app->sleep_inhibited = false;
    }
}

static PcMonitorApp* pc_monitor_alloc() {
    PcMonitorApp* app = calloc(1, sizeof(PcMonitorApp));
    furi_check(app != NULL);
    app->view_port = view_port_alloc();
    app->event_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->notification = furi_record_open(RECORD_NOTIFICATION);
    app->gui = furi_record_open(RECORD_GUI);
    input_canonical_reset(&app->canon);

    app->usb_state = UsbStateWaiting;
    pc_monitor_usb_acquire(app);

    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);
    view_port_draw_callback_set(app->view_port, render_callback, app);
    view_port_input_callback_set(app->view_port, input_callback, app);

    return app;
}

static void pc_monitor_free(PcMonitorApp* app) {
    pc_monitor_usb_release(app);

    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_message_queue_free(app->event_queue);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);
    free(app);
}

int32_t pc_monitor_app(void* p) {
    UNUSED(p);

    PcMonitorApp* app = pc_monitor_alloc();

    FURI_LOG_D(TAG, "USB Monitor started");

    // Main loop
    InputEvent event;
    while(true) {
        if(furi_message_queue_get(app->event_queue, &event, 100) == FuriStatusOk) { // 100ms timeout for checking connection state
            if(event.type == InputTypeShort && event.key == InputKeyBack) break;
        }

        if(app->cdc_claimed && app->new_data_received) {
            app->new_data_received = false;
            if(furi_hal_cdc_receive(PC_MONITOR_CDC_IF, (uint8_t*)&app->data, sizeof(DataStruct)) ==
               sizeof(DataStruct)) {
                app->usb_state = UsbStateActive;
                app->last_packet = furi_get_tick();

                notification_message(app->notification, &sequence_display_backlight_on);
                notification_message(app->notification, &sequence_blink_blue_10);
                view_port_update(app->view_port);
            }
        }

        if(app->usb_state == UsbStateActive &&
           (furi_get_tick() - app->last_packet > furi_ms_to_ticks(5000U))) {
            app->usb_state = UsbStateLost;
            view_port_update(app->view_port);
        }
    }

    pc_monitor_free(app);

    return 0;
}
