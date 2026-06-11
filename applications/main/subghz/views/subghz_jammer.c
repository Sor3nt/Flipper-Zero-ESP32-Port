#include "subghz_jammer.h"
#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_region.h>
#include <gui/elements.h>
#include <toolbox/level_duration.h>
#include <subghz/devices/devices.h>

#define TAG "SubGhzJammer"

/* Decorative scrolling activity graph (no real RSSI — CC1101 is half-duplex
 * and busy transmitting noise while jamming). */
#define GRAPH_WIDTH  128
#define GRAPH_TOP    27
#define GRAPH_BOTTOM 49
#define GRAPH_HEIGHT (GRAPH_BOTTOM - GRAPH_TOP)
#define GRAPH_TICK_MS 60

/* TX is allowed on the full Sub-GHz range so any Read-Mode frequency works. */
static FuriHalRegion jammer_unlocked_region = {
    .country_code = "FTW",
    .bands_count = 3,
    .bands =
        {
            {.start = 299999755, .end = 348000000, .power_limit = 20, .duty_cycle = 50},
            {.start = 386999938, .end = 464000000, .power_limit = 20, .duty_cycle = 50},
            {.start = 778999847, .end = 928000000, .power_limit = 20, .duty_cycle = 50},
        },
};

typedef struct {
    uint32_t frequency;
    bool is_running;
    uint8_t graph[GRAPH_WIDTH]; // 0..GRAPH_HEIGHT, scrolls left
} SubGhzJammerModel;

struct SubGhzJammer {
    View* view;
    SubGhzJammerCallback callback;
    void* context;

    SubGhzSetting* setting; // Read-Mode frequency list (owned by SubGhz app)
    size_t freq_index; // index into the setting frequency list

    const SubGhzDevice* device;
    volatile bool tx_running; // read from the async-TX callback (ISR/timer context)
    volatile bool async_level; // current OOK line state inside the callback
    uint32_t async_rng; // xorshift state (no rand() — must stay ISR-safe)

    FuriTimer* graph_timer;
};

/* Continuous async-TX yield callback.
 *
 * Runs in timer/ISR context, so it must stay lightweight: no logging, no
 * malloc, no locks. It keeps toggling the OOK data line forever (until
 * tx_running clears), which holds the CC1101 in TX the whole time — unlike the
 * old packet worker, which framed ≤60-byte packets and dropped to idle/RX in
 * between (the gaps that were observed on the spectrum).
 *
 * The async-TX middleware merges equal consecutive levels and only emits a
 * sample on a transition, so a constant level would loop forever — we must
 * alternate. Highs dominate (long carrier-on, short off) for a near-continuous,
 * high-duty jam; durations are slightly randomized to spread the energy. */
static LevelDuration jammer_async_tx_yield(void* context) {
    SubGhzJammer* instance = context;
    if(!instance->tx_running) return level_duration_reset();

    uint32_t x = instance->async_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    instance->async_rng = x;

    instance->async_level = !instance->async_level;
    uint32_t duration = instance->async_level ? (400 + (x % 600)) : (20 + (x % 60));
    return level_duration_make(instance->async_level, duration);
}

/* Bring the radio up on the given frequency and start continuous noise TX. */
static void jammer_jam_start(SubGhzJammer* instance, uint32_t frequency) {
    if(!instance->device) return;

    subghz_devices_reset(instance->device);
    subghz_devices_idle(instance->device);
    subghz_devices_load_preset(instance->device, FuriHalSubGhzPresetOok650Async, NULL);
    subghz_devices_set_frequency(instance->device, frequency);

    instance->async_level = false;
    instance->async_rng = (frequency ^ furi_get_tick()) | 1u; // xorshift seed, never 0
    instance->tx_running = true;

    if(!subghz_devices_start_async_tx(instance->device, jammer_async_tx_yield, instance)) {
        instance->tx_running = false;
        subghz_devices_idle(instance->device);
        FURI_LOG_E(TAG, "start_async_tx failed");
    } else {
        FURI_LOG_I(TAG, "Jamming on %lu Hz", frequency);
    }
}

static void jammer_jam_stop(SubGhzJammer* instance) {
    if(!instance->device) return;
    if(instance->tx_running) {
        instance->tx_running = false; // callback yields reset on next call
        subghz_devices_stop_async_tx(instance->device);
    }
    subghz_devices_idle(instance->device);
}

static void jammer_toggle(SubGhzJammer* instance) {
    bool now_running;
    uint32_t frequency;
    with_view_model(
        instance->view,
        SubGhzJammerModel * model,
        {
            model->is_running = !model->is_running;
            now_running = model->is_running;
            frequency = model->frequency;
        },
        true);

    if(now_running) {
        jammer_jam_start(instance, frequency);
    } else {
        jammer_jam_stop(instance);
    }
}

/* Step through the Read-Mode frequency list; restart TX live if jamming. */
static void jammer_step_frequency(SubGhzJammer* instance, bool up) {
    if(!instance->setting) return;
    size_t count = subghz_setting_get_frequency_count(instance->setting);
    if(count == 0) return;

    if(up) {
        instance->freq_index = (instance->freq_index + 1) % count;
    } else {
        instance->freq_index = (instance->freq_index + count - 1) % count;
    }

    uint32_t frequency = subghz_setting_get_frequency(instance->setting, instance->freq_index);

    with_view_model(
        instance->view, SubGhzJammerModel * model, { model->frequency = frequency; }, true);

    if(instance->tx_running) {
        jammer_jam_stop(instance);
        jammer_jam_start(instance, frequency);
    }
}

static void jammer_graph_timer_callback(void* context) {
    SubGhzJammer* instance = context;
    with_view_model(
        instance->view,
        SubGhzJammerModel * model,
        {
            memmove(model->graph, model->graph + 1, GRAPH_WIDTH - 1);
            /* Constant level representing the (fixed) TX power while jamming. */
            model->graph[GRAPH_WIDTH - 1] = model->is_running ? GRAPH_HEIGHT : 0;
        },
        true);
}

static void subghz_jammer_draw_callback(Canvas* canvas, void* _model) {
    SubGhzJammerModel* model = _model;
    canvas_clear(canvas);

    /* Title */
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 0, AlignCenter, AlignTop, "SubGhz Jammer");
    canvas_draw_line(canvas, 0, 11, 127, 11);

    /* Current frequency */
    char freq_str[24];
    snprintf(
        freq_str,
        sizeof(freq_str),
        "%lu.%02lu MHz",
        model->frequency / 1000000,
        (model->frequency % 1000000) / 10000);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 14, AlignCenter, AlignTop, freq_str);

    /* Decorative activity graph */
    canvas_draw_line(canvas, 0, GRAPH_BOTTOM, 127, GRAPH_BOTTOM);
    for(int x = 0; x < GRAPH_WIDTH; x++) {
        int h = model->graph[x];
        if(h > 0) {
            if(h > GRAPH_HEIGHT) h = GRAPH_HEIGHT;
            canvas_draw_line(canvas, x, GRAPH_BOTTOM - h, x, GRAPH_BOTTOM);
        }
    }

    /* Bottom bar: real Flipper UI buttons (Left/Right = MHz, OK = Start/Stop) */
    elements_button_left(canvas, "-");
    elements_button_right(canvas, "+");
    elements_button_center(canvas, model->is_running ? "Stop" : "Start");
}

static bool subghz_jammer_input_callback(InputEvent* event, void* context) {
    SubGhzJammer* instance = context;
    furi_assert(instance);

    if(event->type != InputTypeShort) return false;

    switch(event->key) {
    case InputKeyBack:
        if(instance->callback) {
            instance->callback(SubGhzCustomEventViewReceiverBack, instance->context);
        }
        return true;
    case InputKeyOk:
        jammer_toggle(instance);
        return true;
    case InputKeyUp:
        /* "-" : step down the frequency list */
        jammer_step_frequency(instance, false);
        return true;
    case InputKeyDown:
        /* "+" : step up the frequency list */
        jammer_step_frequency(instance, true);
        return true;
    default:
        return false;
    }
}

SubGhzJammer* subghz_jammer_alloc(void) {
    SubGhzJammer* instance = malloc(sizeof(SubGhzJammer));

    instance->view = view_alloc();
    view_allocate_model(instance->view, ViewModelTypeLocking, sizeof(SubGhzJammerModel));
    view_set_context(instance->view, instance);
    view_set_draw_callback(instance->view, subghz_jammer_draw_callback);
    view_set_input_callback(instance->view, subghz_jammer_input_callback);

    with_view_model(
        instance->view,
        SubGhzJammerModel * model,
        {
            model->frequency = 433920000;
            model->is_running = false;
            memset(model->graph, 0, sizeof(model->graph));
        },
        true);

    instance->callback = NULL;
    instance->context = NULL;
    instance->setting = NULL;
    instance->freq_index = 0;
    instance->device = NULL;
    instance->tx_running = false;
    instance->async_level = false;
    instance->async_rng = 1u;
    instance->graph_timer =
        furi_timer_alloc(jammer_graph_timer_callback, FuriTimerTypePeriodic, instance);

    return instance;
}

void subghz_jammer_free(SubGhzJammer* instance) {
    furi_assert(instance);

    subghz_jammer_stop(instance);
    furi_timer_free(instance->graph_timer);
    view_free(instance->view);
    free(instance);
}

View* subghz_jammer_get_view(SubGhzJammer* instance) {
    furi_assert(instance);
    return instance->view;
}

void subghz_jammer_set_callback(
    SubGhzJammer* instance,
    SubGhzJammerCallback callback,
    void* context) {
    furi_assert(instance);
    instance->callback = callback;
    instance->context = context;
}

void subghz_jammer_set_setting(SubGhzJammer* instance, SubGhzSetting* setting) {
    furi_assert(instance);
    instance->setting = setting;
}

void subghz_jammer_start(SubGhzJammer* instance) {
    furi_assert(instance);

    furi_hal_region_set(&jammer_unlocked_region);

    /* Devices already initialized by SubGhz app's subghz_txrx_alloc(). */
    instance->device = subghz_devices_get_by_name("cc1101_int");
    if(!instance->device) {
        FURI_LOG_E(TAG, "Failed to get radio device");
        return;
    }

    /* Pick the default Read-Mode frequency as the starting point. */
    uint32_t frequency = 433920000;
    if(instance->setting) {
        size_t count = subghz_setting_get_frequency_count(instance->setting);
        if(count > 0) {
            instance->freq_index = subghz_setting_get_frequency_default_index(instance->setting);
            if(instance->freq_index >= count) instance->freq_index = 0;
            frequency = subghz_setting_get_frequency(instance->setting, instance->freq_index);
        }
    }

    with_view_model(
        instance->view,
        SubGhzJammerModel * model,
        {
            model->frequency = frequency;
            model->is_running = false;
            memset(model->graph, 0, sizeof(model->graph));
        },
        true);

    /* Graph animates the whole time the scene is open; jamming starts on OK. */
    furi_timer_start(instance->graph_timer, furi_ms_to_ticks(GRAPH_TICK_MS));
}

void subghz_jammer_stop(SubGhzJammer* instance) {
    furi_assert(instance);

    furi_timer_stop(instance->graph_timer);
    jammer_jam_stop(instance);

    if(instance->device) {
        subghz_devices_idle(instance->device);
        instance->device = NULL;
    }

    with_view_model(
        instance->view, SubGhzJammerModel * model, { model->is_running = false; }, true);

    FURI_LOG_I(TAG, "Jammer stopped");
}
