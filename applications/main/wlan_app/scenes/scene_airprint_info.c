/* AirPrint info scene (Widget): startet den IPP-Drucker auf der aktuellen
 * STA-Verbindung, bewirbt ihn per mDNS und zeigt Name/IP/empfangene Jobs. Back
 * stoppt Server + mDNS und kehrt zum Network-Actions-Menü zurück. */

#include "../wlan_app.h"
#include "../wlan_airprint.h"

static uint32_t s_last_jobs = 0xFFFFFFFF;

static void airprint_button_cb(GuiButtonType result, InputType type, void* context) {
    WlanApp* app = context;
    if(type != InputTypeShort) return;
    if(result == GuiButtonTypeCenter) {
        view_dispatcher_send_custom_event(app->view_dispatcher, WlanAppCustomEventAirPrintStop);
    } else if(result == GuiButtonTypeLeft) {
        view_dispatcher_send_custom_event(app->view_dispatcher, WlanAppCustomEventAirPrintConfig);
    }
}

static void airprint_render(WlanApp* app, bool ok, bool spam) {
    Widget* w = app->widget;
    widget_reset(w);
    widget_add_string_element(
        w, 64, 2, AlignCenter, AlignTop, FontPrimary, spam ? "AirPrint Spam" : "AirPrint");

    if(!ok) {
        widget_add_string_element(
            w, 64, 30, AlignCenter, AlignCenter, FontSecondary,
            spam ? "No names / WiFi?" : "Start failed (WiFi?)");
        if(!spam)
            widget_add_button_element(w, GuiButtonTypeLeft, "Config", airprint_button_cb, app);
        widget_add_button_element(w, GuiButtonTypeCenter, "Exit", airprint_button_cb, app);
        return;
    }

    char line[64];
    char ip[16] = {0};
    wlan_airprint_get_ip(ip, sizeof(ip));

    if(spam) {
        snprintf(line, sizeof(line), "Printers: %u", (unsigned)wlan_airprint_get_spam_count());
    } else {
        snprintf(line, sizeof(line), "%s", wlan_airprint_get_name());
    }
    widget_add_string_element(w, 64, 15, AlignCenter, AlignTop, FontSecondary, line);

    snprintf(line, sizeof(line), "%s:%u", ip, (unsigned)wlan_airprint_get_port());
    widget_add_string_element(w, 64, 27, AlignCenter, AlignTop, FontPrimary, line);

    uint32_t jobs = wlan_airprint_get_job_count();
    snprintf(line, sizeof(line), "Jobs: %u", (unsigned)jobs);
    widget_add_string_element(w, 64, 40, AlignCenter, AlignTop, FontSecondary, line);

    const char* last = wlan_airprint_get_last_file();
    if(last && last[0]) {
        widget_add_string_element(w, 64, 50, AlignCenter, AlignTop, FontSecondary, last);
    }

    /* Config nur im Einzelbetrieb (ändert den einen Druckernamen). */
    if(!spam) widget_add_button_element(w, GuiButtonTypeLeft, "Config", airprint_button_cb, app);
    widget_add_button_element(w, GuiButtonTypeCenter, "Stop", airprint_button_cb, app);
}

static void airprint_leave(WlanApp* app) {
    if(scene_manager_search_and_switch_to_previous_scene(
           app->scene_manager, WlanAppSceneAirPrintMenu))
        return;
    if(scene_manager_search_and_switch_to_previous_scene(app->scene_manager, WlanAppSceneMain))
        return;
    scene_manager_stop(app->scene_manager);
    view_dispatcher_stop(app->view_dispatcher);
}

void wlan_app_scene_airprint_info_on_enter(void* context) {
    WlanApp* app = context;
    bool spam =
        scene_manager_get_scene_state(app->scene_manager, WlanAppSceneAirPrintInfo) == 1;
    bool ok = spam ? wlan_airprint_start_spam() : wlan_airprint_start();
    s_last_jobs = 0xFFFFFFFF;
    airprint_render(app, ok, spam);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewWidget);
}

bool wlan_app_scene_airprint_info_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == WlanAppCustomEventAirPrintStop) {
            airprint_leave(app);
            consumed = true;
        } else if(event.event == WlanAppCustomEventAirPrintConfig) {
            /* on_exit stoppt den Server; Rückkehr aus der Config-Scene startet
             * ihn via on_enter mit dem (evtl. geänderten) Namen neu. */
            scene_manager_next_scene(app->scene_manager, WlanAppSceneAirPrintConfig);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        if(wlan_airprint_is_running()) {
            uint32_t jobs = wlan_airprint_get_job_count();
            if(jobs != s_last_jobs) {
                s_last_jobs = jobs;
                bool spam = scene_manager_get_scene_state(
                                app->scene_manager, WlanAppSceneAirPrintInfo) == 1;
                airprint_render(app, true, spam);
            }
        }
        consumed = true;
    } else if(event.type == SceneManagerEventTypeBack) {
        airprint_leave(app);
        consumed = true;
    }
    return consumed;
}

void wlan_app_scene_airprint_info_on_exit(void* context) {
    UNUSED(context);
    wlan_airprint_stop();
}
