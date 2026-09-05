#include "../wlan_app.h"
#include "../wlan_fw_update.h"
#include "../wlan_sd_update.h"

#include <esp_system.h> // esp_restart

// Kombiniertes Update (Menüpunkt "Update"): prüft ZUERST die Firmware
// (wlan_fw_update, OTA von SD), DANACH die SD-Dateien (wlan_sd_update,
// Delta-Sync). Ein Firmware-Flash rebootet sofort — die SD-Phase läuft daher
// nur, wenn KEIN FW-Update ansteht (bzw. der User es überspringt); nach einem
// FW-Flash+Reboot erreicht der nächste "Update"-Aufruf direkt die SD-Phase,
// weil die FW dann als aktuell erkannt wird (Marker /ext/.fw_version).

typedef enum {
    UpdCheckingFw = 0,
    UpdConfirmFwDownload,
    UpdFwDownloading,
    UpdConfirmFwInstall,
    UpdFwFlashing,
    UpdFwDone,
    UpdSdRunning,
    UpdSdInfo, // Uptodate-/Done-Popup sichtbar
    UpdError,
} UpdState;

#define UPD_DONE_POPUP_MS 2500
#define UPD_INFO_POPUP_MS 1500

// Verlässt die Update-Scene: zurück zu Main (aus dem WiFi-Menü) oder — wenn per
// Launch-Arg "update" aus dem Settings-Menü gestartet, wo kein Main im Stack ist
// — die App beenden.
static void upd_leave(WlanApp* app) {
    if(scene_manager_search_and_switch_to_previous_scene(
           app->scene_manager, WlanAppSceneMain))
        return;

    scene_manager_stop(app->scene_manager);
    view_dispatcher_stop(app->view_dispatcher);
}

static void upd_set_state(WlanApp* app, UpdState s) {
    scene_manager_set_scene_state(app->scene_manager, WlanAppSceneFwUpdate, s);
}

static UpdState upd_get_state(WlanApp* app) {
    return (UpdState)scene_manager_get_scene_state(
        app->scene_manager, WlanAppSceneFwUpdate);
}

// --- Callbacks -------------------------------------------------------------

static void upd_fw_no_cb(GuiButtonType result, InputType type, void* context) {
    WlanApp* app = context;
    if(type == InputTypeShort && result == GuiButtonTypeLeft) {
        // "No" auf FW-Dialog → Firmware überspringen, mit SD weitermachen.
        view_dispatcher_send_custom_event(
            app->view_dispatcher, WlanAppCustomEventFwUpdateSkip);
    }
}

static void upd_fw_download_yes_cb(GuiButtonType result, InputType type, void* context) {
    WlanApp* app = context;
    if(type == InputTypeShort && result == GuiButtonTypeRight) {
        view_dispatcher_send_custom_event(
            app->view_dispatcher, WlanAppCustomEventFwUpdateStart);
    }
}

static void upd_fw_install_yes_cb(GuiButtonType result, InputType type, void* context) {
    WlanApp* app = context;
    if(type == InputTypeShort && result == GuiButtonTypeRight) {
        view_dispatcher_send_custom_event(
            app->view_dispatcher, WlanAppCustomEventFwUpdateInstall);
    }
}

static void upd_error_ok_cb(GuiButtonType result, InputType type, void* context) {
    WlanApp* app = context;
    if(type == InputTypeShort && result == GuiButtonTypeRight) {
        view_dispatcher_send_custom_event(
            app->view_dispatcher, WlanAppCustomEventFwUpdateFinished);
    }
}

static void upd_info_popup_cb(void* context) {
    WlanApp* app = context;
    view_dispatcher_send_custom_event(
        app->view_dispatcher, WlanAppCustomEventFwUpdateFinished);
}

static void upd_reboot_popup_cb(void* context) {
    UNUSED(context);
    esp_restart();
}

// --- Screens ---------------------------------------------------------------

static void upd_show_fw_progress(WlanApp* app, const char* status) {
    wlan_fw_update_view_update(
        app->view_fw_update,
        status,
        wlan_fw_update_get_percent(app->fw_update),
        wlan_fw_update_get_bytes_done(app->fw_update),
        wlan_fw_update_get_bytes_total(app->fw_update),
        wlan_fw_update_get_speed_kbps(app->fw_update));
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewFwUpdate);
}

static void upd_show_sd_progress(WlanApp* app, const char* status) {
    wlan_sd_update_view_update(
        app->view_sd_update,
        status,
        wlan_sd_update_get_percent(app->sd_update),
        wlan_sd_update_get_done(app->sd_update),
        wlan_sd_update_get_total(app->sd_update),
        wlan_sd_update_get_current_file(app->sd_update),
        wlan_sd_update_get_speed_kbps(app->sd_update));
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewSdUpdate);
}

static void upd_show_confirm_fw_download(WlanApp* app) {
    char line[64];
    snprintf(
        line, sizeof(line), "Download %s now?",
        wlan_fw_update_get_remote_version(app->fw_update));
    widget_reset(app->widget);
    widget_add_string_element(
        app->widget, 64, 20, AlignCenter, AlignBottom, FontPrimary, "Firmware update");
    widget_add_string_element(
        app->widget, 64, 38, AlignCenter, AlignBottom, FontSecondary, line);
    widget_add_button_element(app->widget, GuiButtonTypeLeft, "No", upd_fw_no_cb, app);
    widget_add_button_element(
        app->widget, GuiButtonTypeRight, "Yes", upd_fw_download_yes_cb, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewWidget);
}

static void upd_show_confirm_fw_install(WlanApp* app) {
    char line[64];
    snprintf(
        line, sizeof(line), "Install %s?",
        wlan_fw_update_get_remote_version(app->fw_update));
    widget_reset(app->widget);
    widget_add_string_element(
        app->widget, 64, 20, AlignCenter, AlignBottom, FontPrimary, "Download done");
    widget_add_string_element(
        app->widget, 64, 38, AlignCenter, AlignBottom, FontSecondary, line);
    widget_add_button_element(app->widget, GuiButtonTypeLeft, "No", upd_fw_no_cb, app);
    widget_add_button_element(
        app->widget, GuiButtonTypeRight, "Yes", upd_fw_install_yes_cb, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewWidget);
}

static void upd_show_uptodate(WlanApp* app) {
    popup_reset(app->popup);
    popup_set_header(app->popup, "Update", 64, 10, AlignCenter, AlignTop);
    popup_set_text(app->popup, "Already up-to-date", 64, 36, AlignCenter, AlignCenter);
    popup_set_context(app->popup, app);
    popup_set_callback(app->popup, upd_info_popup_cb);
    popup_set_timeout(app->popup, UPD_INFO_POPUP_MS);
    popup_enable_timeout(app->popup);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewPopup);
}

static void upd_show_sd_done(WlanApp* app) {
    popup_reset(app->popup);
    popup_set_header(app->popup, "Update", 64, 10, AlignCenter, AlignTop);
    popup_set_text(app->popup, "Done!", 64, 36, AlignCenter, AlignCenter);
    popup_set_context(app->popup, app);
    popup_set_callback(app->popup, upd_info_popup_cb);
    popup_set_timeout(app->popup, UPD_INFO_POPUP_MS);
    popup_enable_timeout(app->popup);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewPopup);
}

static void upd_show_fw_reboot(WlanApp* app) {
    popup_reset(app->popup);
    popup_set_header(app->popup, "Firmware", 64, 10, AlignCenter, AlignTop);
    popup_set_text(app->popup, "Installed!\nRebooting...", 64, 32, AlignCenter, AlignCenter);
    popup_set_context(app->popup, app);
    popup_set_callback(app->popup, upd_reboot_popup_cb);
    popup_set_timeout(app->popup, UPD_DONE_POPUP_MS);
    popup_enable_timeout(app->popup);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewPopup);
}

static void upd_show_error(WlanApp* app, const char* msg) {
    widget_reset(app->widget);
    widget_add_string_element(
        app->widget, 64, 16, AlignCenter, AlignBottom, FontPrimary, "Update failed");
    widget_add_text_box_element(
        app->widget, 0, 22, 128, 26, AlignCenter, AlignTop, msg, false);
    widget_add_button_element(app->widget, GuiButtonTypeRight, "OK", upd_error_ok_cb, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewWidget);
}

// SD-Phase starten (nach FW aktuell oder übersprungen).
static void upd_start_sd(WlanApp* app) {
    upd_set_state(app, UpdSdRunning);
    wlan_sd_update_start(app->sd_update);
    upd_show_sd_progress(app, "Checking Version");
}

// --- Scene handlers --------------------------------------------------------

void wlan_app_scene_fw_update_on_enter(void* context) {
    WlanApp* app = context;
    app->fw_update_flow = false; // Flow erreicht → Flag konsumiert
    upd_set_state(app, UpdCheckingFw);
    wlan_fw_update_check_start(app->fw_update);
    upd_show_fw_progress(app, "Checking Firmware");
}

bool wlan_app_scene_fw_update_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == WlanAppCustomEventFwUpdateCancel ||
           event.event == WlanAppCustomEventUpdateSdCancel) {
            // Während des Flashens NICHT abbrechen (OTA-Task läuft sonst verwaist
            // weiter, Reboot bliebe aus).
            if(upd_get_state(app) != UpdFwFlashing) {
                upd_leave(app);
            }
            consumed = true;
        } else if(event.event == WlanAppCustomEventFwUpdateFinished) {
            upd_leave(app);
            consumed = true;
        } else if(event.event == WlanAppCustomEventFwUpdateSkip) {
            // Firmware übersprungen → mit der SD-Phase weitermachen.
            upd_start_sd(app);
            consumed = true;
        } else if(event.event == WlanAppCustomEventFwUpdateStart) {
            upd_set_state(app, UpdFwDownloading);
            wlan_fw_update_download_start(app->fw_update);
            upd_show_fw_progress(app, "Downloading");
            consumed = true;
        } else if(event.event == WlanAppCustomEventFwUpdateInstall) {
            upd_set_state(app, UpdFwFlashing);
            wlan_fw_update_flash_start(app->fw_update);
            upd_show_fw_progress(app, "Installing");
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        UpdState st = upd_get_state(app);

        if(st == UpdCheckingFw) {
            FwUpdatePhase ph = wlan_fw_update_get_phase(app->fw_update);
            if(ph == FwUpdateUpToDate) {
                upd_start_sd(app); // FW aktuell → SD prüfen
            } else if(ph == FwUpdateAvailable) {
                upd_set_state(app, UpdConfirmFwDownload);
                upd_show_confirm_fw_download(app);
            } else if(ph == FwUpdateError) {
                upd_set_state(app, UpdError);
                upd_show_error(app, wlan_fw_update_get_error(app->fw_update));
            }
        } else if(st == UpdFwDownloading) {
            FwUpdatePhase ph = wlan_fw_update_get_phase(app->fw_update);
            if(ph == FwUpdateDownloading) {
                upd_show_fw_progress(app, "Downloading");
            } else if(ph == FwUpdateDownloaded) {
                upd_set_state(app, UpdConfirmFwInstall);
                upd_show_confirm_fw_install(app);
            } else if(ph == FwUpdateError) {
                upd_set_state(app, UpdError);
                upd_show_error(app, wlan_fw_update_get_error(app->fw_update));
            }
        } else if(st == UpdFwFlashing) {
            FwUpdatePhase ph = wlan_fw_update_get_phase(app->fw_update);
            if(ph == FwUpdateFlashing) {
                upd_show_fw_progress(app, "Installing");
            } else if(ph == FwUpdateDone) {
                upd_set_state(app, UpdFwDone);
                upd_show_fw_reboot(app);
            } else if(ph == FwUpdateError) {
                upd_set_state(app, UpdError);
                upd_show_error(app, wlan_fw_update_get_error(app->fw_update));
            }
        } else if(st == UpdSdRunning) {
            WlanSdUpdatePhase ph = wlan_sd_update_get_phase(app->sd_update);
            if(ph == WlanSdUpdateChecking) {
                upd_show_sd_progress(app, "Checking Version");
            } else if(ph == WlanSdUpdateDownloading || ph == WlanSdUpdateExtracting) {
                upd_show_sd_progress(app, "Sync Files");
            } else if(ph == WlanSdUpdateUpToDate) {
                upd_set_state(app, UpdSdInfo);
                upd_show_uptodate(app);
            } else if(ph == WlanSdUpdateDone) {
                upd_set_state(app, UpdSdInfo);
                upd_show_sd_done(app);
            } else if(ph == WlanSdUpdateError) {
                upd_set_state(app, UpdError);
                upd_show_error(app, wlan_sd_update_get_error(app->sd_update));
            }
        }
    }

    return consumed;
}

void wlan_app_scene_fw_update_on_exit(void* context) {
    WlanApp* app = context;
    // Check-/Download-Worker abbrechen; während des Flashens NICHT (siehe oben).
    if(upd_get_state(app) != UpdFwFlashing) {
        wlan_fw_update_cancel(app->fw_update);
    }
    wlan_sd_update_cancel(app->sd_update); // no-op wenn nicht laufend
    popup_reset(app->popup);
    widget_reset(app->widget);
    upd_set_state(app, UpdCheckingFw);
}
