/* Printer-Hijack Schritt 2 (Widget): übernimmt beim Enter die IP des gewählten
 * Druckers (wlan_printhijack_start) und leitet dessen Jobs auf dieses Gerät.
 * Zeigt Zielname/IP + empfangene Jobs. Stop/Back beendet und stellt DHCP wieder her. */

#include "../wlan_app.h"
#include "../wlan_airprint.h"
#include "../wlan_printhijack.h"

static uint32_t s_last_jobs = 0xFFFFFFFF;

static void hijack_button_cb(GuiButtonType result, InputType type, void* context) {
    WlanApp* app = context;
    if(type == InputTypeShort && result == GuiButtonTypeCenter) {
        view_dispatcher_send_custom_event(app->view_dispatcher, WlanAppCustomEventAirPrintStop);
    }
}

static void hijack_render(WlanApp* app, bool ok) {
    Widget* w = app->widget;
    widget_reset(w);
    widget_add_string_element(w, 64, 2, AlignCenter, AlignTop, FontPrimary, "Printer Hijack");

    if(!ok) {
        widget_add_string_element(
            w, 64, 30, AlignCenter, AlignCenter, FontSecondary, "Start failed");
        widget_add_button_element(w, GuiButtonTypeCenter, "Exit", hijack_button_cb, app);
        return;
    }

    char line[64];
    snprintf(line, sizeof(line), "%s", app->airprint_hijack_name[0] ? app->airprint_hijack_name : "printer");
    widget_add_string_element(w, 64, 15, AlignCenter, AlignTop, FontSecondary, line);

    char ip[16] = {0};
    wlan_printhijack_get_ip(ip, sizeof(ip));
    snprintf(line, sizeof(line), "-> %s", ip);
    widget_add_string_element(w, 64, 27, AlignCenter, AlignTop, FontPrimary, line);

    snprintf(line, sizeof(line), "Jobs: %u", (unsigned)wlan_airprint_get_job_count());
    widget_add_string_element(w, 64, 40, AlignCenter, AlignTop, FontSecondary, line);

    const char* last = wlan_airprint_get_last_file();
    if(last && last[0]) {
        widget_add_string_element(w, 64, 50, AlignCenter, AlignTop, FontSecondary, last);
    }

    widget_add_button_element(w, GuiButtonTypeCenter, "Stop", hijack_button_cb, app);
}

static void hijack_leave(WlanApp* app) {
    if(scene_manager_search_and_switch_to_previous_scene(
           app->scene_manager, WlanAppSceneAirPrintMenu))
        return;
    if(scene_manager_search_and_switch_to_previous_scene(app->scene_manager, WlanAppSceneMain))
        return;
    scene_manager_stop(app->scene_manager);
    view_dispatcher_stop(app->view_dispatcher);
}

void wlan_app_scene_airprint_hijack_on_enter(void* context) {
    WlanApp* app = context;
    bool ok = wlan_printhijack_start(
        app->airprint_hijack_ip, app->airprint_hijack_name, app->airprint_hijack_mac,
        app->airprint_hijack_has_mac);
    s_last_jobs = 0xFFFFFFFF;
    hijack_render(app, ok);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewWidget);
}

bool wlan_app_scene_airprint_hijack_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == WlanAppCustomEventAirPrintStop) {
            hijack_leave(app);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        if(wlan_printhijack_is_running()) {
            uint32_t jobs = wlan_airprint_get_job_count();
            if(jobs != s_last_jobs) {
                s_last_jobs = jobs;
                hijack_render(app, true);
            }
        }
        consumed = true;
    } else if(event.type == SceneManagerEventTypeBack) {
        hijack_leave(app);
        consumed = true;
    }
    return consumed;
}

void wlan_app_scene_airprint_hijack_on_exit(void* context) {
    UNUSED(context);
    wlan_printhijack_stop();
}
