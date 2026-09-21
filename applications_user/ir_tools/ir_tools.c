/*
 * IR Tools -- original, from-scratch app (no external source available to
 * port from; see the app's git history/PR description for context).
 *
 * Two small utilities:
 *   - Generator: transmits a continuous, adjustable-frequency IR carrier.
 *     Useful for testing IR receivers/AGC, or as a crude "universal jam"
 *     tone. Internal/external TX pin selectable.
 *   - Searcher: listens for any decodable IR remote signal and reports the
 *     carrier frequency (and protocol) of whatever it caught, via the
 *     decoded protocol's known carrier frequency.
 */
#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/elements.h>
#include <lib/infrared/worker/infrared_worker.h>
#include <string.h>
#include <stdio.h>

#define IR_TOOLS_MIN_KHZ (INFRARED_MIN_FREQUENCY / 1000U)
#define IR_TOOLS_MAX_KHZ (INFRARED_MAX_FREQUENCY / 1000U)
#define IR_TOOLS_DEFAULT_KHZ 38U
#define IR_TOOLS_STEP_KHZ 1U
#define IR_TOOLS_DUTY_CYCLE 0.33f
#define IR_TOOLS_PROTOCOL_NAME_LEN 24U
// Hop mode: cycles the carrier frequency instead of holding one tone --
// useful for sweeping an IR receiver's whole response range. There's no API
// to retune a running transmission, so each tick stops and restarts TX at
// the next frequency; short enough (200ms) to look continuous, long enough
// not to hammer the hardware.
#define IR_TOOLS_HOP_STEP_KHZ 2U
#define IR_TOOLS_HOP_INTERVAL_MS 200U

typedef enum {
    ViewIdMenu,
    ViewIdGenerator,
    ViewIdSearcher,
} ViewId;

typedef enum {
    MenuItemGenerator,
    MenuItemSearcher,
} MenuItem;

typedef enum {
    EventSignalFound = 100,
} AppEvent;

typedef struct {
    uint32_t freq_khz;
    FuriHalInfraredTxPin tx_pin;
    bool running;
    bool hopping;
} GeneratorModel;

typedef enum {
    SearchIdle,
    SearchScanning,
    SearchFound,
} SearchState;

typedef struct {
    SearchState state;
    FuriHalInfraredRxPin rx_pin;
    uint32_t found_khz;
    char found_protocol[IR_TOOLS_PROTOCOL_NAME_LEN];
} SearcherModel;

typedef struct {
    Gui* gui;
    ViewDispatcher* dispatcher;
    Submenu* menu;
    View* generator_view;
    View* searcher_view;
    InfraredWorker* worker;
    FuriTimer* hop_timer;
    ViewId current_view;
    // Written by the InfraredWorker thread's callback, read by the GUI
    // thread only after EventSignalFound is delivered via the dispatcher's
    // custom-event queue -- that hop is what makes the handoff safe without
    // extra locking (single writer before the event, single reader after).
    uint32_t pending_khz;
    char pending_protocol[IR_TOOLS_PROTOCOL_NAME_LEN];
} App;

// ---------------------------------------------------------------------
// Generator
// ---------------------------------------------------------------------

static FuriHalInfraredTxGetDataState
    generator_tx_data_callback(void* context, uint32_t* duration, bool* level) {
    UNUSED(context);
    // Re-armed every 20ms for as long as transmission keeps running;
    // furi_hal_infrared_async_tx_stop() ends it -- there is no natural
    // "last" packet for a continuous tone.
    //
    // This chunk size matters beyond just callback overhead: on this port,
    // async_tx_stop() blocks on a semaphore that's only given once the
    // in-flight RMT burst (i.e. this duration) finishes -- so it was
    // originally 500ms, making Stop take up to half a second to respond.
    // Hop mode made that worse: each hop tick calls stop()+start() to
    // retune, on a 200ms timer -- shorter than that 500ms worst-case stop
    // latency, so ticks could overrun and starve the input callback of the
    // view model's mutex. 20ms keeps every stop() call, hop or manual, fast
    // enough to stay well under both the hop interval and human perception.
    *level = true;
    *duration = 20000U;
    return FuriHalInfraredTxGetDataStateOk;
}

static void generator_start(App* app, GeneratorModel* model) {
    if(model->running || furi_hal_infrared_is_busy()) return;
    furi_hal_infrared_set_tx_output(model->tx_pin);
    furi_hal_infrared_async_tx_set_data_isr_callback(generator_tx_data_callback, NULL);
    furi_hal_infrared_async_tx_start(model->freq_khz * 1000U, IR_TOOLS_DUTY_CYCLE);
    model->running = true;
    if(model->hopping) furi_timer_start(app->hop_timer, furi_ms_to_ticks(IR_TOOLS_HOP_INTERVAL_MS));
}

static void generator_stop(App* app, GeneratorModel* model) {
    if(!model->running) return;
    if(model->hopping) furi_timer_stop(app->hop_timer);
    furi_hal_infrared_async_tx_stop();
    model->running = false;
}

// FuriTimer callback -- runs on the timer service thread, not the GUI
// thread. Safe here because generator_view's model uses ViewModelTypeLocking
// (with_view_model takes its mutex), and furi_hal_infrared_async_tx_stop()
// is documented to block until transmission actually ends.
static void generator_hop_tick(void* context) {
    App* app = context;
    with_view_model(
        app->generator_view,
        GeneratorModel * model,
        {
            if(model->running && model->hopping) {
                furi_hal_infrared_async_tx_stop();
                model->freq_khz = model->freq_khz + IR_TOOLS_HOP_STEP_KHZ <= IR_TOOLS_MAX_KHZ ?
                                       model->freq_khz + IR_TOOLS_HOP_STEP_KHZ :
                                       IR_TOOLS_MIN_KHZ;
                furi_hal_infrared_async_tx_set_data_isr_callback(generator_tx_data_callback, NULL);
                furi_hal_infrared_async_tx_start(model->freq_khz * 1000U, IR_TOOLS_DUTY_CYCLE);
            }
        },
        true);
}

static void generator_draw_callback(Canvas* canvas, void* ctx) {
    GeneratorModel* model = ctx;
    char freq_str[24];
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 8, AlignCenter, AlignCenter, "IR Generator");
    canvas_set_font(canvas, FontBigNumbers);
    snprintf(freq_str, sizeof(freq_str), "%lu kHz", (unsigned long)model->freq_khz);
    canvas_draw_str_aligned(canvas, 64, 25, AlignCenter, AlignCenter, freq_str);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(
        canvas,
        64,
        38,
        AlignCenter,
        AlignCenter,
        model->tx_pin == FuriHalInfraredTxPinExtPA7 ? "Pin: External" : "Pin: Internal");
    // elements_button_*() draw a 12px-tall hint box from the very bottom
    // edge (y = canvas_height()) upward, i.e. it occupies y=52..64 on this
    // 64px-tall display -- keep this line's own footprint above that.
    canvas_draw_str_aligned(
        canvas,
        64,
        48,
        AlignCenter,
        AlignCenter,
        model->running ? (model->hopping ? "HOPPING" : "TRANSMITTING") :
                          (model->hopping ? "Idle (hop armed)" : "Idle"));
    if(!model->running) {
        elements_button_left(canvas, "Freq");
        elements_button_right(canvas, "Freq");
        elements_button_center(canvas, "Start");
    } else {
        elements_button_center(canvas, "Stop");
    }
}

static bool generator_input_callback(InputEvent* event, void* ctx) {
    App* app = ctx;
    View* view = app->generator_view;
    bool handled = true;
    with_view_model(
        view,
        GeneratorModel * model,
        {
            if(event->type == InputTypeShort || event->type == InputTypeRepeat) {
                if(event->key == InputKeyLeft && !model->running) {
                    model->freq_khz = model->freq_khz > IR_TOOLS_MIN_KHZ + IR_TOOLS_STEP_KHZ - 1U ?
                                           model->freq_khz - IR_TOOLS_STEP_KHZ :
                                           IR_TOOLS_MIN_KHZ;
                } else if(event->key == InputKeyRight && !model->running) {
                    model->freq_khz = model->freq_khz + IR_TOOLS_STEP_KHZ <= IR_TOOLS_MAX_KHZ ?
                                           model->freq_khz + IR_TOOLS_STEP_KHZ :
                                           IR_TOOLS_MAX_KHZ;
                } else if(event->key == InputKeyUp && !model->running) {
                    model->tx_pin = model->tx_pin == FuriHalInfraredTxPinInternal ?
                                         FuriHalInfraredTxPinExtPA7 :
                                         FuriHalInfraredTxPinInternal;
                } else if(event->key == InputKeyDown && !model->running) {
                    model->hopping = !model->hopping;
                } else if(event->key == InputKeyOk) {
                    if(model->running)
                        generator_stop(app, model);
                    else
                        generator_start(app, model);
                } else {
                    handled = false;
                }
            } else {
                handled = false;
            }
        },
        true);
    return handled;
}

static void generator_view_exit_callback(void* ctx) {
    App* app = ctx;
    with_view_model(
        app->generator_view, GeneratorModel * model, { generator_stop(app, model); }, false);
}

// ---------------------------------------------------------------------
// Searcher
// ---------------------------------------------------------------------

static void searcher_received_signal_callback(void* context, InfraredWorkerSignal* signal) {
    App* app = context;
    if(!infrared_worker_signal_is_decoded(signal)) return;
    const InfraredMessage* message = infrared_worker_get_decoded_signal(signal);
    if(!message) return;
    app->pending_khz = infrared_get_protocol_frequency(message->protocol) / 1000U;
    strlcpy(
        app->pending_protocol,
        infrared_get_protocol_name(message->protocol),
        sizeof(app->pending_protocol));
    view_dispatcher_send_custom_event(app->dispatcher, EventSignalFound);
}

static void searcher_start(App* app, SearcherModel* model) {
    if(model->state == SearchScanning) return;
    furi_hal_infrared_set_rx_input(model->rx_pin);
    infrared_worker_rx_enable_signal_decoding(app->worker, true);
    infrared_worker_rx_enable_blink_on_receiving(app->worker, true);
    infrared_worker_rx_set_received_signal_callback(
        app->worker, searcher_received_signal_callback, app);
    infrared_worker_rx_start(app->worker);
    model->state = SearchScanning;
}

static void searcher_stop(App* app, SearcherModel* model) {
    if(model->state != SearchScanning) return;
    infrared_worker_rx_stop(app->worker);
    model->state = SearchIdle;
}

static void searcher_draw_callback(Canvas* canvas, void* ctx) {
    SearcherModel* model = ctx;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 8, AlignCenter, AlignCenter, "Signal Searcher");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(
        canvas,
        64,
        20,
        AlignCenter,
        AlignCenter,
        model->rx_pin == FuriHalInfraredRxPinExternal ? "Pin: External" : "Pin: Internal");

    if(model->state == SearchIdle) {
        canvas_draw_str_aligned(canvas, 64, 34, AlignCenter, AlignCenter, "Press OK to start");
        elements_button_center(canvas, "Start");
    } else if(model->state == SearchScanning) {
        canvas_draw_str_aligned(canvas, 64, 34, AlignCenter, AlignCenter, "Scanning...");
        canvas_draw_str_aligned(
            canvas, 64, 44, AlignCenter, AlignCenter, "Point a remote at the receiver");
        elements_button_center(canvas, "Stop");
    } else {
        char line[32];
        snprintf(line, sizeof(line), "Found: %lu kHz", (unsigned long)model->found_khz);
        canvas_set_font(canvas, FontBigNumbers);
        canvas_draw_str_aligned(canvas, 64, 32, AlignCenter, AlignCenter, line);
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 44, AlignCenter, AlignCenter, model->found_protocol);
        elements_button_center(canvas, "Scan again");
    }
    if(model->state != SearchScanning) {
        elements_button_left(canvas, "Pin");
        elements_button_right(canvas, "Pin");
    }
}

static bool searcher_input_callback(InputEvent* event, void* ctx) {
    App* app = ctx;
    View* view = app->searcher_view;
    bool handled = true;
    if(event->type != InputTypeShort) return false;
    with_view_model(
        view,
        SearcherModel * model,
        {
            if(event->key == InputKeyOk) {
                if(model->state == SearchScanning)
                    searcher_stop(app, model);
                else if(model->state == SearchFound)
                    model->state = SearchIdle;
                else
                    searcher_start(app, model);
            } else if(event->key == InputKeyLeft && model->state != SearchScanning) {
                model->rx_pin = FuriHalInfraredRxPinInternal;
            } else if(event->key == InputKeyRight && model->state != SearchScanning) {
                model->rx_pin = FuriHalInfraredRxPinExternal;
            } else {
                handled = false;
            }
        },
        true);
    return handled;
}

static void searcher_view_exit_callback(void* ctx) {
    App* app = ctx;
    with_view_model(app->searcher_view, SearcherModel * model, { searcher_stop(app, model); }, false);
}

// ---------------------------------------------------------------------
// Menu / dispatcher plumbing
// ---------------------------------------------------------------------

static void menu_callback(void* context, uint32_t index) {
    App* app = context;
    app->current_view = index == MenuItemGenerator ? ViewIdGenerator : ViewIdSearcher;
    view_dispatcher_switch_to_view(app->dispatcher, app->current_view);
}

static bool custom_event_callback(void* context, uint32_t event) {
    App* app = context;
    if(event == EventSignalFound) {
        with_view_model(
            app->searcher_view,
            SearcherModel * model,
            {
                model->found_khz = app->pending_khz;
                strlcpy(model->found_protocol, app->pending_protocol, sizeof(model->found_protocol));
                model->state = SearchFound;
            },
            true);
        return true;
    }
    return false;
}

static bool navigation_event_callback(void* context) {
    App* app = context;
    if(app->current_view != ViewIdMenu) {
        app->current_view = ViewIdMenu;
        view_dispatcher_switch_to_view(app->dispatcher, ViewIdMenu);
        return true;
    }
    view_dispatcher_stop(app->dispatcher);
    return true;
}

static App* app_alloc(void) {
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(*app));

    app->gui = furi_record_open(RECORD_GUI);
    app->dispatcher = view_dispatcher_alloc();
    app->menu = submenu_alloc();
    app->worker = infrared_worker_alloc();
    app->hop_timer = furi_timer_alloc(generator_hop_tick, FuriTimerTypePeriodic, app);

    submenu_set_header(app->menu, "IR Tools");
    submenu_add_item(app->menu, "Generator", MenuItemGenerator, menu_callback, app);
    submenu_add_item(app->menu, "Signal Searcher", MenuItemSearcher, menu_callback, app);

    app->generator_view = view_alloc();
    view_allocate_model(app->generator_view, ViewModelTypeLocking, sizeof(GeneratorModel));
    with_view_model(
        app->generator_view,
        GeneratorModel * model,
        {
            model->freq_khz = IR_TOOLS_DEFAULT_KHZ;
            model->tx_pin = FuriHalInfraredTxPinInternal;
            model->running = false;
            model->hopping = false;
        },
        false);
    view_set_context(app->generator_view, app);
    view_set_draw_callback(app->generator_view, generator_draw_callback);
    view_set_input_callback(app->generator_view, generator_input_callback);
    view_set_exit_callback(app->generator_view, generator_view_exit_callback);

    app->searcher_view = view_alloc();
    view_allocate_model(app->searcher_view, ViewModelTypeLocking, sizeof(SearcherModel));
    with_view_model(
        app->searcher_view,
        SearcherModel * model,
        {
            model->state = SearchIdle;
            model->rx_pin = FuriHalInfraredRxPinInternal;
        },
        false);
    view_set_context(app->searcher_view, app);
    view_set_draw_callback(app->searcher_view, searcher_draw_callback);
    view_set_input_callback(app->searcher_view, searcher_input_callback);
    view_set_exit_callback(app->searcher_view, searcher_view_exit_callback);

    view_dispatcher_set_event_callback_context(app->dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->dispatcher, custom_event_callback);
    view_dispatcher_set_navigation_event_callback(app->dispatcher, navigation_event_callback);
    view_dispatcher_add_view(app->dispatcher, ViewIdMenu, submenu_get_view(app->menu));
    view_dispatcher_add_view(app->dispatcher, ViewIdGenerator, app->generator_view);
    view_dispatcher_add_view(app->dispatcher, ViewIdSearcher, app->searcher_view);
    view_dispatcher_attach_to_gui(app->dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    app->current_view = ViewIdMenu;
    view_dispatcher_switch_to_view(app->dispatcher, ViewIdMenu);

    return app;
}

static void app_free(App* app) {
    view_dispatcher_remove_view(app->dispatcher, ViewIdMenu);
    view_dispatcher_remove_view(app->dispatcher, ViewIdGenerator);
    view_dispatcher_remove_view(app->dispatcher, ViewIdSearcher);
    view_free(app->generator_view);
    view_free(app->searcher_view);
    submenu_free(app->menu);
    view_dispatcher_free(app->dispatcher);
    infrared_worker_free(app->worker);
    furi_timer_free(app->hop_timer);
    furi_record_close(RECORD_GUI);
    free(app);
}

int32_t ir_tools_app(void* p) {
    UNUSED(p);
    App* app = app_alloc();
    view_dispatcher_run(app->dispatcher);
    app_free(app);
    return 0;
}
