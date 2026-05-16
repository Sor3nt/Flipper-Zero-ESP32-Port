#include "../wlan_app.h"
#include "../wlan_sd_update.h"

typedef enum {
    UpdateSdStateConfirm = 0,
    UpdateSdStateRunning,
    UpdateSdStateDone,
    UpdateSdStateUpToDate,
    UpdateSdStateError,
} UpdateSdState;

#define UPDATE_SD_DONE_POPUP_MS 1500

static void update_sd_set_state(WlanApp* app, UpdateSdState s) {
    scene_manager_set_scene_state(app->scene_manager, WlanAppSceneUpdateSd, s);
}

static UpdateSdState update_sd_get_state(WlanApp* app) {
    return (UpdateSdState)scene_manager_get_scene_state(
        app->scene_manager, WlanAppSceneUpdateSd);
}

static void update_sd_confirm_no_cb(GuiButtonType result, InputType type, void* context) {
    WlanApp* app = context;
    if(type == InputTypeShort && result == GuiButtonTypeLeft) {
        view_dispatcher_send_custom_event(
            app->view_dispatcher, WlanAppCustomEventUpdateSdCancel);
    }
}

static void update_sd_confirm_yes_cb(GuiButtonType result, InputType type, void* context) {
    WlanApp* app = context;
    if(type == InputTypeShort && result == GuiButtonTypeRight) {
        view_dispatcher_send_custom_event(
            app->view_dispatcher, WlanAppCustomEventUpdateSdStart);
    }
}

static void update_sd_error_ok_cb(GuiButtonType result, InputType type, void* context) {
    WlanApp* app = context;
    if(type == InputTypeShort && result == GuiButtonTypeRight) {
        view_dispatcher_send_custom_event(
            app->view_dispatcher, WlanAppCustomEventUpdateSdFinished);
    }
}

static void update_sd_done_popup_cb(void* context) {
    WlanApp* app = context;
    view_dispatcher_send_custom_event(
        app->view_dispatcher, WlanAppCustomEventUpdateSdFinished);
}

static void update_sd_show_confirm(WlanApp* app) {
    widget_reset(app->widget);
    widget_add_string_element(
        app->widget, 64, 20, AlignCenter, AlignBottom, FontPrimary, "Update SD Card");
    widget_add_string_element(
        app->widget, 64, 38, AlignCenter, AlignBottom, FontSecondary,
        "with latest files ?");
    widget_add_button_element(
        app->widget, GuiButtonTypeLeft, "No", update_sd_confirm_no_cb, app);
    widget_add_button_element(
        app->widget, GuiButtonTypeRight, "Yes", update_sd_confirm_yes_cb, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewWidget);
}

static void update_sd_show_running(
    WlanApp* app, const char* status, uint8_t pct) {
    wlan_sd_update_view_update(
        app->view_sd_update,
        status,
        pct,
        wlan_sd_update_get_done(app->sd_update),
        wlan_sd_update_get_total(app->sd_update),
        wlan_sd_update_get_current_file(app->sd_update),
        wlan_sd_update_get_speed_kbps(app->sd_update));
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewSdUpdate);
}

static void update_sd_show_uptodate(WlanApp* app) {
    popup_reset(app->popup);
    popup_set_header(app->popup, "Update SD", 64, 10, AlignCenter, AlignTop);
    popup_set_text(
        app->popup, "Already up-to-date", 64, 36, AlignCenter, AlignCenter);
    popup_set_context(app->popup, app);
    popup_set_callback(app->popup, update_sd_done_popup_cb);
    popup_set_timeout(app->popup, UPDATE_SD_DONE_POPUP_MS);
    popup_enable_timeout(app->popup);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewPopup);
}

static void update_sd_show_done(WlanApp* app) {
    popup_reset(app->popup);
    popup_set_header(app->popup, "Update SD", 64, 10, AlignCenter, AlignTop);
    popup_set_text(app->popup, "Done!", 64, 36, AlignCenter, AlignCenter);
    popup_set_context(app->popup, app);
    popup_set_callback(app->popup, update_sd_done_popup_cb);
    popup_set_timeout(app->popup, UPDATE_SD_DONE_POPUP_MS);
    popup_enable_timeout(app->popup);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewPopup);
}

static void update_sd_show_error(WlanApp* app, const char* msg) {
    widget_reset(app->widget);
    widget_add_string_element(
        app->widget, 64, 16, AlignCenter, AlignBottom, FontPrimary, "Update failed");
    widget_add_text_box_element(
        app->widget, 0, 22, 128, 26, AlignCenter, AlignTop, msg, false);
    widget_add_button_element(
        app->widget, GuiButtonTypeRight, "OK", update_sd_error_ok_cb, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewWidget);
}

void wlan_app_scene_update_sd_on_enter(void* context) {
    WlanApp* app = context;
    app->update_sd_flow = false; // Flow erreicht → Flag konsumiert
    update_sd_set_state(app, UpdateSdStateConfirm);
    update_sd_show_confirm(app);
}

bool wlan_app_scene_update_sd_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == WlanAppCustomEventUpdateSdCancel) {
            scene_manager_search_and_switch_to_previous_scene(
                app->scene_manager, WlanAppSceneMain);
            consumed = true;
        } else if(event.event == WlanAppCustomEventUpdateSdStart) {
            update_sd_set_state(app, UpdateSdStateRunning);
            wlan_sd_update_start(app->sd_update);
            update_sd_show_running(app, "Checking Version", 0);
            consumed = true;
        } else if(event.event == WlanAppCustomEventUpdateSdFinished) {
            scene_manager_search_and_switch_to_previous_scene(
                app->scene_manager, WlanAppSceneMain);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        if(update_sd_get_state(app) == UpdateSdStateRunning) {
            WlanSdUpdatePhase ph = wlan_sd_update_get_phase(app->sd_update);
            uint8_t pct = wlan_sd_update_get_percent(app->sd_update);
            if(ph == WlanSdUpdateChecking) {
                update_sd_show_running(app, "Checking Version", pct);
            } else if(ph == WlanSdUpdateDownloading || ph == WlanSdUpdateExtracting) {
                update_sd_show_running(app, "Sync Files", pct);
            } else if(ph == WlanSdUpdateUpToDate) {
                update_sd_set_state(app, UpdateSdStateUpToDate);
                update_sd_show_uptodate(app);
            } else if(ph == WlanSdUpdateDone) {
                update_sd_set_state(app, UpdateSdStateDone);
                update_sd_show_done(app);
            } else if(ph == WlanSdUpdateError) {
                update_sd_set_state(app, UpdateSdStateError);
                update_sd_show_error(app, wlan_sd_update_get_error(app->sd_update));
            }
        }
    }

    return consumed;
}

void wlan_app_scene_update_sd_on_exit(void* context) {
    WlanApp* app = context;
    // Egal wie wir die Scene verlassen: laufenden Worker abbrechen und auf
    // sein Ende warten, damit kein Task auf freigegebenen State zugreift.
    wlan_sd_update_cancel(app->sd_update);
    popup_reset(app->popup);
    widget_reset(app->widget);
    update_sd_set_state(app, UpdateSdStateConfirm);
}
