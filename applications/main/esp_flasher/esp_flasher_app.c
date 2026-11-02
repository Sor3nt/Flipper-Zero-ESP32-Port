#include "esp_flasher_app_i.h"
#include "esp_flasher_worker.h"

#include <furi.h>
#include <furi_hal.h>
#include <expansion/expansion.h>
#include <toolbox/saved_struct.h>
#include <esp_heap_caps.h>

#define TAG "EspFlasher"

/* Largest single console append; keeps the truncation math below STORE_SIZE / 2. */
#define ESP_FLASHER_CONSOLE_CHUNK_MAX (1024)

static const char* const esp_flasher_chip_names[ESP_MAX_CHIP] = {
    [ESP8266_CHIP] = "ESP8266",
    [ESP32_CHIP] = "ESP32",
    [ESP32S2_CHIP] = "ESP32-S2",
    [ESP32C3_CHIP] = "ESP32-C3",
    [ESP32S3_CHIP] = "ESP32-S3",
    [ESP32C2_CHIP] = "ESP32-C2",
    [ESP32C5_CHIP] = "ESP32-C5",
    [ESP32H2_CHIP] = "ESP32-H2",
    [ESP32C6_CHIP] = "ESP32-C6",
    [ESP32P4_CHIP] = "ESP32-P4",
};

const char* esp_flasher_chip_name(target_chip_t chip) {
    return (unsigned)chip < ESP_MAX_CHIP ? esp_flasher_chip_names[chip] : "unknown chip";
}

uint32_t esp_flasher_boot_offset(target_chip_t chip) {
    /* Matches esptool's BOOTLOADER_FLASH_OFFSET per target. */
    switch(chip) {
    case ESP32_CHIP:
    case ESP32S2_CHIP:
        return 0x1000;
    case ESP32C5_CHIP:
    case ESP32P4_CHIP:
        return 0x2000;
    default:
        return 0x0;
    }
}

uint32_t esp_flasher_fast_baud_value(EspFlasherFastBaud baud) {
    switch(baud) {
    case EspFlasherFastBaud460800:
        return 460800;
    case EspFlasherFastBaud230400:
        return 230400;
    default:
        return 921600;
    }
}

void esp_flasher_console_append(EspFlasherApp* app, const uint8_t* buf, size_t len) {
    furi_assert(app);
    if(!len) return;
    if(len > ESP_FLASHER_CONSOLE_CHUNK_MAX) {
        buf += len - ESP_FLASHER_CONSOLE_CHUNK_MAX;
        len = ESP_FLASHER_CONSOLE_CHUNK_MAX;
    }

    furi_check(furi_mutex_acquire(app->console_mutex, FuriWaitForever) == FuriStatusOk);
    const size_t size = furi_string_size(app->text_box_store);
    if(size + len > ESP_FLASHER_TEXT_BOX_STORE_SIZE - 1) {
        furi_string_right(app->text_box_store, size + len - ESP_FLASHER_TEXT_BOX_STORE_SIZE / 2);
    }
    furi_string_cat_printf(app->text_box_store, "%.*s", (int)len, (const char*)buf);
    const bool send_refresh = app->console_active && !app->console_refresh_pending;
    if(send_refresh) app->console_refresh_pending = true;
    furi_check(furi_mutex_release(app->console_mutex) == FuriStatusOk);

    /* One refresh in flight at most: a fast UART must never fill the event queue, since
     * view_dispatcher_send_custom_event() blocks forever when it is full. */
    if(send_refresh) {
        view_dispatcher_send_custom_event(
            app->view_dispatcher, EspFlasherEventRefreshConsoleOutput);
    }
}

void esp_flasher_console_clear(EspFlasherApp* app) {
    furi_check(furi_mutex_acquire(app->console_mutex, FuriWaitForever) == FuriStatusOk);
    furi_string_reset(app->text_box_store);
    furi_check(furi_mutex_release(app->console_mutex) == FuriStatusOk);
}

void esp_flasher_console_show(EspFlasherApp* app) {
    /* text_box_set_text() swaps the pointer under the view model lock, so once it returns
     * the previously shown buffer is no longer read by the GUI thread and can be refilled. */
    const uint8_t next = app->text_box_display_index ^ 1;
    furi_check(furi_mutex_acquire(app->console_mutex, FuriWaitForever) == FuriStatusOk);
    furi_string_set(app->text_box_display[next], app->text_box_store);
    furi_check(furi_mutex_release(app->console_mutex) == FuriStatusOk);
    text_box_set_text(app->text_box, furi_string_get_cstr(app->text_box_display[next]));
    app->text_box_display_index = next;
}

static void esp_flasher_app_load_settings(EspFlasherApp* app) {
    EspFlasherSettings settings = {
        .reset_mode = EspFlasherResetModeOff,
        .reserved = 0,
        .fast_baud = EspFlasherFastBaud921600,
    };
    EspFlasherSettings loaded;
    if(saved_struct_load(
           ESP_FLASHER_SETTINGS_PATH,
           &loaded,
           sizeof(loaded),
           ESP_FLASHER_SETTINGS_MAGIC,
           ESP_FLASHER_SETTINGS_VERSION)) {
        if(loaded.reset_mode < EspFlasherResetModeCount) settings.reset_mode = loaded.reset_mode;
        if(loaded.fast_baud < EspFlasherFastBaudCount) settings.fast_baud = loaded.fast_baud;
    }
    if(!esp_flasher_reset_available()) settings.reset_mode = EspFlasherResetModeOff;
    app->settings = settings;
}

void esp_flasher_app_save_settings(EspFlasherApp* app) {
    if(!saved_struct_save(
           ESP_FLASHER_SETTINGS_PATH,
           &app->settings,
           sizeof(app->settings),
           ESP_FLASHER_SETTINGS_MAGIC,
           ESP_FLASHER_SETTINGS_VERSION)) {
        FURI_LOG_W(TAG, "Settings not saved");
    }
}

static bool esp_flasher_app_custom_event_callback(void* context, uint32_t event) {
    furi_assert(context);
    EspFlasherApp* app = context;
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

static bool esp_flasher_app_back_event_callback(void* context) {
    furi_assert(context);
    EspFlasherApp* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

static void esp_flasher_app_tick_event_callback(void* context) {
    furi_assert(context);
    EspFlasherApp* app = context;
    scene_manager_handle_tick_event(app->scene_manager);
}

static EspFlasherApp* esp_flasher_app_alloc(void) {
    EspFlasherApp* app = malloc(sizeof(EspFlasherApp));
    furi_check(app);
    memset(app, 0, sizeof(EspFlasherApp));

    app->gui = furi_record_open(RECORD_GUI);
    app->dialogs = furi_record_open(RECORD_DIALOGS);
    app->storage = furi_record_open(RECORD_STORAGE);
    app->notification = furi_record_open(RECORD_NOTIFICATION);

    app->view_dispatcher = view_dispatcher_alloc();
    app->scene_manager = scene_manager_alloc(&esp_flasher_scene_handlers, app);

    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);

    view_dispatcher_set_custom_event_callback(
        app->view_dispatcher, esp_flasher_app_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, esp_flasher_app_back_event_callback);
    view_dispatcher_set_tick_event_callback(
        app->view_dispatcher, esp_flasher_app_tick_event_callback, 100);

    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    app->var_item_list = variable_item_list_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher,
        EspFlasherAppViewVarItemList,
        variable_item_list_get_view(app->var_item_list));

    app->text_box = text_box_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, EspFlasherAppViewConsoleOutput, text_box_get_view(app->text_box));
    app->text_box_store = furi_string_alloc();
    furi_string_reserve(app->text_box_store, ESP_FLASHER_TEXT_BOX_STORE_SIZE + 1);
    for(size_t i = 0; i < COUNT_OF(app->text_box_display); i++) {
        app->text_box_display[i] = furi_string_alloc();
        furi_string_reserve(app->text_box_display[i], ESP_FLASHER_TEXT_BOX_STORE_SIZE + 1);
    }
    app->console_mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    app->widget = widget_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, EspFlasherAppViewWidget, widget_get_view(app->widget));

    app->submenu = submenu_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, EspFlasherAppViewSubmenu, submenu_get_view(app->submenu));

    // Text Input (for custom flash offset entry)
    app->text_input = text_input_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, EspFlasherAppViewTextInput, text_input_get_view(app->text_input));

    app->expected_chip = ESP_UNKNOWN_CHIP;
    app->pending_addr_slot = -1;

    if(!storage_simply_mkdir(app->storage, ESP_APP_FOLDER)) {
        dialog_message_show_storage_error(app->dialogs, "Cannot create\napp folder");
    } else {
        // Show where Quick Flash looks for the C5 images
        static const char* const asset_dirs[] = {
            ESP_APP_FOLDER "/assets",
            ESP_APP_ASSETS("marauder"),
            ESP_APP_ASSETS("marauder/c5"),
            ESP_APP_ASSETS("custom"),
            ESP_APP_ASSETS("custom/c5"),
        };
        for(size_t i = 0; i < COUNT_OF(asset_dirs); i++) {
            storage_simply_mkdir(app->storage, asset_dirs[i]);
        }
    }
    esp_flasher_app_load_settings(app);

    scene_manager_next_scene(app->scene_manager, EspFlasherSceneStart);

    return app;
}

static void esp_flasher_app_free(EspFlasherApp* app) {
    furi_assert(app);

    /* The worker and the RX thread post to the view dispatcher and use the UART: stop both
     * before anything they touch goes away. */
    if(app->flash_worker) esp_flasher_worker_stop_thread(app);
    esp_flasher_uart_free(app->uart);
    app->uart = NULL;
    esp_flasher_reset_lines_deinit();

    // Views
    view_dispatcher_remove_view(app->view_dispatcher, EspFlasherAppViewVarItemList);
    view_dispatcher_remove_view(app->view_dispatcher, EspFlasherAppViewConsoleOutput);
    view_dispatcher_remove_view(app->view_dispatcher, EspFlasherAppViewWidget);
    view_dispatcher_remove_view(app->view_dispatcher, EspFlasherAppViewSubmenu);
    view_dispatcher_remove_view(app->view_dispatcher, EspFlasherAppViewTextInput);

    widget_free(app->widget);
    text_box_free(app->text_box);
    furi_string_free(app->text_box_store);
    for(size_t i = 0; i < COUNT_OF(app->text_box_display); i++) {
        furi_string_free(app->text_box_display[i]);
    }
    furi_mutex_free(app->console_mutex);
    submenu_free(app->submenu);
    text_input_free(app->text_input);
    variable_item_list_free(app->var_item_list);

    // View dispatcher
    view_dispatcher_free(app->view_dispatcher);
    scene_manager_free(app->scene_manager);

    // Close records
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_DIALOGS);
    furi_record_close(RECORD_NOTIFICATION);

    free(app);
}

static void esp_flasher_app_log_heap(const char* when) {
    // "enter" vs "exit" in the log shows whether a session left anything allocated
    FURI_LOG_I(
        TAG,
        "heap %s: internal %zu (min %zu), psram %zu",
        when,
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

int32_t esp_flasher_app(void* p) {
    UNUSED(p);
    esp_flasher_app_log_heap("enter");

    // Expansion modules share the UART pads: keep the listener off for the whole session
    Expansion* expansion = furi_record_open(RECORD_EXPANSION);
    expansion_disable(expansion);

    EspFlasherApp* app = esp_flasher_app_alloc();

    app->uart = esp_flasher_uart_init(app);
    if(app->uart) {
        esp_flasher_reset_lines_init(app->settings.reset_mode);
        view_dispatcher_run(app->view_dispatcher);
    } else {
        DialogMessage* message = dialog_message_alloc();
        dialog_message_set_header(message, "UART unavailable", 64, 3, AlignCenter, AlignTop);
        dialog_message_set_text(
            message,
#if ESP_FLASHER_HAS_UART
            "External UART is busy.\nClose apps using it\n(RFID, USB-UART) and retry.",
#else
            "This board has no\nexternal UART.",
#endif
            64,
            32,
            AlignCenter,
            AlignCenter);
        dialog_message_set_buttons(message, NULL, "OK", NULL);
        dialog_message_show(app->dialogs, message);
        dialog_message_free(message);
    }

    esp_flasher_app_free(app);

    expansion_enable(expansion);
    furi_record_close(RECORD_EXPANSION);

    esp_flasher_app_log_heap("exit");
    return 0;
}
