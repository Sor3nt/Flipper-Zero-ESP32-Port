#include "subghz_jammer.h"
#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_region.h>
#include <gui/elements.h>
#include <lib/subghz/subghz_tx_rx_worker.h>
#include <subghz/devices/devices.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

#define jammer_sinf(x)  __builtin_sinf(x)
#define jammer_cosf(x)  __builtin_cosf(x)
#define jammer_sqrtf(x) __builtin_sqrtf(x)
#define jammer_logf(x)  __builtin_logf(x)

#define TAG "SubGhzJammer"
#define JAMMER_FREQUENCY_MIN 300000000
#define JAMMER_FREQUENCY_MAX 928000000
#define JAMMER_MESSAGE_MAX_LEN 1024
#define JAMMER_NUM_MODES 14

typedef enum {
    JammerModeOok650Async,
    JammerMode2FSKDev238Async,
    JammerMode2FSKDev476Async,
    JammerModeMSK99_97KbAsync,
    JammerModeGFSK9_99KbAsync,
    JammerModeBruteforce,
    JammerModeSineWave,
    JammerModeSquareWave,
    JammerModeSawtoothWave,
    JammerModeWhiteNoise,
    JammerModeTriangleWave,
    JammerModeChirp,
    JammerModeGaussianNoise,
    JammerModeBurst,
} JammerMode;

static const char* jammer_mode_names[] = {
    "OOK 650kHz",
    "2FSK 2.38kHz",
    "2FSK 47.6kHz",
    "MSK 99.97Kb/s",
    "GFSK 9.99Kb/s",
    "Bruteforce 0xFF",
    "Sine Wave",
    "Square Wave",
    "Sawtooth Wave",
    "White Noise",
    "Triangle Wave",
    "Chirp Signal",
    "Gaussian Noise",
    "Burst Mode",
};

typedef struct {
    uint32_t min;
    uint32_t max;
} JammerFrequencyBand;

static const JammerFrequencyBand jammer_valid_bands[] = {
    {300000000, 348000000},
    {387000000, 464000000},
    {779000000, 928000000},
};

#define JAMMER_NUM_BANDS (sizeof(jammer_valid_bands) / sizeof(jammer_valid_bands[0]))

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
    uint8_t cursor_position;
    int jamming_mode;
    bool is_running;
} SubGhzJammerModel;

struct SubGhzJammer {
    View* view;
    SubGhzJammerCallback callback;
    void* context;

    const SubGhzDevice* device;
    SubGhzTxRxWorker* subghz_txrx;
    FuriThread* tx_thread;
    bool tx_running;
};

static bool jammer_is_frequency_valid(uint32_t frequency) {
    for(size_t i = 0; i < JAMMER_NUM_BANDS; i++) {
        if(frequency >= jammer_valid_bands[i].min && frequency <= jammer_valid_bands[i].max) {
            return true;
        }
    }
    return false;
}

static uint32_t jammer_adjust_frequency_to_valid(uint32_t frequency, bool up) {
    if(jammer_is_frequency_valid(frequency)) {
        return frequency;
    }
    if(up) {
        for(size_t i = 0; i < JAMMER_NUM_BANDS; i++) {
            if(frequency < jammer_valid_bands[i].min) {
                return jammer_valid_bands[i].min;
            }
        }
        return jammer_valid_bands[0].min;
    } else {
        for(int i = JAMMER_NUM_BANDS - 1; i >= 0; i--) {
            if(frequency > jammer_valid_bands[i].max) {
                return jammer_valid_bands[i].max;
            }
        }
        return jammer_valid_bands[JAMMER_NUM_BANDS - 1].max;
    }
}

static int32_t jammer_tx_thread_callback(void* context) {
    SubGhzJammer* instance = context;
    uint8_t jam_data[JAMMER_MESSAGE_MAX_LEN];

    int mode;
    with_view_model(
        instance->view,
        SubGhzJammerModel * model,
        { mode = model->jamming_mode; },
        false);

    FURI_LOG_I(TAG, "TX Thread started with mode %d", mode);

    switch(mode) {
    case JammerModeOok650Async:
        memset(jam_data, 0xFF, sizeof(jam_data));
        break;
    case JammerMode2FSKDev238Async:
    case JammerMode2FSKDev476Async:
        for(size_t i = 0; i < sizeof(jam_data); i++) {
            jam_data[i] = (i % 2 == 0) ? 0xAA : 0x55;
        }
        break;
    case JammerModeMSK99_97KbAsync:
    case JammerModeGFSK9_99KbAsync:
        for(size_t i = 0; i < sizeof(jam_data); i++) {
            jam_data[i] = rand() % 256;
        }
        break;
    case JammerModeBruteforce:
        memset(jam_data, 0xFF, sizeof(jam_data));
        break;
    case JammerModeSineWave:
        for(size_t i = 0; i < sizeof(jam_data); i++) {
            jam_data[i] = (uint8_t)(127 * jammer_sinf(2 * M_PI * i / sizeof(jam_data)) + 128);
        }
        break;
    case JammerModeSquareWave:
        for(size_t i = 0; i < sizeof(jam_data); i++) {
            jam_data[i] = (i % 2 == 0) ? 0xFF : 0x00;
        }
        break;
    case JammerModeSawtoothWave:
        for(size_t i = 0; i < sizeof(jam_data); i++) {
            jam_data[i] = (uint8_t)(255 * i / sizeof(jam_data));
        }
        break;
    case JammerModeWhiteNoise:
        for(size_t i = 0; i < sizeof(jam_data); i++) {
            jam_data[i] = rand() % 256;
        }
        break;
    case JammerModeTriangleWave:
        for(size_t i = 0; i < sizeof(jam_data); i++) {
            jam_data[i] = (i < sizeof(jam_data) / 2)
                              ? (i * 255 / (sizeof(jam_data) / 2))
                              : (255 - (i * 255 / (sizeof(jam_data) / 2)));
        }
        break;
    case JammerModeChirp:
        for(size_t i = 0; i < sizeof(jam_data); i++) {
            jam_data[i] =
                (uint8_t)(127 * jammer_sinf(2 * M_PI * i * (1 + (float)i / sizeof(jam_data))));
        }
        break;
    case JammerModeGaussianNoise:
        for(size_t i = 0; i < sizeof(jam_data); i++) {
            float u1 = (float)rand() / RAND_MAX;
            float u2 = (float)rand() / RAND_MAX;
            if(u1 < 1e-10f) u1 = 1e-10f;
            float gaussian_noise = jammer_sqrtf(-2.0f * jammer_logf(u1)) * jammer_cosf(2 * M_PI * u2);
            jam_data[i] = (uint8_t)(127 * gaussian_noise + 128);
        }
        break;
    case JammerModeBurst:
        for(size_t i = 0; i < sizeof(jam_data); i++) {
            jam_data[i] = (i % 10 == 0) ? 0xFF : 0x00;
        }
        break;
    }

    while(instance->tx_running) {
        if(instance->subghz_txrx) {
            if(!subghz_tx_rx_worker_write(instance->subghz_txrx, jam_data, sizeof(jam_data))) {
                furi_delay_ms(20);
            }
        } else {
            break;
        }
        furi_delay_ms(10);
    }

    FURI_LOG_I(TAG, "TX Thread exiting");
    return 0;
}

static void jammer_stop_tx(SubGhzJammer* instance) {
    if(instance->tx_running) {
        instance->tx_running = false;
        if(instance->tx_thread) {
            furi_thread_join(instance->tx_thread);
            furi_thread_free(instance->tx_thread);
            instance->tx_thread = NULL;
        }
    }
}

static void jammer_start_tx(SubGhzJammer* instance) {
    instance->tx_running = true;
    instance->tx_thread = furi_thread_alloc();
    furi_thread_set_name(instance->tx_thread, "JammerTX");
    furi_thread_set_stack_size(instance->tx_thread, 2048);
    furi_thread_set_context(instance->tx_thread, instance);
    furi_thread_set_callback(instance->tx_thread, jammer_tx_thread_callback);
    furi_thread_start(instance->tx_thread);
}

static void jammer_load_preset_for_mode(const SubGhzDevice* device, int mode) {
    switch(mode) {
    case JammerModeOok650Async:
        subghz_devices_load_preset(device, FuriHalSubGhzPresetOok650Async, NULL);
        break;
    case JammerMode2FSKDev238Async:
        subghz_devices_load_preset(device, FuriHalSubGhzPreset2FSKDev238Async, NULL);
        break;
    case JammerMode2FSKDev476Async:
        subghz_devices_load_preset(device, FuriHalSubGhzPreset2FSKDev476Async, NULL);
        break;
    case JammerModeMSK99_97KbAsync:
        subghz_devices_load_preset(device, FuriHalSubGhzPresetMSK99_97KbAsync, NULL);
        break;
    case JammerModeGFSK9_99KbAsync:
        subghz_devices_load_preset(device, FuriHalSubGhzPresetGFSK9_99KbAsync, NULL);
        break;
    case JammerModeBruteforce:
        subghz_devices_load_preset(device, FuriHalSubGhzPresetOok650Async, NULL);
        break;
    default:
        break;
    }
}

static void jammer_switch_mode(SubGhzJammer* instance) {
    jammer_stop_tx(instance);

    if(instance->subghz_txrx && subghz_tx_rx_worker_is_running(instance->subghz_txrx)) {
        subghz_tx_rx_worker_stop(instance->subghz_txrx);
    }

    if(!instance->device) return;

    subghz_devices_reset(instance->device);
    subghz_devices_idle(instance->device);

    int new_mode;
    uint32_t frequency;
    with_view_model(
        instance->view,
        SubGhzJammerModel * model,
        {
            model->jamming_mode = (model->jamming_mode + 1) % JAMMER_NUM_MODES;
            new_mode = model->jamming_mode;
            frequency = model->frequency;
        },
        true);

    jammer_load_preset_for_mode(instance->device, new_mode);

    if(instance->subghz_txrx) {
        if(subghz_tx_rx_worker_start(instance->subghz_txrx, instance->device, frequency)) {
            jammer_start_tx(instance);
        }
    }
}

static void jammer_adjust_frequency(SubGhzJammer* instance, bool up) {
    uint32_t frequency;
    uint8_t cursor_position;

    with_view_model(
        instance->view,
        SubGhzJammerModel * model,
        {
            frequency = model->frequency;
            cursor_position = model->cursor_position;
        },
        false);

    uint32_t step;
    switch(cursor_position) {
    case 0:
        step = 100000000;
        break;
    case 1:
        step = 10000000;
        break;
    case 2:
        step = 1000000;
        break;
    case 3:
        step = 100000;
        break;
    case 4:
        step = 10000;
        break;
    default:
        return;
    }

    frequency = up ? frequency + step : frequency - step;

    if(frequency > JAMMER_FREQUENCY_MAX) {
        frequency = JAMMER_FREQUENCY_MIN;
    } else if(frequency < JAMMER_FREQUENCY_MIN) {
        frequency = JAMMER_FREQUENCY_MAX;
    }

    frequency = jammer_adjust_frequency_to_valid(frequency, up);

    with_view_model(
        instance->view, SubGhzJammerModel * model, { model->frequency = frequency; }, true);

    if(instance->tx_running && instance->subghz_txrx && instance->device) {
        subghz_tx_rx_worker_stop(instance->subghz_txrx);
        if(!subghz_tx_rx_worker_start(instance->subghz_txrx, instance->device, frequency)) {
            FURI_LOG_E(TAG, "Failed to restart TX RX worker");
        }
    }
}

static void subghz_jammer_draw_callback(Canvas* canvas, void* _model) {
    SubGhzJammerModel* model = _model;
    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 0, AlignCenter, AlignTop, "RF Jammer");

    char freq_str[20];
    snprintf(
        freq_str,
        sizeof(freq_str),
        "%3lu.%02lu",
        model->frequency / 1000000,
        (model->frequency % 1000000) / 10000);

    int total_width = strlen(freq_str) * 12;
    int start_x = (128 - total_width) / 2;
    int digit_position = 0;

    for(size_t i = 0; i < strlen(freq_str); i++) {
        bool highlight = (digit_position == model->cursor_position);

        if(freq_str[i] != '.') {
            canvas_set_font(canvas, highlight ? FontBigNumbers : FontPrimary);
            char temp[2] = {freq_str[i], '\0'};
            canvas_draw_str_aligned(canvas, start_x + (i * 12), 18, AlignCenter, AlignTop, temp);
            digit_position++;
        } else {
            canvas_set_font(canvas, FontPrimary);
            char temp[2] = {freq_str[i], '\0'};
            canvas_draw_str_aligned(canvas, start_x + (i * 12), 18, AlignCenter, AlignTop, temp);
        }
    }

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 45, AlignCenter, AlignTop, "MHz");

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(
        canvas, 64, 55, AlignCenter, AlignTop, jammer_mode_names[model->jamming_mode]);
}

static bool subghz_jammer_input_callback(InputEvent* event, void* context) {
    SubGhzJammer* instance = context;
    furi_assert(instance);

    if(event->type == InputTypeShort) {
        switch(event->key) {
        case InputKeyBack:
            if(instance->callback) {
                instance->callback(SubGhzCustomEventViewReceiverBack, instance->context);
            }
            return true;
        case InputKeyOk:
            jammer_switch_mode(instance);
            return true;
        case InputKeyLeft:
            with_view_model(
                instance->view,
                SubGhzJammerModel * model,
                {
                    if(model->cursor_position > 0) model->cursor_position--;
                },
                true);
            return true;
        case InputKeyRight:
            with_view_model(
                instance->view,
                SubGhzJammerModel * model,
                {
                    if(model->cursor_position < 4) model->cursor_position++;
                },
                true);
            return true;
        case InputKeyUp:
            jammer_adjust_frequency(instance, true);
            return true;
        case InputKeyDown:
            jammer_adjust_frequency(instance, false);
            return true;
        default:
            break;
        }
    }
    return false;
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
            model->frequency = 315000000;
            model->cursor_position = 0;
            model->jamming_mode = JammerModeOok650Async;
            model->is_running = false;
        },
        true);

    instance->callback = NULL;
    instance->context = NULL;
    instance->device = NULL;
    instance->subghz_txrx = NULL;
    instance->tx_thread = NULL;
    instance->tx_running = false;

    return instance;
}

void subghz_jammer_free(SubGhzJammer* instance) {
    furi_assert(instance);

    subghz_jammer_stop(instance);

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

void subghz_jammer_start(SubGhzJammer* instance) {
    furi_assert(instance);

    furi_hal_region_set(&jammer_unlocked_region);

    // Devices already initialized by SubGhz app's subghz_txrx_alloc()
    // Just get the device by name
    instance->device = subghz_devices_get_by_name("cc1101_int");
    if(!instance->device) {
        FURI_LOG_E(TAG, "Failed to get radio device");
        return;
    }

    subghz_devices_reset(instance->device);
    subghz_devices_idle(instance->device);

    int mode;
    uint32_t frequency;
    with_view_model(
        instance->view,
        SubGhzJammerModel * model,
        {
            mode = model->jamming_mode;
            frequency = model->frequency;
            model->is_running = true;
        },
        true);

    jammer_load_preset_for_mode(instance->device, mode);

    instance->subghz_txrx = subghz_tx_rx_worker_alloc();
    if(subghz_tx_rx_worker_start(instance->subghz_txrx, instance->device, frequency)) {
        FURI_LOG_I(TAG, "Jammer started on %lu Hz", frequency);
        jammer_start_tx(instance);
    } else {
        FURI_LOG_E(TAG, "Failed to start TX RX worker");
    }
}

void subghz_jammer_stop(SubGhzJammer* instance) {
    furi_assert(instance);

    jammer_stop_tx(instance);

    if(instance->subghz_txrx) {
        if(subghz_tx_rx_worker_is_running(instance->subghz_txrx)) {
            subghz_tx_rx_worker_stop(instance->subghz_txrx);
        }
        subghz_tx_rx_worker_free(instance->subghz_txrx);
        instance->subghz_txrx = NULL;
    }

    if(instance->device) {
        subghz_devices_idle(instance->device);
        instance->device = NULL;
    }

    // Don't call subghz_devices_deinit() - managed by SubGhz app

    with_view_model(
        instance->view, SubGhzJammerModel * model, { model->is_running = false; }, true);

    FURI_LOG_I(TAG, "Jammer stopped");
}
