#include "tpms_bridge.h"
#include "tpms_lf.h"
#include "tpms_view.h"

#include <furi.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <input/input.h>
#include <furi_hal_board_info.h> // PORT: board gate + input layout (dpad vs encoder)
#include <dialogs/dialogs.h> // PORT: "board not supported" dialog

#define TAG "TpmsBridge"

// PORT: encoder support. This app is a custom ViewPort, so the firmware's automatic encoder remap
// does not apply. The upstream key map needs Long Up/Down to pick a list row, and a wheel never
// emits that, so on encoder boards (T-Embed, ThorChimera) tpms_bridge_handle_encoder() uses its own
// map: wheel = pick sensor, OK held + turn (Left/Right) = band/modulation, side key held = clear
// list. Resolved once in tpms_bridge_app(). D-pad / button boards keep the upstream map.
static bool s_is_encoder = false;

#define TPMS_INPUT_QUEUE_SIZE 8

/** Pause before retrying to claim the radio, ms. */
#define TPMS_RADIO_RETRY_MS 1000

void tpms_bridge_report_frame(TpmsBridgeApp* app, const TpmsFrame* frame, float rssi_dbm) {
    furi_check(app);

    furi_mutex_acquire(app->state_mutex, FuriWaitForever);
    tpms_store_update(&app->store, frame, (int16_t)(rssi_dbm * 10.0f), furi_get_tick());
    tpms_view_follow_selection(app);
    furi_mutex_release(app->state_mutex);

    if(app->view_port) view_port_update(app->view_port);
}

void tpms_bridge_tune_radio(TpmsBridgeApp* app, TpmsSession* session) {
    furi_check(app);

    uint8_t slot;
    if(app->config == TpmsConfigScan) {
        const uint32_t now = furi_get_tick();
        if(app->scan_tick == 0 || now - app->scan_tick > furi_ms_to_ticks(TPMS_SCAN_PERIOD_MS)) {
            app->scan_tick = now;
            app->scan_step = (uint8_t)((app->scan_step + 1) % TPMS_SLOT_COUNT);
        }
        slot = app->scan_step;
    } else {
        slot = (uint8_t)(app->config % TPMS_SLOT_COUNT);
    }

    if(tpms_session_retune(session, tpms_slot_frequency(slot), tpms_slot_modulation(slot))) {
        app->active_slot = slot;
    }
}

static void tpms_bridge_draw_callback(Canvas* canvas, void* context) {
    TpmsBridgeApp* app = context;
    furi_mutex_acquire(app->state_mutex, FuriWaitForever);
    tpms_view_draw(canvas, app);
    furi_mutex_release(app->state_mutex);
}

static void tpms_bridge_input_callback(InputEvent* event, void* context) {
    TpmsBridgeApp* app = context;
    furi_message_queue_put(app->input_queue, event, FuriWaitForever);
}

/** Lets the app be closed from the outside: `loader close`, `ufbt launch`.
 * Without this the system can only ask the user to press Back. */
static bool tpms_bridge_signal_callback(uint32_t signal, void* arg, void* context) {
    UNUSED(arg);
    TpmsBridgeApp* app = context;

    if(signal != FuriSignalExit) return false;

    /* From the list Back closes the app, from the detail screen it only
     * goes back. Switch to the list first so that the signal always
     * reaches its goal. */
    furi_mutex_acquire(app->state_mutex, FuriWaitForever);
    app->screen = TpmsScreenList;
    furi_mutex_release(app->state_mutex);

    const InputEvent event = {.type = InputTypeShort, .key = InputKeyBack};
    furi_message_queue_put(app->input_queue, &event, 0);
    return true;
}

static void
    tpms_bridge_local_frame_callback(const TpmsFrame* frame, float rssi_dbm, void* context) {
    tpms_bridge_report_frame(context, frame, rssi_dbm);
}

/** Local reception: runs all the time unless the USB session has taken
 * the radio. That is what makes the app useful in a car, with no
 * computer attached. */
static int32_t tpms_bridge_local_rx_thread(void* context) {
    TpmsBridgeApp* app = context;

    if(furi_mutex_acquire(app->radio_mutex, 0) != FuriStatusOk) {
        FURI_LOG_W(TAG, "radio busy, local rx cancelled");
        app->local_rx = false;
        view_port_update(app->view_port);
        return 0;
    }

    TpmsSession* session = tpms_session_alloc();
    tpms_session_set_frame_callback(session, tpms_bridge_local_frame_callback, app);

    const uint8_t slot =
        app->config == TpmsConfigScan ? app->scan_step : (uint8_t)(app->config % TPMS_SLOT_COUNT);

    if(tpms_session_start(session, tpms_slot_frequency(slot), tpms_slot_modulation(slot))) {
        app->active_slot = slot;
        uint32_t last_wake = 0;

        while(app->local_rx && !app->stop_requested && !app->radio_yield_requested) {
            const bool wake_due =
                app->auto_wake && (last_wake == 0 ||
                                   furi_get_tick() - last_wake > furi_ms_to_ticks(TPMS_LF_PERIOD_MS));

            if(wake_due || app->wake_requested) {
                app->wake_requested = false;
                last_wake = furi_get_tick();
                tpms_session_wake_pulse(session, TPMS_LF_PULSE_MS);
            }

            tpms_bridge_tune_radio(app, session);
            tpms_session_pump(session, 100);
        }
        tpms_session_stop(session);
    } else {
        FURI_LOG_E(TAG, "cannot start local rx");
    }

    tpms_session_free(session);
    furi_mutex_release(app->radio_mutex);

    app->local_rx = false;
    view_port_update(app->view_port);
    return 0;
}

static void tpms_bridge_stop_local_rx(TpmsBridgeApp* app) {
    app->local_rx = false;
    if(!app->local_thread) return;

    furi_thread_join(app->local_thread);
    furi_thread_free(app->local_thread);
    app->local_thread = NULL;
}

/** Keeps local reception running while the radio is free, and gets it out
 * of the way once a USB session comes for the radio. */
static void tpms_bridge_reconcile_radio(TpmsBridgeApp* app) {
    if(app->local_thread && !app->local_rx) {
        furi_thread_join(app->local_thread);
        furi_thread_free(app->local_thread);
        app->local_thread = NULL;
    }

    if(app->local_thread) return;
    if(app->stop_requested || app->radio_yield_requested || app->usb_streaming) return;

    /* Back off between attempts: if the radio refuses to start, do not
     * spin up the thread over and over. */
    const uint32_t now = furi_get_tick();
    if(app->radio_retry_tick != 0 &&
       now - app->radio_retry_tick < furi_ms_to_ticks(TPMS_RADIO_RETRY_MS)) {
        return;
    }
    app->radio_retry_tick = now;

    app->local_rx = true;
    app->local_thread = furi_thread_alloc_ex("TpmsLocalRx", 2048, tpms_bridge_local_rx_thread, app);
    furi_thread_start(app->local_thread);
}

/** Step through the radio configurations.
 *
 * The band and the modulation are one list rather than two settings: the
 * radio holds one of each at a time, and one ring of five entries is less
 * to remember than two separate cycles.
 */
static void tpms_bridge_step_config(TpmsBridgeApp* app, int8_t delta) {
    app->config = (uint8_t)((app->config + TpmsConfigCount + delta) % TpmsConfigCount);
    app->scan_tick = 0;
}

static void tpms_bridge_select(TpmsBridgeApp* app, int8_t delta) {
    if(delta < 0) {
        if(app->selected > 0) app->selected--;
    } else if(app->store.count > 0 && app->selected + 1 < app->store.count) {
        app->selected++;
    }
    tpms_view_follow_selection(app);
}

static void tpms_bridge_wake_sensor(TpmsBridgeApp* app) {
#if TPMS_LF_WAKE_SUPPORTED
    /* If reception is running the pulse must not stop the receiver: the
     * sensor answers right away. Decoding is driven by the session owner,
     * so all we do here is raise a flag. */
    if(app->local_thread || app->usb_streaming) {
        app->wake_requested = true;
        return;
    }

    tpms_lf_wake(TPMS_LF_PULSE_MS);
#else
    /* PORT: no controllable 125 kHz coil on these boards (see tpms_lf.h). */
    UNUSED(app);
#endif
}

/* Clear the list: handy when moving from one car to another and the table
 * still holds someone else's sensors. Caller holds state_mutex. */
static void tpms_bridge_clear_list(TpmsBridgeApp* app) {
    tpms_store_reset(&app->store);
    app->selected = 0;
    app->scroll = 0;
    app->screen = TpmsScreenList;
}

/** PORT: encoder key map (T-Embed, ThorChimera). Returns true when the event was handled here;
 * false hands it to the upstream handler (OK short = info / back to list, Back short = back/exit).
 *
 *   list:   wheel (Up/Down)             -> pick sensor row  (upstream: Long Up/Down, which a
 *                                                             wheel cannot emit)
 *           OK held + turn (Left/Right) -> step band/modulation (upstream: Up/Down)
 *   detail: wheel (Up/Down)             -> previous / next sensor (same as upstream)
 *           OK held + turn              -> ignored
 *   both:   side key (Back) held        -> clear the list (upstream: OK Long)
 *           OK held                     -> ignored: on the encoder it is also the start of
 *                                          "OK held + turn", and turning later than 500 ms would
 *                                          have wiped the list.
 */
static bool tpms_bridge_handle_encoder(TpmsBridgeApp* app, const InputEvent* event) {
    if(event->type == InputTypeLong) {
        if(event->key == InputKeyOk) return true;
        if(event->key == InputKeyBack) {
            furi_mutex_acquire(app->state_mutex, FuriWaitForever);
            tpms_bridge_clear_list(app);
            furi_mutex_release(app->state_mutex);
            return true;
        }
    }

    if(event->key != InputKeyUp && event->key != InputKeyDown && event->key != InputKeyLeft &&
       event->key != InputKeyRight) {
        return false;
    }
    /* Direction keys on the encoder only ever come as Press/Short/Release: swallow the rest. */
    if(event->type != InputTypeShort) return true;

    furi_mutex_acquire(app->state_mutex, FuriWaitForever);
    const bool vertical = event->key == InputKeyUp || event->key == InputKeyDown;
    const int8_t delta = (event->key == InputKeyUp || event->key == InputKeyLeft) ? -1 : 1;
    if(vertical) {
        tpms_bridge_select(app, delta);
    } else if(app->screen == TpmsScreenList) {
        tpms_bridge_step_config(app, delta);
    }
    furi_mutex_release(app->state_mutex);
    return true;
}

static void tpms_bridge_handle_input(TpmsBridgeApp* app, const InputEvent* event, bool* running) {
    if(s_is_encoder && tpms_bridge_handle_encoder(app, event)) return;
    /* The wake pulse touches the hardware and may take almost a second —
     * do not hold the state lock across it. */
    if(event->key == InputKeyRight && event->type == InputTypeShort) {
        tpms_bridge_wake_sensor(app);
        return;
    }

    furi_mutex_acquire(app->state_mutex, FuriWaitForever);

    if(event->type == InputTypeLong) {
        switch(event->key) {
        case InputKeyOk:
            tpms_bridge_clear_list(app);
            break;

        case InputKeyUp:
        case InputKeyDown:
            /* Up and Down step the radio configuration, so picking a row
             * out of the list moves to holding them. */
            tpms_bridge_select(app, event->key == InputKeyUp ? -1 : 1);
            break;

        default:
            break;
        }

        furi_mutex_release(app->state_mutex);
        return;
    }

    if(event->type != InputTypeShort) {
        furi_mutex_release(app->state_mutex);
        return;
    }

    switch(event->key) {
    case InputKeyBack:
        if(app->screen == TpmsScreenDetail) {
            app->screen = TpmsScreenList;
        } else {
            *running = false;
        }
        break;

    case InputKeyOk:
        if(app->screen == TpmsScreenList) {
            if(app->store.count > 0) app->screen = TpmsScreenDetail;
        } else {
            app->screen = TpmsScreenList;
        }
        break;

    case InputKeyLeft:
        if(app->screen == TpmsScreenDetail) {
            app->screen = TpmsScreenList;
        } else {
#if TPMS_LF_WAKE_SUPPORTED
            app->auto_wake = !app->auto_wake;
#endif
        }
        break;

    case InputKeyUp:
    case InputKeyDown: {
        const int8_t delta = event->key == InputKeyUp ? -1 : 1;
        /* On the list these step the band and the modulation; on the
         * detail screen they walk from one sensor to the next. */
        if(app->screen == TpmsScreenDetail) {
            tpms_bridge_select(app, delta);
        } else {
            tpms_bridge_step_config(app, delta);
        }
        break;
    }

    default:
        break;
    }

    furi_mutex_release(app->state_mutex);
}

static TpmsBridgeApp* tpms_bridge_app_alloc(void) {
    TpmsBridgeApp* app = malloc(sizeof(TpmsBridgeApp));
    memset(app, 0, sizeof(TpmsBridgeApp));

    app->state_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->radio_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->input_queue = furi_message_queue_alloc(TPMS_INPUT_QUEUE_SIZE, sizeof(InputEvent));

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, tpms_bridge_draw_callback, app);
    view_port_input_callback_set(app->view_port, tpms_bridge_input_callback, app);

    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    furi_thread_set_signal_callback(furi_thread_get_current(), tpms_bridge_signal_callback, app);

    app->cli_registry = furi_record_open(RECORD_CLI);
    /* Set the stack explicitly: the command formats strings and holds a
     * FuriString, and the default CLI command stack is rather small. */
    cli_registry_add_command_ex(
        app->cli_registry,
        TPMS_CLI_COMMAND_NAME,
        CliCommandFlagParallelSafe,
        tpms_cli_command,
        app,
        TPMS_CLI_STACK_SIZE);

    return app;
}

static void tpms_bridge_app_free(TpmsBridgeApp* app) {
    cli_registry_delete_command(app->cli_registry, TPMS_CLI_COMMAND_NAME);
    furi_record_close(RECORD_CLI);

    gui_remove_view_port(app->gui, app->view_port);
    furi_record_close(RECORD_GUI);
    view_port_free(app->view_port);

    furi_message_queue_free(app->input_queue);
    furi_mutex_free(app->radio_mutex);
    furi_mutex_free(app->state_mutex);
    free(app);
}

// PORT: ported external apps run only on the opted-in ESP32-S3 boards (T-Embed CC1101, Paradoxt,
// ThorChimera, Thormini). The 125 kHz LF wake uses the RFID coil, which those boards carry.
static bool tpms_board_supported(void) {
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

int32_t tpms_bridge_app(void* p) {
    UNUSED(p);
    if(!tpms_board_supported()) return 0;
    s_is_encoder = (furi_hal_board_get_input_layout() == FuriHalBoardInputLayoutEncoder);
    TpmsBridgeApp* app = tpms_bridge_app_alloc();
    app->is_encoder = s_is_encoder;

    bool running = true;
    InputEvent event;
    while(running) {
        if(furi_message_queue_get(app->input_queue, &event, 200) == FuriStatusOk) {
            tpms_bridge_handle_input(app, &event, &running);
        }

        tpms_bridge_reconcile_radio(app);
        view_port_update(app->view_port);
    }

    tpms_bridge_stop_local_rx(app);

    /* While a CLI command is running its code lives in this .fap, so the
     * app must not be unloaded. Ask the session to finish and wait.
     *
     * Wait in bounded steps: if the session does not respond for some
     * reason, staying on screen with a clear message beats pulling the
     * code out from under a running thread. */
    app->stop_requested = true;
    uint32_t waited_ms = 0;
    while(app->cli_sessions > 0) {
        furi_delay_ms(20);
        waited_ms += 20;
        if(waited_ms > TPMS_CLI_STOP_TIMEOUT_MS) {
            FURI_LOG_E(TAG, "cli session did not stop, keeping app loaded");
            furi_mutex_acquire(app->state_mutex, FuriWaitForever);
            app->exit_blocked = true;
            furi_mutex_release(app->state_mutex);
            view_port_update(app->view_port);
            waited_ms = 0;
        }
    }

    tpms_bridge_app_free(app);
    return 0;
}
