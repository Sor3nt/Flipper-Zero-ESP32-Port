#include "bad_usb_app_i.h"
#include <furi.h>
#include <furi_hal.h>
#include <storage/storage.h>
#include <toolbox/path.h>
#include <flipper_format/flipper_format.h>

#define BAD_USB_SETTINGS_PATH           BAD_USB_APP_BASE_FOLDER "/.badusb.settings"
#define BAD_USB_SETTINGS_FILE_TYPE      "Flipper BadUSB Settings File"
#define BAD_USB_SETTINGS_VERSION        1
#define BAD_USB_SETTINGS_DEFAULT_LAYOUT BAD_USB_APP_PATH_LAYOUT_FOLDER "/en-US.kl"
#define BAD_USB_APP_EVENT_SCRIPT_STOPPED UINT32_MAX
#define BAD_USB_STOP_WARNING_MS 3000U

static void bad_usb_app_script_stopped(void* context) {
    BadUsbApp* app = context;
    view_dispatcher_send_custom_event(
        app->view_dispatcher, BAD_USB_APP_EVENT_SCRIPT_STOPPED);
}

static bool bad_usb_app_custom_event_callback(void* context, uint32_t event) {
    furi_assert(context);
    BadUsbApp* app = context;
    if((event == BAD_USB_APP_EVENT_SCRIPT_STOPPED) &&
       (app->shutdown_action != BadUsbShutdownNone)) {
        FURI_LOG_I(
            "BadUsb",
            "Worker stopped; completing shutdown action=%u",
            (unsigned)app->shutdown_action);
        bad_usb_script_close(app->bad_usb_script);
        app->bad_usb_script = NULL;
        /* A stopped BLE script leaves the app alive on file selection or
         * configuration. Restore the normal serial profile now; app_free keeps
         * the same call as a fallback for exits that bypass this callback. */
        bad_usb_hid_ble_restore_default();
        BadUsbShutdownAction action = app->shutdown_action;
        app->shutdown_action = BadUsbShutdownNone;
        app->shutdown_warning_logged = false;
        bad_usb_view_set_stopping(app->bad_usb_view, false);
        if(action == BadUsbShutdownConfig) {
            scene_manager_set_scene_state(app->scene_manager, BadUsbSceneConfig, 0);
            scene_manager_next_scene(app->scene_manager, BadUsbSceneConfig);
        } else if((action == BadUsbShutdownPreviousScene) &&
                  scene_manager_previous_scene(app->scene_manager)) {
            return true;
        } else {
            view_dispatcher_stop(app->view_dispatcher);
        }
        return true;
    }
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

void bad_usb_app_stop_script_async(BadUsbApp* app, BadUsbShutdownAction action) {
    furi_assert(app);
    furi_assert(app->bad_usb_script);
    furi_assert(action != BadUsbShutdownNone);
    if(app->shutdown_action != BadUsbShutdownNone) return;

    app->shutdown_action = action;
    app->shutdown_started_tick = furi_get_tick();
    app->shutdown_warning_logged = false;
    FURI_LOG_I("BadUsb", "Script stop requested action=%u", (unsigned)action);
    bad_usb_view_set_stopping(app->bad_usb_view, true);
    bad_usb_script_stop_async(app->bad_usb_script, bad_usb_app_script_stopped, app);
}

static bool bad_usb_app_back_event_callback(void* context) {
    furi_assert(context);
    BadUsbApp* app = context;
    if(app->shutdown_action != BadUsbShutdownNone) {
        FURI_LOG_W("BadUsb", "Back ignored while script shutdown is pending");
        return true;
    }

    if((scene_manager_get_current_scene(app->scene_manager) == BadUsbSceneWork) &&
       app->bad_usb_script) {
        bad_usb_app_stop_script_async(app, BadUsbShutdownPreviousScene);
        return true;
    }

    /* File selection is the root scene. A Back here must leave BadUSB so the
     * loader returns to the launcher, even if stale scene history remains. */
    if(scene_manager_get_current_scene(app->scene_manager) == BadUsbSceneFileSelect) {
        return false;
    }

    if(scene_manager_handle_back_event(app->scene_manager)) return true;
    if(app->bad_usb_script) {
        bad_usb_app_stop_script_async(app, BadUsbShutdownExit);
        return true;
    }
    return false;
}

static void bad_usb_app_tick_event_callback(void* context) {
    furi_assert(context);
    BadUsbApp* app = context;
    if((app->shutdown_action != BadUsbShutdownNone) && !app->shutdown_warning_logged &&
       ((furi_get_tick() - app->shutdown_started_tick) >= BAD_USB_STOP_WARNING_MS)) {
        BadUsbState* state =
            app->bad_usb_script ? bad_usb_script_get_state(app->bad_usb_script) : NULL;
        FURI_LOG_E(
            "BadUsb",
            "Script shutdown still pending action=%u worker_state=%d elapsed=%lu ms",
            (unsigned)app->shutdown_action,
            state ? (int)state->state : -1,
            (unsigned long)(furi_get_tick() - app->shutdown_started_tick));
        app->shutdown_warning_logged = true;
    }
    scene_manager_handle_tick_event(app->scene_manager);
}

static void bad_usb_load_settings(BadUsbApp* app) {
    BadUsbHidConfig* hid_cfg = &app->user_hid_cfg;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* settings = flipper_format_file_alloc(storage);
    FuriString* value = furi_string_alloc();
    uint32_t version = 0;
    uint32_t interface = 0;
    bool loaded = false;

    if(flipper_format_file_open_existing(settings, BAD_USB_SETTINGS_PATH)) {
        do {
            if(!flipper_format_read_header(settings, value, &version)) break;
            if((strcmp(furi_string_get_cstr(value), BAD_USB_SETTINGS_FILE_TYPE) != 0) ||
               (version != BAD_USB_SETTINGS_VERSION))
                break;
            if(!flipper_format_read_string(settings, "layout", value)) break;
            if(!flipper_format_read_uint32(settings, "interface", &interface, 1)) break;
            if(interface > BadUsbHidInterfaceBle) break;

            furi_string_set(app->keyboard_layout, value);
            app->interface = interface;
            loaded = true;

            FileInfo layout_info;
            if((storage_common_stat(
                    storage, furi_string_get_cstr(app->keyboard_layout), &layout_info) != FSE_OK) ||
               (layout_info.size != 256)) {
                furi_string_set(app->keyboard_layout, BAD_USB_SETTINGS_DEFAULT_LAYOUT);
            }
        } while(false);
    }

    flipper_format_free(settings);
    furi_record_close(RECORD_STORAGE);
    furi_string_free(value);

    if(!loaded) {
        furi_string_set(app->keyboard_layout, BAD_USB_SETTINGS_DEFAULT_LAYOUT);
        app->interface =
            bad_usb_hid_supports_usb() ? BadUsbHidInterfaceUsb : BadUsbHidInterfaceBle;
    } else if((app->interface == BadUsbHidInterfaceUsb) && !bad_usb_hid_supports_usb()) {
        app->interface = BadUsbHidInterfaceBle;
    }
    hid_cfg->ble.name[0] = '\0';
    memset(hid_cfg->ble.mac, 0, sizeof(hid_cfg->ble.mac));
    hid_cfg->ble.bonding = true;
    hid_cfg->ble.pairing = GapPairingPinCodeVerifyYesNo;
    hid_cfg->usb.vid = 0;
    hid_cfg->usb.pid = 0;
    hid_cfg->usb.manuf[0] = '\0';
    hid_cfg->usb.product[0] = '\0';
}

static void bad_usb_save_settings(BadUsbApp* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* settings = flipper_format_file_alloc(storage);

    if(flipper_format_file_open_always(settings, BAD_USB_SETTINGS_PATH)) {
        do {
            if(!flipper_format_write_header_cstr(
                   settings, BAD_USB_SETTINGS_FILE_TYPE, BAD_USB_SETTINGS_VERSION))
                break;
            if(!flipper_format_write_string(settings, "layout", app->keyboard_layout)) break;
            const uint32_t interface = app->interface;
            if(!flipper_format_write_uint32(settings, "interface", &interface, 1)) break;
        } while(false);
    }

    flipper_format_free(settings);
    furi_record_close(RECORD_STORAGE);
}

void bad_usb_set_interface(BadUsbApp* app, BadUsbHidInterface interface) {
    if((interface == BadUsbHidInterfaceUsb) &&
       (!bad_usb_hid_supports_usb() || furi_hal_usb_is_locked())) {
        interface = BadUsbHidInterfaceBle;
    }

    app->interface = interface;
    if(app->bad_usb_view) {
        bad_usb_view_set_interface(app->bad_usb_view, interface);
    }
}

BadUsbApp* bad_usb_app_alloc(char* arg) {
    BadUsbApp* app = malloc(sizeof(BadUsbApp));

    app->shutdown_action = BadUsbShutdownNone;
    app->shutdown_started_tick = 0;
    app->shutdown_warning_logged = false;
    app->bad_usb_script = NULL;

    app->file_path = furi_string_alloc();
    app->keyboard_layout = furi_string_alloc();
    if(arg && strlen(arg)) {
        furi_string_set(app->file_path, arg);
    }

    bad_usb_load_settings(app);

    app->gui = furi_record_open(RECORD_GUI);
    app->dialogs = furi_record_open(RECORD_DIALOGS);

    app->view_dispatcher = view_dispatcher_alloc();
    app->scene_manager = scene_manager_alloc(&bad_usb_scene_handlers, app);

    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_tick_event_callback(
        app->view_dispatcher, bad_usb_app_tick_event_callback, 250);
    view_dispatcher_set_custom_event_callback(
        app->view_dispatcher, bad_usb_app_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, bad_usb_app_back_event_callback);

    // Custom Widget
    app->widget = widget_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, BadUsbAppViewWidget, widget_get_view(app->widget));

    // Popup
    app->popup = popup_alloc();
    view_dispatcher_add_view(app->view_dispatcher, BadUsbAppViewPopup, popup_get_view(app->popup));

    app->var_item_list = variable_item_list_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher,
        BadUsbAppViewConfig,
        variable_item_list_get_view(app->var_item_list));

    app->bad_usb_view = bad_usb_view_alloc();
    bad_usb_view_set_usb_supported(app->bad_usb_view, bad_usb_hid_supports_usb());
    view_dispatcher_add_view(
        app->view_dispatcher, BadUsbAppViewWork, bad_usb_view_get_view(app->bad_usb_view));

    app->text_input = text_input_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, BadUsbAppViewTextInput, text_input_get_view(app->text_input));

    app->byte_input = byte_input_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, BadUsbAppViewByteInput, byte_input_get_view(app->byte_input));

    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    bad_usb_set_interface(app, app->interface);

    if(!bad_usb_hid_supports_usb()) {
        app->usb_if_prev = NULL;
    } else if(furi_hal_usb_is_locked()) {
        app->usb_if_prev = NULL;
        bad_usb_set_interface(app, app->interface);
    } else {
        app->usb_if_prev = furi_hal_usb_get_config();
        furi_check(furi_hal_usb_set_config(NULL, NULL));
    }

    if(!furi_string_empty(app->file_path)) {
        scene_manager_set_scene_state(app->scene_manager, BadUsbSceneWork, true);
        scene_manager_next_scene(app->scene_manager, BadUsbSceneWork);
    } else {
        furi_string_set(app->file_path, BAD_USB_APP_BASE_FOLDER);
        scene_manager_next_scene(app->scene_manager, BadUsbSceneFileSelect);
    }

    return app;
}

void bad_usb_app_free(BadUsbApp* app) {
    furi_assert(app);
    FURI_LOG_I("BadUsb", "app_free begin");

    if(app->bad_usb_script) {
        bad_usb_script_close(app->bad_usb_script);
        app->bad_usb_script = NULL;
    }

    // Views
    view_dispatcher_remove_view(app->view_dispatcher, BadUsbAppViewWork);
    bad_usb_view_free(app->bad_usb_view);

    // Custom Widget
    view_dispatcher_remove_view(app->view_dispatcher, BadUsbAppViewWidget);
    widget_free(app->widget);

    // Popup
    view_dispatcher_remove_view(app->view_dispatcher, BadUsbAppViewPopup);
    popup_free(app->popup);

    // Config menu
    view_dispatcher_remove_view(app->view_dispatcher, BadUsbAppViewConfig);
    variable_item_list_free(app->var_item_list);

    // Text Input
    view_dispatcher_remove_view(app->view_dispatcher, BadUsbAppViewTextInput);
    text_input_free(app->text_input);

    // Byte Input
    view_dispatcher_remove_view(app->view_dispatcher, BadUsbAppViewByteInput);
    byte_input_free(app->byte_input);

    // View dispatcher
    view_dispatcher_free(app->view_dispatcher);
    scene_manager_free(app->scene_manager);

    // Close records
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_DIALOGS);

    bad_usb_save_settings(app);

    furi_string_free(app->file_path);
    furi_string_free(app->keyboard_layout);

    if(!furi_hal_usb_hid_release_stack()) {
        FURI_LOG_E("BadUsb", "Failed to release the USB HID stack on app exit");
    }

    if(app->usb_if_prev) {
        furi_check(furi_hal_usb_set_config(app->usb_if_prev, NULL));
    }

    /* Queue BLE profile restoration only after detaching BadUSB's GUI views.
     * The BT service may wait for GATT startup, but that wait must not keep the
     * BadUSB screen attached or delay returning to the launcher. */
    bad_usb_hid_ble_restore_default();

    free(app);
    FURI_LOG_I("BadUsb", "app_free end");
}

int32_t bad_usb_app(void* p) {
    BadUsbApp* bad_usb_app = bad_usb_app_alloc((char*)p);

    view_dispatcher_run(bad_usb_app->view_dispatcher);

    bad_usb_app_free(bad_usb_app);
    return 0;
}
