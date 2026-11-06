#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_infrared.h>
#include <furi_hal_board_info.h>
#include <dialogs/dialogs.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <input/input.h>
#include <storage/storage.h>
#include <toolbox/saved_struct.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>

// ESP32 port notes (details: PORT_NOTES.md)
//  - Nothing here knows a GPIO number. The infrared HAL picks the output pin for every
//    transmission from Infrared > Settings (internal LED or the external pin) and does
//    the pin leasing, so this app always follows what the user configured there.
//  - Carrier mode and pulse mode are both sent by the HAL's RMT transmitter (pulse mode
//    without a carrier), so the timing is exact and the pad is handled like every other IR
//    transmission. There is no GPIO timer any more.

#define FLIPIRFREQ_DEFAULT_FREQUENCY 38000U
#define FLIPIRFREQ_DEFAULT_DUTY_CYCLE 33U
#define FLIPIRFREQ_DEFAULT_BURST_MS 250U
#define FLIPIRFREQ_DEFAULT_SIGNAL_MODE FlipIRFreqSignalModeCarrier
#define FLIPIRFREQ_TX_CHUNK_US 25000U
#define FLIPIRFREQ_TX_PACKET_US 1000000U
#define FLIPIRFREQ_UI_TICK_HZ 8U
#define FLIPIRFREQ_SETTINGS_PATH APP_DATA_PATH("flipirfreq.settings")
#define FLIPIRFREQ_SETTINGS_MAGIC 0x49
#define FLIPIRFREQ_SETTINGS_VERSION 1U
#define FLIPIRFREQ_PULSE_MIN_FREQUENCY_TENTHS 100U
#define FLIPIRFREQ_PULSE_MAX_FREQUENCY_TENTHS 5000U
#define FLIPIRFREQ_MIN_DUTY_CYCLE 1U
#define FLIPIRFREQ_MAX_DUTY_CYCLE 99U
#define FLIPIRFREQ_MIN_BURST_MS 1U
#define FLIPIRFREQ_MAX_BURST_MS 5000U
#define FLIPIRFREQ_VISIBLE_ROWS 4U
#define FLIPIRFREQ_ROW_HEIGHT 9

typedef enum {
    FlipIRFreqFieldFrequency,
    FlipIRFreqFieldDutyCycle,
    FlipIRFreqFieldBurst,
    FlipIRFreqFieldSignal,
    FlipIRFreqFieldMode,
    FlipIRFreqFieldOutput,
    FlipIRFreqFieldSend,
    FlipIRFreqFieldCount,
} FlipIRFreqField;

typedef enum {
    FlipIRFreqModeBurst,
    FlipIRFreqModeContinuous,
    FlipIRFreqModeCount,
} FlipIRFreqMode;

typedef enum {
    FlipIRFreqSignalModeCarrier,
    FlipIRFreqSignalModePulse,
    FlipIRFreqSignalModeCount,
} FlipIRFreqSignalMode;

typedef enum {
    FlipIRFreqEventTypeInput,
    FlipIRFreqEventTypeTick,
    FlipIRFreqEventTypeTxFinished,
} FlipIRFreqEventType;

typedef struct {
    FlipIRFreqEventType type;
    InputEvent input;
    uint32_t generation; // which transmission posted a TxFinished event
    bool error; // the HAL gave up on the transmission
} FlipIRFreqEvent;

// On disk. The output selection of the upstream app is gone (the pin follows Infrared
// Settings); its byte stays so settings files of earlier versions keep loading.
typedef struct {
    uint32_t frequency;
    uint32_t pulse_frequency_tenths;
    uint8_t duty_cycle;
    uint16_t burst_ms;
    uint8_t output_mode; // unused
    uint8_t tx_mode;
    uint8_t signal_mode;
} FlipIRFreqSettings;

// Read by the HAL's TX task while a transmission runs, written by the app thread only
// before it starts.
typedef struct {
    bool continuous;
    bool pulse; // plain on/off pulse train instead of 25 ms carrier chunks
    uint32_t remaining_us;
    uint32_t chunk_us;
    uint32_t on_us;
    uint32_t off_us;
    uint32_t packet_us; // time already queued in the current packet
    bool level; // level of the next pulse phase
    atomic_bool last_packet_queued; // the callback has returned LastDone
} FlipIRFreqTxContext;

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* event_queue;
    FuriTimer* ui_timer;
    FuriMutex* mutex;

    uint32_t frequency;
    uint32_t pulse_frequency_tenths;
    uint8_t duty_cycle;
    uint16_t burst_ms;
    FlipIRFreqField selected_field;
    uint8_t scroll_offset;
    FlipIRFreqMode tx_mode;
    FlipIRFreqSignalMode signal_mode;

    bool running;
    bool transmitting;
    bool ir_claimed; // we hold the infrared peripheral (between claim and the final stop)
    bool sleep_inhibited;
    atomic_uint tx_generation;
    // The HAL callbacks also leave a mark here, because their event can be lost (the queue
    // is full while the user hammers the keys): generation + 1 of the transmission that
    // ended. The UI tick looks at it, so a transmission can never stay on "LIVE".
    atomic_uint tx_done_gen;
    atomic_bool tx_done_error;
    bool settings_dirty;
    uint32_t last_settings_change_tick;
    uint8_t tx_anim_phase;
    // Encoder boards (T-Embed, ThorChimera): the wheel only sends Up/Down, so OK on a value
    // row toggles edit mode and the wheel then changes the value (quick turns = coarse).
    bool encoder;
    bool enc_editing;
    uint32_t enc_last_step;
    FlipIRFreqTxContext tx_context;
    char status[16];
} FlipIRFreqApp;

static void flipirfreq_ensure_selection_visible(FlipIRFreqApp* app);

static uint32_t flipirfreq_clamp_i32_to_u32(int32_t value, uint32_t min, uint32_t max) {
    if(value < (int32_t)min) return min;
    if((uint32_t)value > max) return max;
    return (uint32_t)value;
}

static uint8_t flipirfreq_clamp_i32_to_u8(int32_t value, uint8_t min, uint8_t max) {
    if(value < min) return min;
    if(value > max) return max;
    return (uint8_t)value;
}

static const char* flipirfreq_mode_label(FlipIRFreqMode mode) {
    switch(mode) {
    case FlipIRFreqModeBurst:
        return "BURST";
    case FlipIRFreqModeContinuous:
        return "CONT";
    default:
        return "?";
    }
}

static const char* flipirfreq_signal_mode_label(FlipIRFreqSignalMode mode) {
    switch(mode) {
    case FlipIRFreqSignalModeCarrier:
        return "CARR";
    case FlipIRFreqSignalModePulse:
        return "PULSE";
    default:
        return "?";
    }
}

// What Infrared > Settings currently selects, as the HAL will use it for the next send.
static void flipirfreq_output_text(char* value, size_t size) {
    if(!furi_hal_infrared_is_available()) {
        snprintf(value, size, "NO IR");
        return;
    }
    const uint16_t ext_pin = furi_hal_infrared_get_ext_tx_pin();
    if(ext_pin != UINT16_MAX && furi_hal_infrared_get_tx_output() == FuriHalInfraredTxPinExtPA7) {
        snprintf(value, size, "EXT IO%u", (unsigned int)ext_pin);
    } else {
        snprintf(value, size, "INT LED");
    }
}

static uint32_t flipirfreq_frequency_min(FlipIRFreqApp* app) {
    return (app->signal_mode == FlipIRFreqSignalModePulse) ? FLIPIRFREQ_PULSE_MIN_FREQUENCY_TENTHS :
                                                             INFRARED_MIN_FREQUENCY;
}

static uint32_t flipirfreq_frequency_max(FlipIRFreqApp* app) {
    return (app->signal_mode == FlipIRFreqSignalModePulse) ? FLIPIRFREQ_PULSE_MAX_FREQUENCY_TENTHS :
                                                             INFRARED_MAX_FREQUENCY;
}

static uint32_t flipirfreq_frequency_step(bool coarse) {
    return coarse ? 1000U : 100U;
}

static uint32_t flipirfreq_frequency_step_for_mode(FlipIRFreqApp* app, bool coarse) {
    if(app->signal_mode == FlipIRFreqSignalModePulse) {
        return coarse ? 10U : 1U;
    }

    return flipirfreq_frequency_step(coarse);
}

static uint16_t flipirfreq_burst_step(bool coarse) {
    return coarse ? 100U : 10U;
}

static void flipirfreq_set_status(FlipIRFreqApp* app, const char* text) {
    snprintf(app->status, sizeof(app->status), "%s", text);
}

static void flipirfreq_mark_settings_dirty(FlipIRFreqApp* app) {
    app->settings_dirty = true;
    app->last_settings_change_tick = furi_get_tick();
}

static void flipirfreq_load_defaults(FlipIRFreqApp* app) {
    app->frequency = FLIPIRFREQ_DEFAULT_FREQUENCY;
    app->pulse_frequency_tenths = 380U;
    app->duty_cycle = FLIPIRFREQ_DEFAULT_DUTY_CYCLE;
    app->burst_ms = FLIPIRFREQ_DEFAULT_BURST_MS;
    app->tx_mode = FlipIRFreqModeBurst;
    app->signal_mode = FLIPIRFREQ_DEFAULT_SIGNAL_MODE;
}

static void flipirfreq_save_settings(FlipIRFreqApp* app) {
    FlipIRFreqSettings settings = {
        .frequency = app->frequency,
        .pulse_frequency_tenths = app->pulse_frequency_tenths,
        .duty_cycle = app->duty_cycle,
        .burst_ms = app->burst_ms,
        .output_mode = 0,
        .tx_mode = app->tx_mode,
        .signal_mode = app->signal_mode,
    };

    if(saved_struct_save(
           FLIPIRFREQ_SETTINGS_PATH,
           &settings,
           sizeof(settings),
           FLIPIRFREQ_SETTINGS_MAGIC,
           FLIPIRFREQ_SETTINGS_VERSION)) {
        app->settings_dirty = false;
    }
}

static void flipirfreq_load_settings(FlipIRFreqApp* app) {
    FlipIRFreqSettings settings = {0};
    flipirfreq_load_defaults(app);

    if(saved_struct_load(
           FLIPIRFREQ_SETTINGS_PATH,
           &settings,
           sizeof(settings),
           FLIPIRFREQ_SETTINGS_MAGIC,
           FLIPIRFREQ_SETTINGS_VERSION)) {
        app->duty_cycle = flipirfreq_clamp_i32_to_u8(
            settings.duty_cycle, FLIPIRFREQ_MIN_DUTY_CYCLE, FLIPIRFREQ_MAX_DUTY_CYCLE);
        app->burst_ms = flipirfreq_clamp_i32_to_u32(
            settings.burst_ms, FLIPIRFREQ_MIN_BURST_MS, FLIPIRFREQ_MAX_BURST_MS);
        app->tx_mode = (settings.tx_mode < FlipIRFreqModeCount) ?
                           (FlipIRFreqMode)settings.tx_mode :
                           FlipIRFreqModeBurst;
        app->signal_mode = (settings.signal_mode < FlipIRFreqSignalModeCount) ?
                               (FlipIRFreqSignalMode)settings.signal_mode :
                               FLIPIRFREQ_DEFAULT_SIGNAL_MODE;
        app->frequency = flipirfreq_clamp_i32_to_u32(
            settings.frequency, INFRARED_MIN_FREQUENCY, INFRARED_MAX_FREQUENCY);
        app->pulse_frequency_tenths = flipirfreq_clamp_i32_to_u32(
            settings.pulse_frequency_tenths,
            FLIPIRFREQ_PULSE_MIN_FREQUENCY_TENTHS,
            FLIPIRFREQ_PULSE_MAX_FREQUENCY_TENTHS);
    }
}

static uint32_t flipirfreq_get_active_frequency(FlipIRFreqApp* app) {
    return (app->signal_mode == FlipIRFreqSignalModePulse) ? app->pulse_frequency_tenths :
                                                             app->frequency;
}

static void flipirfreq_set_active_frequency(FlipIRFreqApp* app, uint32_t value) {
    if(app->signal_mode == FlipIRFreqSignalModePulse) {
        app->pulse_frequency_tenths = value;
    } else {
        app->frequency = value;
    }
}

// The HAL sends each "packet" (every Done) as one RMT transaction and fires the
// signal-sent callback after every one of them. Packets are therefore about one second long
// (the carrier or the pulse train has a tiny gap between two packets, so there are few of
// them), and the burst is only reported finished once its last packet (LastDone) was sent.
// A stop does not wait for the packet: it uses furi_hal_infrared_async_tx_abort().
static FuriHalInfraredTxGetDataState
    flipirfreq_tx_data_callback(void* context, uint32_t* duration, bool* level) {
    FlipIRFreqTxContext* tx_context = context;

    bool mark;
    uint32_t phase_us;
    if(tx_context->pulse) {
        mark = tx_context->level;
        phase_us = mark ? tx_context->on_us : tx_context->off_us;
        tx_context->level = !tx_context->level;
    } else {
        mark = true;
        phase_us = tx_context->chunk_us;
    }

    bool last = false;
    if(!tx_context->continuous) {
        if(phase_us >= tx_context->remaining_us) {
            phase_us = tx_context->remaining_us;
            tx_context->remaining_us = 0;
            last = true;
        } else {
            tx_context->remaining_us -= phase_us;
        }
    }

    *level = mark;
    *duration = phase_us;

    if(last) {
        atomic_store_explicit(&tx_context->last_packet_queued, true, memory_order_release);
        return FuriHalInfraredTxGetDataStateLastDone;
    }

    tx_context->packet_us += phase_us;
    // A pulse packet ends after a space, so it holds whole mark/space pairs.
    if(tx_context->packet_us >= FLIPIRFREQ_TX_PACKET_US && (!tx_context->pulse || !mark)) {
        tx_context->packet_us = 0;
        return FuriHalInfraredTxGetDataStateDone;
    }
    return FuriHalInfraredTxGetDataStateOk;
}

static void flipirfreq_tx_finished_callback(void* context) {
    FlipIRFreqApp* app = context;
    const uint32_t generation = atomic_load_explicit(&app->tx_generation, memory_order_acquire);
    atomic_store_explicit(&app->tx_done_gen, generation + 1U, memory_order_release);
    FlipIRFreqEvent event = {
        .type = FlipIRFreqEventTypeTxFinished,
        .generation = generation,
    };
    furi_message_queue_put(app->event_queue, &event, 0);
}

// Called by the HAL after EVERY packet; only the last one ends a burst.
static void flipirfreq_tx_packet_sent_callback(void* context) {
    FlipIRFreqApp* app = context;
    if(atomic_load_explicit(&app->tx_context.last_packet_queued, memory_order_acquire)) {
        flipirfreq_tx_finished_callback(app);
    }
}

static void flipirfreq_tx_error_callback(void* context) {
    FlipIRFreqApp* app = context;
    const uint32_t generation = atomic_load_explicit(&app->tx_generation, memory_order_acquire);
    atomic_store_explicit(&app->tx_done_error, true, memory_order_relaxed);
    atomic_store_explicit(&app->tx_done_gen, generation + 1U, memory_order_release);
    FlipIRFreqEvent event = {
        .type = FlipIRFreqEventTypeTxFinished,
        .generation = generation,
        .error = true,
    };
    furi_message_queue_put(app->event_queue, &event, 0);
}

// End of a transmission (or of a failed start): forget our callbacks (they point into this
// app) and give the peripheral and the light-sleep veto back. The HAL has already released
// the output pin by itself.
static void flipirfreq_release_hal(FlipIRFreqApp* app) {
    furi_hal_infrared_async_tx_reset_callbacks();
    app->ir_claimed = false;
    if(app->sleep_inhibited) {
        furi_hal_power_light_sleep_inhibit_exit();
        app->sleep_inhibited = false;
    }
}

static void flipirfreq_stop_transmit(FlipIRFreqApp* app, bool interrupted) {
    if(!app->transmitting) {
        return;
    }

    // From here on the TX callbacks' late events are stale.
    app->transmitting = false;
    atomic_fetch_add_explicit(&app->tx_generation, 1, memory_order_acq_rel);

    if(interrupted) {
        // The user's STOP must act now, not after the packet in flight (up to a second).
        furi_hal_infrared_async_tx_abort();
    } else {
        // The burst is over: the HAL task finishes by itself, this only waits for it.
        furi_hal_infrared_async_tx_stop();
    }
    flipirfreq_release_hal(app);

    app->tx_anim_phase = 0;
    flipirfreq_set_status(app, interrupted ? "stopped" : "DONE");
}

static void flipirfreq_start_transmit(FlipIRFreqApp* app) {
    if(!furi_hal_infrared_is_available()) {
        flipirfreq_set_status(app, "NO IR");
        return;
    }
    // Atomic: nobody (CLI, JS, an IR app) can start between this check and our start.
    if(!furi_hal_infrared_claim_tx()) {
        flipirfreq_set_status(app, "BUSY");
        return;
    }
    app->ir_claimed = true;
    atomic_fetch_add_explicit(&app->tx_generation, 1, memory_order_acq_rel);
    atomic_store_explicit(&app->tx_done_gen, 0, memory_order_release);
    atomic_store_explicit(&app->tx_done_error, false, memory_order_relaxed);

    FlipIRFreqTxContext* tx = &app->tx_context;
    memset(tx, 0, sizeof(*tx));
    tx->continuous = (app->tx_mode == FlipIRFreqModeContinuous);
    tx->remaining_us = (uint32_t)app->burst_ms * 1000U;
    tx->chunk_us = FLIPIRFREQ_TX_CHUNK_US;
    atomic_init(&tx->last_packet_queued, false);

    const bool pulse = (app->signal_mode == FlipIRFreqSignalModePulse);
    if(pulse) {
        // period = 1 / (tenths / 10) s
        const uint32_t period_us = 10000000U / app->pulse_frequency_tenths;
        uint32_t on_us = (uint32_t)(((uint64_t)period_us * app->duty_cycle) / 100U);
        if(on_us == 0U) on_us = 1U;
        if(on_us >= period_us) on_us = period_us - 1U;
        tx->pulse = true;
        tx->on_us = on_us;
        tx->off_us = period_us - on_us;
        tx->level = true;
    }

    furi_hal_infrared_async_tx_set_data_isr_callback(flipirfreq_tx_data_callback, tx);
    furi_hal_infrared_async_tx_set_signal_sent_isr_callback(
        tx->continuous ? NULL : flipirfreq_tx_packet_sent_callback, app);
    // A transmission that dies on a channel/pin error must not leave the UI on "LIVE".
    furi_hal_infrared_async_tx_set_error_callback(flipirfreq_tx_error_callback, app);

    // The RMT output and the pin selection (Infrared Settings) belong to the HAL.
    const bool accepted =
        pulse ? furi_hal_infrared_async_tx_start_plain() :
                furi_hal_infrared_async_tx_start(
                    app->frequency, (float)app->duty_cycle / 100.0f);
    if(!accepted) {
        // Only possible if someone else holds the peripheral: nothing was started.
        furi_hal_infrared_release_claim();
        flipirfreq_release_hal(app);
        flipirfreq_set_status(app, "BUSY");
        return;
    }

    const FuriHalInfraredTxError error = furi_hal_infrared_get_last_tx_error();
    if(error != FuriHalInfraredTxErrorNone) {
        // Refused at once (external pin owned by another function, no IR hardware, RMT
        // setup failed): the HAL still expects its finished transmission to be reaped.
        furi_hal_infrared_async_tx_wait_termination();
        flipirfreq_release_hal(app);
        flipirfreq_set_status(
            app,
            error == FuriHalInfraredTxErrorExtPinBusy ? "PIN BUSY" :
            error == FuriHalInfraredTxErrorNoHardware ? "NO IR" :
                                                        "FAILED");
        return;
    }

    // The RMT driver already keeps the SoC out of light sleep while it transmits, and the
    // loader holds insomnia for the whole app. This veto is the explicit third layer: it is
    // taken only after a successful start and always returned by flipirfreq_release_hal().
    furi_hal_power_light_sleep_inhibit_enter();
    app->sleep_inhibited = true;

    app->transmitting = true;
    app->tx_anim_phase = 0;
    app->selected_field = FlipIRFreqFieldSend;
    flipirfreq_ensure_selection_visible(app);

    flipirfreq_set_status(app, "LIVE");
}

static void flipirfreq_adjust_field(FlipIRFreqApp* app, bool increase, bool coarse) {
    const int32_t direction = increase ? 1 : -1;

    switch(app->selected_field) {
    case FlipIRFreqFieldFrequency: {
        int32_t next = (int32_t)flipirfreq_get_active_frequency(app) +
                       (int32_t)flipirfreq_frequency_step_for_mode(app, coarse) * direction;
        flipirfreq_set_active_frequency(
            app,
            flipirfreq_clamp_i32_to_u32(
                next, flipirfreq_frequency_min(app), flipirfreq_frequency_max(app)));
        break;
    }
    case FlipIRFreqFieldDutyCycle: {
        int32_t next = (int32_t)app->duty_cycle + (coarse ? 5 : 1) * direction;
        app->duty_cycle =
            flipirfreq_clamp_i32_to_u8(next, FLIPIRFREQ_MIN_DUTY_CYCLE, FLIPIRFREQ_MAX_DUTY_CYCLE);
        break;
    }
    case FlipIRFreqFieldBurst: {
        int32_t next = (int32_t)app->burst_ms + (int32_t)flipirfreq_burst_step(coarse) * direction;
        app->burst_ms =
            flipirfreq_clamp_i32_to_u32(next, FLIPIRFREQ_MIN_BURST_MS, FLIPIRFREQ_MAX_BURST_MS);
        break;
    }
    case FlipIRFreqFieldSignal: {
        int32_t next = (int32_t)app->signal_mode + direction;
        if(next < 0) {
            next = FlipIRFreqSignalModeCount - 1;
        } else if(next >= FlipIRFreqSignalModeCount) {
            next = 0;
        }
        app->signal_mode = next;
        break;
    }
    case FlipIRFreqFieldMode: {
        int32_t next = (int32_t)app->tx_mode + direction;
        if(next < 0) {
            next = FlipIRFreqModeCount - 1;
        } else if(next >= FlipIRFreqModeCount) {
            next = 0;
        }
        app->tx_mode = next;
        break;
    }
    default:
        break;
    }
}

static const char* flipirfreq_tx_indicator(uint8_t phase) {
    static const char* frames[] = {"TX", "TX.", "TX..", "TX..."};
    return frames[phase % COUNT_OF(frames)];
}

static void flipirfreq_ensure_selection_visible(FlipIRFreqApp* app) {
    if(app->selected_field < app->scroll_offset) {
        app->scroll_offset = app->selected_field;
    } else if(app->selected_field >= (app->scroll_offset + FLIPIRFREQ_VISIBLE_ROWS)) {
        app->scroll_offset = app->selected_field - FLIPIRFREQ_VISIBLE_ROWS + 1U;
    }
}

static const char* flipirfreq_field_label(FlipIRFreqField field) {
    switch(field) {
    case FlipIRFreqFieldFrequency:
        return "Frequency";
    case FlipIRFreqFieldDutyCycle:
        return "Duty Cycle";
    case FlipIRFreqFieldBurst:
        return "Burst Time";
    case FlipIRFreqFieldSignal:
        return "Signal Type";
    case FlipIRFreqFieldMode:
        return "Tx Mode";
    case FlipIRFreqFieldOutput:
        return "IR Output";
    case FlipIRFreqFieldSend:
        return "Transmit";
    default:
        return "?";
    }
}

static void
    flipirfreq_field_value(FlipIRFreqApp* app, FlipIRFreqField field, char* value, size_t size) {
    switch(field) {
    case FlipIRFreqFieldFrequency:
        if(app->signal_mode == FlipIRFreqSignalModePulse) {
            snprintf(
                value,
                size,
                "%lu.%lu Hz",
                (unsigned long)(app->pulse_frequency_tenths / 10U),
                (unsigned long)(app->pulse_frequency_tenths % 10U));
        } else {
            snprintf(value, size, "%lu Hz", (unsigned long)app->frequency);
        }
        break;
    case FlipIRFreqFieldDutyCycle:
        snprintf(value, size, "%u%%", app->duty_cycle);
        break;
    case FlipIRFreqFieldBurst:
        snprintf(value, size, "%u ms", (unsigned int)app->burst_ms);
        break;
    case FlipIRFreqFieldSignal:
        snprintf(value, size, "%s", flipirfreq_signal_mode_label(app->signal_mode));
        break;
    case FlipIRFreqFieldMode:
        snprintf(value, size, "%s", flipirfreq_mode_label(app->tx_mode));
        break;
    case FlipIRFreqFieldOutput:
        flipirfreq_output_text(value, size);
        break;
    case FlipIRFreqFieldSend:
        snprintf(
            value,
            size,
            "%s",
            app->transmitting ? "STOP" :
                                (app->tx_mode == FlipIRFreqModeContinuous ? "START" : "SEND"));
        break;
    default:
        value[0] = '\0';
        break;
    }
}

static void flipirfreq_draw_row(
    Canvas* canvas,
    int32_t y,
    bool selected,
    const char* label,
    const char* value) {
    if(selected) {
        canvas_draw_box(canvas, 0, y - 7, 128, 9);
        canvas_set_color(canvas, ColorWhite);
    } else {
        canvas_set_color(canvas, ColorBlack);
    }

    canvas_draw_str(canvas, 3, y, label);
    canvas_draw_str_aligned(canvas, 125, y, AlignRight, AlignBottom, value);

    if(selected) {
        canvas_set_color(canvas, ColorBlack);
    }
}

static void flipirfreq_draw_callback(Canvas* canvas, void* context) {
    FlipIRFreqApp* app = context;
    if(furi_mutex_acquire(app->mutex, FuriWaitForever) != FuriStatusOk) return;
    char value[32];

    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 9, "FlipIRFreq");
    if(app->transmitting) {
        canvas_draw_str_aligned(
            canvas, 125, 9, AlignRight, AlignBottom, flipirfreq_tx_indicator(app->tx_anim_phase));
    } else if(app->status[0] != '\0') {
        canvas_draw_str_aligned(canvas, 125, 9, AlignRight, AlignBottom, app->status);
    }

    canvas_set_font(canvas, FontSecondary);

    for(uint8_t row = 0; row < FLIPIRFREQ_VISIBLE_ROWS; row++) {
        const uint8_t field_index = app->scroll_offset + row;
        if(field_index >= FlipIRFreqFieldCount) break;

        const FlipIRFreqField field = (FlipIRFreqField)field_index;
        flipirfreq_field_value(app, field, value, sizeof(value));
        if(app->enc_editing && app->selected_field == field) {
            char edited[sizeof(value) + 4];
            snprintf(edited, sizeof(edited), "< %s >", value);
            strlcpy(value, edited, sizeof(value));
        }
        flipirfreq_draw_row(
            canvas,
            18 + (row * FLIPIRFREQ_ROW_HEIGHT),
            app->selected_field == field,
            flipirfreq_field_label(field),
            value);
    }

    if(app->scroll_offset > 0) {
        canvas_draw_str(canvas, 120, 16, "^");
    }
    if((app->scroll_offset + FLIPIRFREQ_VISIBLE_ROWS) < FlipIRFreqFieldCount) {
        canvas_draw_str(canvas, 120, 53, "v");
    }

    canvas_draw_line(canvas, 2, 54, 126, 54);
    canvas_draw_str(canvas, 2, 62, "By ConsultingJoe.com");
    furi_mutex_release(app->mutex);
}

static void flipirfreq_input_callback(InputEvent* input_event, void* context) {
    FlipIRFreqApp* app = context;
    FlipIRFreqEvent event = {.type = FlipIRFreqEventTypeInput, .input = *input_event};
    furi_message_queue_put(app->event_queue, &event, 0);
}

static bool flipirfreq_field_is_editable(FlipIRFreqField field) {
    return field != FlipIRFreqFieldOutput && field != FlipIRFreqFieldSend;
}

// Encoder only. Returns true when the event was handled here.
static bool flipirfreq_handle_encoder(FlipIRFreqApp* app, const InputEvent* input) {
    if(input->key == InputKeyOk && input->type == InputTypeShort) {
        if(app->selected_field == FlipIRFreqFieldSend) {
            app->enc_editing = false;
            flipirfreq_start_transmit(app);
        } else if(flipirfreq_field_is_editable(app->selected_field)) {
            app->enc_editing = !app->enc_editing;
        }
        return true;
    }
    if(input->key != InputKeyUp && input->key != InputKeyDown) return false;

    // One wheel detent = Press + Short + Release: act on Press only.
    if(input->type != InputTypePress) return true;
    if(app->enc_editing) {
        const uint32_t now = furi_get_tick();
        const bool coarse = (now - app->enc_last_step) < furi_ms_to_ticks(90);
        app->enc_last_step = now;
        flipirfreq_adjust_field(app, input->key == InputKeyDown, coarse);
        flipirfreq_mark_settings_dirty(app);
    } else if(input->key == InputKeyUp) {
        app->selected_field =
            app->selected_field == 0 ? FlipIRFreqFieldCount - 1 : app->selected_field - 1;
        flipirfreq_ensure_selection_visible(app);
    } else {
        app->selected_field = (app->selected_field + 1) % FlipIRFreqFieldCount;
        flipirfreq_ensure_selection_visible(app);
    }
    return true;
}

static void flipirfreq_handle_input(FlipIRFreqApp* app, const InputEvent* input) {
    const bool pressed = (input->type == InputTypeShort) || (input->type == InputTypeRepeat);
    const bool coarse = input->type == InputTypeRepeat;

    if(app->enc_editing && input->key == InputKeyBack && input->type == InputTypeShort) {
        app->enc_editing = false; // leave edit mode first, Back again exits
        return;
    }

    if(input->key == InputKeyBack && input->type == InputTypeShort) {
        if(app->transmitting) {
            flipirfreq_stop_transmit(app, true);
            return;
        }
        app->running = false;
        return;
    }

    if(app->transmitting) {
        if((input->key == InputKeyOk) && (input->type == InputTypeShort)) {
            flipirfreq_stop_transmit(app, true);
        }
        return;
    }

    if(app->encoder && flipirfreq_handle_encoder(app, input)) {
        return;
    }

    if(!pressed && input->key != InputKeyOk) {
        return;
    }

    switch(input->key) {
    case InputKeyUp:
        if(pressed) {
            if(app->selected_field == 0) {
                app->selected_field = FlipIRFreqFieldCount - 1;
            } else {
                app->selected_field--;
            }
            flipirfreq_ensure_selection_visible(app);
        }
        break;
    case InputKeyDown:
        if(pressed) {
            app->selected_field = (app->selected_field + 1) % FlipIRFreqFieldCount;
            flipirfreq_ensure_selection_visible(app);
        }
        break;
    case InputKeyLeft:
        if(pressed) {
            flipirfreq_adjust_field(app, false, coarse);
            if(app->selected_field != FlipIRFreqFieldSend &&
               app->selected_field != FlipIRFreqFieldOutput) {
                flipirfreq_mark_settings_dirty(app);
            }
        }
        break;
    case InputKeyRight:
        if(pressed) {
            flipirfreq_adjust_field(app, true, coarse);
            if(app->selected_field != FlipIRFreqFieldSend &&
               app->selected_field != FlipIRFreqFieldOutput) {
                flipirfreq_mark_settings_dirty(app);
            }
        }
        break;
    case InputKeyOk:
        if(input->type == InputTypeShort) {
            if(app->selected_field == FlipIRFreqFieldSend) {
                flipirfreq_start_transmit(app);
            }
        }
        break;
    default:
        break;
    }
}

static void flipirfreq_tick_callback(void* context) {
    FlipIRFreqApp* app = context;
    FlipIRFreqEvent event = {.type = FlipIRFreqEventTypeTick};
    furi_message_queue_put(app->event_queue, &event, 0);
}

// A transmission ended by itself (burst done) or failed. Called for the event and, as a
// safety net for a lost event, from the UI tick; idempotent.
static void flipirfreq_handle_tx_done(FlipIRFreqApp* app, bool error) {
    if(app->transmitting && (error || app->tx_mode == FlipIRFreqModeBurst)) {
        flipirfreq_stop_transmit(app, false);
        if(error) flipirfreq_set_status(app, "TX ERROR");
    }
}

static void flipirfreq_handle_tick(FlipIRFreqApp* app) {
    if(app->transmitting) {
        app->tx_anim_phase = (app->tx_anim_phase + 1) & 0x03;
        if(atomic_load_explicit(&app->tx_done_gen, memory_order_acquire) ==
           atomic_load_explicit(&app->tx_generation, memory_order_acquire) + 1U) {
            flipirfreq_handle_tx_done(
                app, atomic_load_explicit(&app->tx_done_error, memory_order_relaxed));
        }
    }

    if(app->settings_dirty &&
       ((furi_get_tick() - app->last_settings_change_tick) >=
        (furi_kernel_get_tick_frequency() / 2))) {
        flipirfreq_save_settings(app);
    }
}

static FlipIRFreqApp* flipirfreq_app_alloc(void) {
    FlipIRFreqApp* app = calloc(1, sizeof(FlipIRFreqApp));
    if(app) {
        app->encoder = furi_hal_board_get_input_layout() == FuriHalBoardInputLayoutEncoder;
    }
    if(!app) return NULL;
    atomic_init(&app->tx_generation, 0);
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->gui = furi_record_open(RECORD_GUI);
    app->view_port = view_port_alloc();
    app->event_queue = furi_message_queue_alloc(16, sizeof(FlipIRFreqEvent));
    app->ui_timer = furi_timer_alloc(flipirfreq_tick_callback, FuriTimerTypePeriodic, app);
    if(!app->mutex || !app->gui || !app->view_port || !app->event_queue || !app->ui_timer) {
        if(app->ui_timer) furi_timer_free(app->ui_timer);
        if(app->event_queue) furi_message_queue_free(app->event_queue);
        if(app->view_port) view_port_free(app->view_port);
        if(app->gui) furi_record_close(RECORD_GUI);
        if(app->mutex) furi_mutex_free(app->mutex);
        free(app);
        return NULL;
    }

    flipirfreq_load_settings(app);
    app->selected_field = FlipIRFreqFieldFrequency;
    app->scroll_offset = 0;
    app->running = true;
    flipirfreq_set_status(app, "");

    view_port_draw_callback_set(app->view_port, flipirfreq_draw_callback, app);
    view_port_input_callback_set(app->view_port, flipirfreq_input_callback, app);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);
    furi_timer_start(app->ui_timer, furi_kernel_get_tick_frequency() / FLIPIRFREQ_UI_TICK_HZ);

    return app;
}

static void flipirfreq_app_free(FlipIRFreqApp* app) {
    if(!app) return;

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    if(app->transmitting) {
        flipirfreq_stop_transmit(app, true);
    }
    if(app->settings_dirty) {
        flipirfreq_save_settings(app);
    }
    furi_mutex_release(app->mutex);
    furi_timer_stop(app->ui_timer);
    furi_timer_free(app->ui_timer);
    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_message_queue_free(app->event_queue);
    furi_record_close(RECORD_GUI);
    furi_mutex_free(app->mutex);
    free(app);
}

// Supported boards only (LilyGo T-Embed, Paradoxt, ThorChimera, Thormini): the pins, power
// and USB layout of any other board were never checked against this app.
static bool flipirfreq_board_supported(void) {
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

int32_t flipirfreq_app(void* p) {
    UNUSED(p);
    if(!flipirfreq_board_supported()) return 0;

    FlipIRFreqApp* app = flipirfreq_app_alloc();
    if(!app) return 255;
    FlipIRFreqEvent event;

    view_port_update(app->view_port);

    while(app->running) {
        if(furi_message_queue_get(app->event_queue, &event, FuriWaitForever) == FuriStatusOk) {
            if(furi_mutex_acquire(app->mutex, FuriWaitForever) != FuriStatusOk) continue;
            if(event.type == FlipIRFreqEventTypeInput) {
                flipirfreq_handle_input(app, &event.input);
            } else if(event.type == FlipIRFreqEventTypeTick) {
                flipirfreq_handle_tick(app);
            } else if(event.type == FlipIRFreqEventTypeTxFinished) {
                // A late event of an earlier transmission must not stop the current one.
                if(event.generation ==
                   atomic_load_explicit(&app->tx_generation, memory_order_acquire)) {
                    flipirfreq_handle_tx_done(app, event.error);
                }
            }
            furi_mutex_release(app->mutex);
            view_port_update(app->view_port);
        }
    }

    flipirfreq_app_free(app);
    return 0;
}
