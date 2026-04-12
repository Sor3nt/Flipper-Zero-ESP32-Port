#include "subghz_playlist.h"
#include <furi.h>
#include <furi_hal.h>
#include <gui/elements.h>
#include <storage/storage.h>
#include <lib/toolbox/path.h>
#include <lib/subghz/protocols/protocol_items.h>
#include <lib/subghz/transmitter.h>
#include <lib/subghz/protocols/raw.h>
#include <subghz/devices/devices.h>
#include <flipper_format/flipper_format_i.h>
#include "flipper_format_stream.h"
#include "flipper_format_stream_i.h"

#define TAG "SubGhzPlaylist"
#define PLAYLIST_FOLDER "/ext/subplaylist"
#define PLAYLIST_WIDTH 128
#define PLAYLIST_HEIGHT 64

#define PLAYLIST_STATE_NONE 0
#define PLAYLIST_STATE_OVERVIEW 1
#define PLAYLIST_STATE_SENDING 2

typedef struct {
    int current_count;
    int total_count;
    int playlist_repetitions;
    int current_playlist_repetition;

    FuriString* prev_0_path;
    FuriString* prev_1_path;
    FuriString* prev_2_path;
    FuriString* prev_3_path;

    int state;
    bool ctl_pause;
} SubGhzPlaylistModel;

struct SubGhzPlaylist {
    View* view;
    SubGhzPlaylistCallback callback;
    void* context;

    FuriThread* worker_thread;
    FuriString* file_path;

    const SubGhzDevice* device;

    bool ctl_request_exit;
    bool ctl_pause;
    bool is_running;
};

// Canvas helpers (from playlist app)
static void playlist_draw_centered_boxed_str(
    Canvas* canvas,
    int x,
    int y,
    int height,
    int pad,
    const char* text) {
    int w = canvas_string_width(canvas, text);
    canvas_draw_rframe(canvas, x, y, w + pad, height, 2);
    canvas_draw_str_aligned(canvas, x + pad / 2, y + height / 2, AlignLeft, AlignCenter, text);
}

static void
    playlist_draw_corner_aligned(Canvas* canvas, int width, int height, Align horizontal, Align vertical) {
    canvas_set_color(canvas, ColorBlack);
    switch(horizontal) {
    case AlignLeft:
        switch(vertical) {
        case AlignTop:
            canvas_draw_rbox(canvas, 0, 0, width, height, 3);
            canvas_draw_box(canvas, 0, 0, width, 3);
            canvas_draw_box(canvas, 0, 0, 3, height);
            break;
        case AlignCenter:
            canvas_draw_rbox(canvas, 0, PLAYLIST_HEIGHT - height / 2, width, height, 3);
            canvas_draw_box(canvas, 0, PLAYLIST_HEIGHT - height / 2, 3, height);
            break;
        case AlignBottom:
            canvas_draw_rbox(canvas, 0, PLAYLIST_HEIGHT - height, width, height, 3);
            canvas_draw_box(canvas, 0, PLAYLIST_HEIGHT - height, 3, height);
            canvas_draw_box(canvas, 0, PLAYLIST_HEIGHT - 3, width, 3);
            break;
        default:
            break;
        }
        break;
    case AlignRight:
        switch(vertical) {
        case AlignTop:
            canvas_draw_rbox(canvas, PLAYLIST_WIDTH - width, 0, width, height, 3);
            canvas_draw_box(canvas, PLAYLIST_WIDTH - width, 0, width, 3);
            canvas_draw_box(canvas, PLAYLIST_WIDTH - 3, 0, 3, height);
            break;
        case AlignCenter:
            canvas_draw_rbox(
                canvas,
                PLAYLIST_WIDTH - width,
                PLAYLIST_HEIGHT / 2 - height / 2,
                width,
                height,
                3);
            canvas_draw_box(
                canvas, PLAYLIST_WIDTH - 3, PLAYLIST_HEIGHT / 2 - height / 2, 3, height);
            break;
        case AlignBottom:
            canvas_draw_rbox(canvas, PLAYLIST_WIDTH - width, PLAYLIST_HEIGHT - height, width, height, 3);
            canvas_draw_box(canvas, PLAYLIST_WIDTH - 3, PLAYLIST_HEIGHT - height, 3, height);
            canvas_draw_box(canvas, PLAYLIST_WIDTH - width, PLAYLIST_HEIGHT - 3, width, 3);
            break;
        default:
            break;
        }
        break;
    case AlignCenter:
        switch(vertical) {
        case AlignTop:
            canvas_draw_rbox(canvas, PLAYLIST_WIDTH / 2 - width / 2, 0, width, height, 3);
            canvas_draw_box(canvas, PLAYLIST_WIDTH / 2 - width / 2, 0, width, 3);
            canvas_draw_box(canvas, PLAYLIST_WIDTH / 2 - 3, 0, 3, height);
            break;
        case AlignCenter:
            canvas_draw_rbox(
                canvas,
                PLAYLIST_WIDTH / 2 - width / 2,
                PLAYLIST_HEIGHT / 2 - height / 2,
                width,
                height,
                3);
            canvas_draw_box(
                canvas, PLAYLIST_WIDTH / 2 - 3, PLAYLIST_HEIGHT / 2 - height / 2, 3, height);
            break;
        case AlignBottom:
            canvas_draw_rbox(
                canvas,
                PLAYLIST_WIDTH / 2 - width / 2,
                PLAYLIST_HEIGHT - height,
                width,
                height,
                3);
            canvas_draw_box(
                canvas, PLAYLIST_WIDTH / 2 - 3, PLAYLIST_HEIGHT - height, 3, height);
            canvas_draw_box(
                canvas, PLAYLIST_WIDTH / 2 - width / 2, PLAYLIST_HEIGHT - 3, width, 3);
            break;
        default:
            break;
        }
        break;
    default:
        break;
    }
}

static int playlist_count_items(Storage* storage, const char* file_path) {
    FlipperFormat* format = flipper_format_file_alloc(storage);
    if(!flipper_format_file_open_existing(format, file_path)) {
        flipper_format_free(format);
        return -1;
    }
    int count = 0;
    FuriString* data = furi_string_alloc();
    while(flipper_format_read_string(format, "sub", data)) {
        ++count;
    }
    flipper_format_file_close(format);
    flipper_format_free(format);
    furi_string_free(data);
    return count;
}

static FuriHalSubGhzPreset playlist_str_to_preset(FuriString* preset) {
    if(furi_string_cmp_str(preset, "FuriHalSubGhzPresetOok270Async") == 0) {
        return FuriHalSubGhzPresetOok270Async;
    }
    if(furi_string_cmp_str(preset, "FuriHalSubGhzPresetOok650Async") == 0) {
        return FuriHalSubGhzPresetOok650Async;
    }
    if(furi_string_cmp_str(preset, "FuriHalSubGhzPreset2FSKDev238Async") == 0) {
        return FuriHalSubGhzPreset2FSKDev238Async;
    }
    if(furi_string_cmp_str(preset, "FuriHalSubGhzPreset2FSKDev476Async") == 0) {
        return FuriHalSubGhzPreset2FSKDev476Async;
    }
    if(furi_string_cmp_str(preset, "FuriHalSubGhzPresetMSK99_97KbAsync") == 0) {
        return FuriHalSubGhzPresetMSK99_97KbAsync;
    }
    return FuriHalSubGhzPresetCustom;
}

// -4: missing protocol
// -3: missing preset
// -2: transmit error
// -1: error
// 0: ok
// 1: resend (paused)
// 2: exit requested
static int playlist_worker_process(
    SubGhzPlaylist* instance,
    FlipperFormat* fff_file,
    FlipperFormat* fff_data,
    const char* path,
    FuriString* preset,
    FuriString* protocol) {
    if(!flipper_format_file_open_existing(fff_file, path)) {
        FURI_LOG_E(TAG, "Failed to open %s", path);
        return -1;
    }

    uint32_t frequency = 0;
    if(!flipper_format_read_uint32(fff_file, "Frequency", &frequency, 1)) {
        FURI_LOG_W(TAG, "Missing Frequency, defaulting to 433.92MHz");
        frequency = 433920000;
    }
    if(!furi_hal_subghz_is_tx_allowed(frequency)) {
        return -2;
    }

    if(!flipper_format_read_string(fff_file, "Preset", preset)) {
        FURI_LOG_E(TAG, "Missing Preset");
        return -3;
    }

    if(!flipper_format_read_string(fff_file, "Protocol", protocol)) {
        FURI_LOG_E(TAG, "Missing Protocol");
        return -4;
    }

    if(!furi_string_cmp_str(protocol, "RAW")) {
        subghz_protocol_raw_gen_fff_data(fff_data, path, "cc1101_int");
    } else {
        stream_copy_full(
            flipper_format_get_raw_stream(fff_file), flipper_format_get_raw_stream(fff_data));
    }
    flipper_format_free(fff_file);

    SubGhzEnvironment* environment = subghz_environment_alloc();
    subghz_environment_set_protocol_registry(environment, (void*)&subghz_protocol_registry);
    SubGhzTransmitter* transmitter =
        subghz_transmitter_alloc_init(environment, furi_string_get_cstr(protocol));

    subghz_transmitter_deserialize(transmitter, fff_data);

    const SubGhzDevice* device = instance->device;
    if(!device) {
        subghz_transmitter_free(transmitter);
        return -1;
    }

    subghz_devices_reset(device);
    subghz_devices_idle(device);
    subghz_devices_load_preset(device, playlist_str_to_preset(preset), NULL);
    subghz_devices_set_frequency(device, frequency);

    int status = 0;
    subghz_devices_start_async_tx(device, subghz_transmitter_yield, transmitter);
    while(!subghz_devices_is_async_complete_tx(device)) {
        if(instance->ctl_request_exit) {
            status = 2;
            break;
        }
        if(instance->ctl_pause) {
            status = 1;
            break;
        }
        furi_delay_ms(50);
    }

    subghz_devices_stop_async_tx(device);
    subghz_devices_sleep(device);
    subghz_transmitter_free(transmitter);

    return status;
}

static bool playlist_worker_wait_pause(SubGhzPlaylist* instance) {
    while(instance->ctl_pause && !instance->ctl_request_exit) {
        furi_delay_ms(50);
    }
    if(instance->ctl_request_exit) {
        return false;
    }
    return true;
}

static void playlist_update_model_state(SubGhzPlaylist* instance, int state) {
    with_view_model(
        instance->view, SubGhzPlaylistModel * model, { model->state = state; }, true);
}

static bool playlist_worker_play_once(
    SubGhzPlaylist* instance,
    Storage* storage,
    FlipperFormat* fff_head,
    FlipperFormat* fff_data,
    FuriString* data,
    FuriString* preset,
    FuriString* protocol) {
    if(!flipper_format_rewind(fff_head)) {
        FURI_LOG_E(TAG, "Failed to rewind file");
        return false;
    }

    while(flipper_format_read_string(fff_head, "sub", data)) {
        if(!playlist_worker_wait_pause(instance)) {
            break;
        }

        playlist_update_model_state(instance, PLAYLIST_STATE_SENDING);

        const char* str = furi_string_get_cstr(data);

        with_view_model(
            instance->view,
            SubGhzPlaylistModel * model,
            {
                ++model->current_count;
                furi_string_set(model->prev_3_path, furi_string_get_cstr(model->prev_2_path));
                furi_string_set(model->prev_2_path, furi_string_get_cstr(model->prev_1_path));
                furi_string_set(model->prev_1_path, furi_string_get_cstr(model->prev_0_path));
                furi_string_set(model->prev_0_path, str);
            },
            true);

        for(int i = 0; i < 1; i++) {
            if(!playlist_worker_wait_pause(instance)) {
                break;
            }

            FlipperFormat* fff_file = flipper_format_file_alloc(storage);
            int status =
                playlist_worker_process(instance, fff_file, fff_data, str, preset, protocol);

            if(status < 0) {
                flipper_format_free(fff_file);
            }
            if(status == 1) {
                i -= 1;
            } else if(status < 0) {
                break;
            } else if(status == 2) {
                return false;
            }
        }
    }
    return true;
}

static int32_t playlist_worker_thread(void* ctx) {
    SubGhzPlaylist* instance = ctx;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* fff_head = flipper_format_file_alloc(storage);

    if(!flipper_format_file_open_existing(fff_head, furi_string_get_cstr(instance->file_path))) {
        FURI_LOG_E(TAG, "Failed to open %s", furi_string_get_cstr(instance->file_path));
        instance->is_running = false;
        flipper_format_free(fff_head);
        furi_record_close(RECORD_STORAGE);
        return 0;
    }

    playlist_worker_wait_pause(instance);
    FlipperFormat* fff_data = flipper_format_string_alloc();

    FuriString* data = furi_string_alloc();
    FuriString* preset = furi_string_alloc();
    FuriString* protocol = furi_string_alloc();

    int playlist_repetitions;
    with_view_model(
        instance->view,
        SubGhzPlaylistModel * model,
        { playlist_repetitions = model->playlist_repetitions; },
        false);

    for(int i = 0; i < MAX(1, playlist_repetitions); i++) {
        if(playlist_repetitions <= 0) {
            --i;
        }

        with_view_model(
            instance->view,
            SubGhzPlaylistModel * model,
            {
                ++model->current_playlist_repetition;
                model->current_count = 0;
            },
            true);

        if(instance->ctl_request_exit) {
            break;
        }

        if(!playlist_worker_play_once(
               instance, storage, fff_head, fff_data, data, preset, protocol)) {
            break;
        }
    }

    furi_record_close(RECORD_STORAGE);
    flipper_format_free(fff_head);
    furi_string_free(data);
    furi_string_free(preset);
    furi_string_free(protocol);
    flipper_format_free(fff_data);

    instance->is_running = false;
    playlist_update_model_state(instance, PLAYLIST_STATE_OVERVIEW);

    return 0;
}

static void subghz_playlist_draw_callback(Canvas* canvas, void* _model) {
    SubGhzPlaylistModel* model = _model;
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontSecondary);

    FuriString* temp_str = furi_string_alloc();

    switch(model->state) {
    case PLAYLIST_STATE_NONE:
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(
            canvas,
            PLAYLIST_WIDTH / 2,
            PLAYLIST_HEIGHT / 2,
            AlignCenter,
            AlignCenter,
            "No playlist loaded");
        break;

    case PLAYLIST_STATE_OVERVIEW:
        // File name
        path_extract_filename(model->prev_0_path, temp_str, true);
        canvas_set_font(canvas, FontPrimary);
        playlist_draw_centered_boxed_str(canvas, 1, 1, 15, 6, furi_string_get_cstr(temp_str));

        canvas_set_font(canvas, FontSecondary);

        // Item count
        furi_string_printf(temp_str, "%d Items in playlist", model->total_count);
        canvas_draw_str_aligned(
            canvas, 1, 19, AlignLeft, AlignTop, furi_string_get_cstr(temp_str));

        // Repetitions
        if(model->playlist_repetitions <= 0) {
            furi_string_set(temp_str, "Repeat: inf");
        } else if(model->playlist_repetitions == 1) {
            furi_string_set(temp_str, "Repeat: no");
        } else {
            furi_string_printf(temp_str, "Repeat: %dx", model->playlist_repetitions);
        }
        canvas_draw_str_aligned(
            canvas, 1, 29, AlignLeft, AlignTop, furi_string_get_cstr(temp_str));

        // Buttons
        playlist_draw_corner_aligned(canvas, 40, 15, AlignCenter, AlignBottom);
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_str_aligned(
            canvas, PLAYLIST_WIDTH / 2 - 7, PLAYLIST_HEIGHT - 11, AlignLeft, AlignTop, "Start");
        canvas_draw_disc(canvas, PLAYLIST_WIDTH / 2 - 14, PLAYLIST_HEIGHT - 8, 3);

        canvas_set_color(canvas, ColorBlack);
        playlist_draw_corner_aligned(canvas, 20, 15, AlignLeft, AlignBottom);
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_str_aligned(canvas, 4, PLAYLIST_HEIGHT - 11, AlignLeft, AlignTop, "R-");

        canvas_set_color(canvas, ColorBlack);
        playlist_draw_corner_aligned(canvas, 20, 15, AlignRight, AlignBottom);
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_str_aligned(
            canvas, PLAYLIST_WIDTH - 4, PLAYLIST_HEIGHT - 11, AlignRight, AlignTop, "R+");

        canvas_set_color(canvas, ColorBlack);
        break;

    case PLAYLIST_STATE_SENDING:
        canvas_set_color(canvas, ColorBlack);
        if(model->ctl_pause) {
            // Play icon (paused state)
            canvas_draw_line(canvas, 2, PLAYLIST_HEIGHT - 8, 2, PLAYLIST_HEIGHT - 2);
            canvas_draw_line(canvas, 2, PLAYLIST_HEIGHT - 8, 7, PLAYLIST_HEIGHT - 5);
            canvas_draw_line(canvas, 2, PLAYLIST_HEIGHT - 2, 7, PLAYLIST_HEIGHT - 5);
        } else {
            // Pause icon (playing state)
            canvas_draw_box(canvas, 2, PLAYLIST_HEIGHT - 8, 2, 7);
            canvas_draw_box(canvas, 5, PLAYLIST_HEIGHT - 8, 2, 7);
        }

        // Progress text
        {
            canvas_set_font(canvas, FontSecondary);
            furi_string_printf(
                temp_str, "[%d/%d]", model->current_count, model->total_count);
            canvas_draw_str_aligned(
                canvas, 11, PLAYLIST_HEIGHT - 8, AlignLeft, AlignTop, furi_string_get_cstr(temp_str));

            int h = canvas_string_width(canvas, furi_string_get_cstr(temp_str));
            int xs = 11 + h + 2;
            int w = PLAYLIST_WIDTH - xs - 1;
            canvas_draw_box(canvas, xs, PLAYLIST_HEIGHT - 5, w, 1);

            float progress =
                (float)model->current_count / (float)MAX(1, model->total_count);
            int wp = (int)(progress * w);
            canvas_draw_box(canvas, xs + wp - 1, PLAYLIST_HEIGHT - 7, 2, 5);
        }

        // Repetition counter
        {
            if(model->playlist_repetitions <= 0) {
                furi_string_printf(
                    temp_str, "[%d/Inf]", model->current_playlist_repetition);
            } else {
                furi_string_printf(
                    temp_str,
                    "[%d/%d]",
                    model->current_playlist_repetition,
                    model->playlist_repetitions);
            }
            canvas_set_color(canvas, ColorBlack);
            int w = canvas_string_width(canvas, furi_string_get_cstr(temp_str));
            playlist_draw_corner_aligned(canvas, w + 6, 13, AlignRight, AlignTop);
            canvas_set_color(canvas, ColorWhite);
            canvas_draw_str_aligned(
                canvas,
                PLAYLIST_WIDTH - 3,
                3,
                AlignRight,
                AlignTop,
                furi_string_get_cstr(temp_str));
        }

        // Current and last files
        {
            canvas_set_color(canvas, ColorBlack);
            canvas_set_font(canvas, FontSecondary);

            if(!furi_string_empty(model->prev_0_path)) {
                path_extract_filename(model->prev_0_path, temp_str, true);
                int w = canvas_string_width(canvas, furi_string_get_cstr(temp_str));
                canvas_set_color(canvas, ColorBlack);
                canvas_draw_rbox(canvas, 1, 1, w + 4, 12, 2);
                canvas_set_color(canvas, ColorWhite);
                canvas_draw_str_aligned(
                    canvas, 3, 3, AlignLeft, AlignTop, furi_string_get_cstr(temp_str));
            }

            canvas_set_color(canvas, ColorBlack);

            if(!furi_string_empty(model->prev_1_path)) {
                path_extract_filename(model->prev_1_path, temp_str, true);
                canvas_draw_str_aligned(
                    canvas, 3, 15, AlignLeft, AlignTop, furi_string_get_cstr(temp_str));
            }

            if(!furi_string_empty(model->prev_2_path)) {
                path_extract_filename(model->prev_2_path, temp_str, true);
                canvas_draw_str_aligned(
                    canvas, 3, 26, AlignLeft, AlignTop, furi_string_get_cstr(temp_str));
            }

            if(!furi_string_empty(model->prev_3_path)) {
                path_extract_filename(model->prev_3_path, temp_str, true);
                canvas_draw_str_aligned(
                    canvas, 3, 37, AlignLeft, AlignTop, furi_string_get_cstr(temp_str));
            }
        }
        break;
    }

    furi_string_free(temp_str);
}

static bool subghz_playlist_input_callback(InputEvent* event, void* context) {
    SubGhzPlaylist* instance = context;
    furi_assert(instance);

    if(event->type != InputTypeShort) return false;

    switch(event->key) {
    case InputKeyBack:
        if(instance->callback) {
            instance->callback(SubGhzCustomEventViewReceiverBack, instance->context);
        }
        return true;

    case InputKeyLeft: {
        int state;
        with_view_model(
            instance->view, SubGhzPlaylistModel * model, { state = model->state; }, false);
        if(state == PLAYLIST_STATE_OVERVIEW) {
            with_view_model(
                instance->view,
                SubGhzPlaylistModel * model,
                {
                    if(model->playlist_repetitions > 0) {
                        --model->playlist_repetitions;
                    }
                },
                true);
        }
        return true;
    }

    case InputKeyRight: {
        int state;
        with_view_model(
            instance->view, SubGhzPlaylistModel * model, { state = model->state; }, false);
        if(state == PLAYLIST_STATE_OVERVIEW) {
            with_view_model(
                instance->view,
                SubGhzPlaylistModel * model,
                { ++model->playlist_repetitions; },
                true);
        }
        return true;
    }

    case InputKeyOk:
        if(!instance->is_running) {
            // Start worker
            instance->ctl_pause = false;
            instance->ctl_request_exit = false;
            instance->is_running = true;

            with_view_model(
                instance->view,
                SubGhzPlaylistModel * model,
                {
                    model->current_count = 0;
                    model->current_playlist_repetition = 0;
                    furi_string_reset(model->prev_0_path);
                    furi_string_reset(model->prev_1_path);
                    furi_string_reset(model->prev_2_path);
                    furi_string_reset(model->prev_3_path);
                    model->ctl_pause = false;
                },
                true);

            instance->worker_thread = furi_thread_alloc();
            furi_thread_set_name(instance->worker_thread, "PlaylistWorker");
            furi_thread_set_stack_size(instance->worker_thread, 2048);
            furi_thread_set_context(instance->worker_thread, instance);
            furi_thread_set_callback(instance->worker_thread, playlist_worker_thread);
            furi_thread_start(instance->worker_thread);
        } else {
            // Toggle pause
            instance->ctl_pause = !instance->ctl_pause;
            with_view_model(
                instance->view,
                SubGhzPlaylistModel * model,
                { model->ctl_pause = instance->ctl_pause; },
                true);
        }
        return true;

    default:
        break;
    }

    return false;
}

SubGhzPlaylist* subghz_playlist_alloc(void) {
    SubGhzPlaylist* instance = malloc(sizeof(SubGhzPlaylist));

    instance->view = view_alloc();
    view_allocate_model(instance->view, ViewModelTypeLocking, sizeof(SubGhzPlaylistModel));
    view_set_context(instance->view, instance);
    view_set_draw_callback(instance->view, subghz_playlist_draw_callback);
    view_set_input_callback(instance->view, subghz_playlist_input_callback);
    view_set_input_mode(instance->view, ViewInputModeLeftRight);

    with_view_model(
        instance->view,
        SubGhzPlaylistModel * model,
        {
            model->current_count = 0;
            model->total_count = 0;
            model->playlist_repetitions = 1;
            model->current_playlist_repetition = 0;
            model->prev_0_path = furi_string_alloc();
            model->prev_1_path = furi_string_alloc();
            model->prev_2_path = furi_string_alloc();
            model->prev_3_path = furi_string_alloc();
            model->state = PLAYLIST_STATE_NONE;
            model->ctl_pause = false;
        },
        true);

    instance->callback = NULL;
    instance->context = NULL;
    instance->worker_thread = NULL;
    instance->file_path = furi_string_alloc();
    instance->ctl_request_exit = false;
    instance->ctl_pause = false;
    instance->is_running = false;

    return instance;
}

void subghz_playlist_free(SubGhzPlaylist* instance) {
    furi_assert(instance);

    subghz_playlist_stop(instance);

    with_view_model(
        instance->view,
        SubGhzPlaylistModel * model,
        {
            furi_string_free(model->prev_0_path);
            furi_string_free(model->prev_1_path);
            furi_string_free(model->prev_2_path);
            furi_string_free(model->prev_3_path);
        },
        false);

    view_free(instance->view);
    furi_string_free(instance->file_path);
    free(instance);
}

View* subghz_playlist_get_view(SubGhzPlaylist* instance) {
    furi_assert(instance);
    return instance->view;
}

void subghz_playlist_set_callback(
    SubGhzPlaylist* instance,
    SubGhzPlaylistCallback callback,
    void* context) {
    furi_assert(instance);
    instance->callback = callback;
    instance->context = context;
}

void subghz_playlist_set_file_path(SubGhzPlaylist* instance, const char* file_path) {
    furi_assert(instance);
    furi_string_set(instance->file_path, file_path);

    // Count items and set up overview
    Storage* storage = furi_record_open(RECORD_STORAGE);
    int count = playlist_count_items(storage, file_path);
    furi_record_close(RECORD_STORAGE);

    FuriString* filename = furi_string_alloc();
    furi_string_set(filename, file_path);

    with_view_model(
        instance->view,
        SubGhzPlaylistModel * model,
        {
            model->total_count = count;
            model->current_count = 0;
            model->current_playlist_repetition = 0;
            furi_string_set(model->prev_0_path, file_path);
            furi_string_reset(model->prev_1_path);
            furi_string_reset(model->prev_2_path);
            furi_string_reset(model->prev_3_path);
            model->state = PLAYLIST_STATE_OVERVIEW;
            model->ctl_pause = false;
        },
        true);

    furi_string_free(filename);
}

void subghz_playlist_start(SubGhzPlaylist* instance) {
    furi_assert(instance);
    // Create playlist folder if needed
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, PLAYLIST_FOLDER);
    furi_record_close(RECORD_STORAGE);

    // Get radio device
    instance->device = subghz_devices_get_by_name("cc1101_int");
    if(!instance->device) {
        FURI_LOG_E(TAG, "Failed to get radio device");
    }
}

void subghz_playlist_stop(SubGhzPlaylist* instance) {
    furi_assert(instance);

    if(instance->is_running) {
        instance->ctl_request_exit = true;
        if(instance->worker_thread) {
            furi_thread_join(instance->worker_thread);
            furi_thread_free(instance->worker_thread);
            instance->worker_thread = NULL;
        }
        instance->is_running = false;
    }

    if(instance->device) {
        subghz_devices_idle(instance->device);
        instance->device = NULL;
    }
}
