#include "pc_monitor.h"

#include <furi_hal_board_info.h>
#include <furi_hal_usb_tinyusb_composite.h>
#include <dialogs/dialogs.h>

// ESP32 port notes (details: PORT_NOTES.md)
//  - A stock Flipper always has its USB CDC port up and the CLI listens on it. Here the USB
//    port is the ESP32-S3's USB-Serial-JTAG (console/flasher) until a TinyUSB composite is
//    installed on demand; only that composite has the CDC port a PC back-end can open (it
//    identifies as a Flipper Zero, VID 0483 / PID 5740).
//  - The app takes the USB port the same way the USB-UART bridge does, in this order:
//        1. USB lease          (furi_hal_usb_lease_acquire: exclusive, restores the previous
//                               interface and resumes a preempted qFlipper session on release)
//        2. CLI console off    (cli_vcp_disable: the CLI would fight for CDC0)
//        3. CDC interface      (furi_hal_usb_set_config_for_owner)
//        4. TinyUSB composite  (furi_hal_usb_composite_install_for_owner)
//        5. light-sleep veto + CDC0 callbacks
//    and gives it back in the reverse order. Every step records what it owns, so a failed
//    release keeps what is still owned and is retried; nothing is dropped half way.
//  - On exit the composite is removed again, which also puts USB-Serial-JTAG (and flashing)
//    back without a reboot.

#define PC_MONITOR_CDC_IF 0
// A partly received packet that does not complete within this time is garbage (the PC back-end
// sends one packet a second): drop it so the next packet is framed correctly again.
#define PC_MONITOR_PARTIAL_PACKET_MS 250U
#define PC_MONITOR_RELEASE_TRIES 5
#define PC_MONITOR_RELEASE_RETRY_MS 100

static const char pc_monitor_usb_owner[] = "PC Monitor USB";

static void render_callback(Canvas* canvas, void* ctx) {
    furi_assert(ctx);
    PcMonitorApp* app = ctx;
    if(furi_mutex_acquire(app->mutex, FuriWaitForever) != FuriStatusOk) return;

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
    furi_mutex_release(app->mutex);
}

static void input_callback(InputEvent* input_event, void* ctx) {
    furi_assert(ctx);
    PcMonitorApp* app = ctx;
    // Only the clicks matter (Back leaves). A full queue drops them, never blocks the GUI.
    if(input_event->type == InputTypeShort || input_event->type == InputTypeLong) {
        furi_message_queue_put(app->event_queue, input_event, 0);
    }
}

// Registering callbacks (even empty ones) is what claims CDC0 for this app; the main loop polls
// the received bytes itself every 100 ms (the back-end sends a packet a second).
static CdcCallbacks pc_monitor_cdc_callbacks = {
    .tx_ep_callback = NULL,
    .rx_ep_callback = NULL,
    .state_callback = NULL,
    .ctrl_line_callback = NULL,
    .config_callback = NULL,
};

// Give back, in reverse order, whatever is still owned. Returns true only when nothing is
// owned any more. Safe to call again after a failure: it continues where it stopped.
static bool pc_monitor_usb_release(PcMonitorApp* app) {
    if(app->cdc_claimed) {
        // Blocks until a callback that is running has returned.
        furi_hal_cdc_set_callbacks(PC_MONITOR_CDC_IF, NULL, NULL);
        app->cdc_claimed = false;
    }

    // A lease that preempted a composite may find its own composite installed.
    if(app->usb_lease && furi_hal_usb_composite_is_installed()) {
        app->usb_composite = true;
    }
    if(app->usb_composite) {
        if(furi_hal_usb_composite_uninstall_for_owner(pc_monitor_usb_owner)) {
            app->usb_composite = false;
        } else {
            FURI_LOG_E(TAG, "Could not uninstall the USB composite; keeping the USB lease");
        }
    }

    if(app->usb_lease && !app->usb_composite) {
        if(furi_hal_usb_lease_release(pc_monitor_usb_owner, app->usb_previous)) {
            app->usb_lease = false;
        } else if(!furi_hal_usb_is_lease_owner(pc_monitor_usb_owner)) {
            // The lease is gone (only resuming the preempted owner failed): nothing to keep.
            app->usb_lease = false;
        } else {
            FURI_LOG_E(TAG, "Could not release the USB lease; USB stays reserved");
        }
    }

    if(!app->usb_lease) {
        // Same rule as the USB-UART bridge: the CLI console is only switched back on when
        // no composite is installed (a resumed qFlipper session owns the port then).
        if(app->cli_disabled && !furi_hal_usb_composite_is_installed()) {
            cli_vcp_enable(app->cli_vcp);
        }
        app->cli_disabled = false;
        if(app->cli_record_open) {
            furi_record_close(RECORD_CLI_VCP);
            app->cli_record_open = false;
            app->cli_vcp = NULL;
        }
    }

    // Last: while a composite or the lease is still ours, USB may need the clock.
    if(!app->usb_lease && !app->usb_composite && app->sleep_inhibited) {
        furi_hal_power_light_sleep_inhibit_exit();
        app->sleep_inhibited = false;
    }

    return !app->usb_lease && !app->usb_composite && !app->cdc_claimed && !app->cli_record_open &&
           !app->sleep_inhibited;
}

// Take the USB serial port. Sets app->usb_state to Busy / Error and returns false if it
// cannot be had; nothing stays half-owned in that case.
static bool pc_monitor_usb_acquire(PcMonitorApp* app) {
    if(!furi_hal_usb_lease_acquire(pc_monitor_usb_owner, &app->usb_previous)) {
        // Locked (qFlipper lock menu), leased to another feature, or a composite that
        // cannot be preempted (USB storage).
        app->usb_state = UsbStateBusy;
        return false;
    }
    app->usb_lease = true;

    app->cli_vcp = furi_record_open(RECORD_CLI_VCP);
    if(!app->cli_vcp) goto fail;
    app->cli_record_open = true;
    app->cli_disabled = cli_vcp_disable(app->cli_vcp);

    if(!furi_hal_usb_set_config_for_owner(pc_monitor_usb_owner, &usb_cdc_single, NULL)) goto fail;

    if(!furi_hal_usb_composite_install_for_owner(pc_monitor_usb_owner, 0, 0, NULL, NULL)) {
        // It may have been installed even though the call failed: then it is ours to remove.
        app->usb_composite = furi_hal_usb_composite_is_installed();
        goto fail;
    }
    app->usb_composite = true;
    furi_hal_usb_reinit_for_owner(pc_monitor_usb_owner);

    // The composite keeps the SoC awake by itself; this is the explicit veto that is
    // returned in pc_monitor_usb_release().
    furi_hal_power_light_sleep_inhibit_enter();
    app->sleep_inhibited = true;

    furi_hal_cdc_set_callbacks(PC_MONITOR_CDC_IF, &pc_monitor_cdc_callbacks, app);
    app->cdc_claimed = true;
    return true;

fail:
    app->usb_state = UsbStateError;
    (void)pc_monitor_usb_release(app);
    return false;
}

// Collect the CDC bytes into whole 16-byte packets. A USB read may hold part of a packet or
// several of them; the newest complete packet wins and is returned in `packet` (the caller
// stores it under the app mutex: the GUI thread reads it).
static bool pc_monitor_poll_rx(PcMonitorApp* app, DataStruct* packet) {
    if(!app->cdc_claimed) return false;

    bool updated = false;
    for(;;) {
        const uint16_t space = (uint16_t)(sizeof(app->rx_buf) - app->rx_len);
        const int32_t received =
            furi_hal_cdc_receive(PC_MONITOR_CDC_IF, app->rx_buf + app->rx_len, space);
        if(received <= 0) break;
        if(app->rx_len == 0) app->rx_partial_tick = furi_get_tick();
        app->rx_len = (uint8_t)(app->rx_len + received);
        if(app->rx_len == sizeof(DataStruct)) {
            memcpy(packet, app->rx_buf, sizeof(DataStruct));
            app->rx_len = 0;
            updated = true;
        }
    }

    // A packet that stays incomplete would shift every later one: start over.
    if(app->rx_len > 0 &&
       (furi_get_tick() - app->rx_partial_tick) > furi_ms_to_ticks(PC_MONITOR_PARTIAL_PACKET_MS)) {
        app->rx_len = 0;
    }
    return updated;
}

static PcMonitorApp* pc_monitor_alloc(void) {
    PcMonitorApp* app = calloc(1, sizeof(PcMonitorApp));
    furi_check(app != NULL);
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->view_port = view_port_alloc();
    app->event_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->notification = furi_record_open(RECORD_NOTIFICATION);
    app->gui = furi_record_open(RECORD_GUI);

    app->usb_state = UsbStateWaiting;
    pc_monitor_usb_acquire(app);

    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);
    view_port_draw_callback_set(app->view_port, render_callback, app);
    view_port_input_callback_set(app->view_port, input_callback, app);

    return app;
}

static void pc_monitor_free(PcMonitorApp* app) {
    // The view port first: the render callback must not run while USB is being torn down.
    gui_remove_view_port(app->gui, app->view_port);

    // Retry a release that failed (a composite that does not uninstall at once, a lease whose
    // preempted owner is slow to resume). The FAP is unloaded after this returns, so what is
    // still owned then could never be released.
    for(uint8_t attempt = 0; attempt < PC_MONITOR_RELEASE_TRIES; attempt++) {
        if(pc_monitor_usb_release(app)) break;
        furi_delay_ms(PC_MONITOR_RELEASE_RETRY_MS);
    }
    if(app->usb_lease || app->usb_composite) {
        FURI_LOG_E(TAG, "USB could not be released; it stays reserved until reboot");
    }

    view_port_free(app->view_port);
    furi_message_queue_free(app->event_queue);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);
    furi_mutex_free(app->mutex);
    free(app);
}

// Supported boards only (LilyGo T-Embed, Paradoxt, ThorChimera, Thormini).
static bool pc_monitor_board_supported(void) {
    if(furi_hal_board_supports_ported_apps()) return true;
    DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
    DialogMessage* message = dialog_message_alloc();
    dialog_message_set_header(message, "Board not supported", 64, 2, AlignCenter, AlignTop);
    dialog_message_set_text(
        message,
        "Runs on T-Embed, Paradoxt,\nThorChimera and Thormini\nonly.",
        64,
        34,
        AlignCenter,
        AlignCenter);
    dialog_message_set_buttons(message, NULL, "OK", NULL);
    dialog_message_show(dialogs, message);
    dialog_message_free(message);
    furi_record_close(RECORD_DIALOGS);
    return false;
}

int32_t pc_monitor_app(void* p) {
    UNUSED(p);
    if(!pc_monitor_board_supported()) return 0;

    PcMonitorApp* app = pc_monitor_alloc();

    FURI_LOG_D(TAG, "USB Monitor started");

    // Main loop
    InputEvent event;
    while(true) {
        // 100 ms: also the poll interval of the USB port
        if(furi_message_queue_get(app->event_queue, &event, 100) == FuriStatusOk) {
            if(event.type == InputTypeShort && event.key == InputKeyBack) break;
        }

        DataStruct packet;
        const bool updated = pc_monitor_poll_rx(app, &packet);

        furi_check(furi_mutex_acquire(app->mutex, FuriWaitForever) == FuriStatusOk);
        const UsbState state_before = app->usb_state;
        if(updated) {
            app->data = packet;
            app->usb_state = UsbStateActive;
            app->last_packet = furi_get_tick();
        } else if(
            app->usb_state == UsbStateActive &&
            (furi_get_tick() - app->last_packet > furi_ms_to_ticks(5000U))) {
            app->usb_state = UsbStateLost;
        }
        const bool changed = updated || app->usb_state != state_before;
        furi_check(furi_mutex_release(app->mutex) == FuriStatusOk);

        if(updated) {
            notification_message(app->notification, &sequence_display_backlight_on);
            notification_message(app->notification, &sequence_blink_blue_10);
        }
        // Redraw only when something changed: a lost connection must not repaint 10 times a second.
        if(changed) {
            view_port_update(app->view_port);
        }
    }

    pc_monitor_free(app);

    return 0;
}
