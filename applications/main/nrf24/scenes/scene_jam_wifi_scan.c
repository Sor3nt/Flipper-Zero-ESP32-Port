#include "../nrf24_app.h"
#include "../nrf24_wifi_scan.h"

#include <furi.h>
#include <stdlib.h>
#include <string.h>

#define SCAN_EVENT_RUN  1
#define SCAN_EVENT_PICK 1000

static void wifi_scan_submenu_callback(void* context, uint32_t index) {
    Nrf24App* app = context;
    if(index < app->wifi_ap_count) {
        view_dispatcher_send_custom_event(app->view_dispatcher, SCAN_EVENT_PICK + index);
    }
}

static void show_results(Nrf24App* app) {
    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, "Pick WiFi network");

    if(app->wifi_ap_count == 0) {
        widget_reset(app->widget);
        widget_add_text_box_element(
            app->widget, 0, 0, 128, 14, AlignCenter, AlignTop, "\e#WiFi Scan\e#", false);
        widget_add_text_box_element(
            app->widget, 0, 24, 128, 16, AlignCenter, AlignCenter, "No networks", false);
        widget_add_text_box_element(
            app->widget, 0, 44, 128, 12, AlignCenter, AlignCenter, "Back to retry", false);
        view_dispatcher_switch_to_view(app->view_dispatcher, Nrf24ViewWidget);
        return;
    }

    char buf[40];
    for(uint16_t i = 0; i < app->wifi_ap_count; i++) {
        wifi_ap_record_t* ap = &app->wifi_aps[i];
        const char* ssid = (const char*)ap->ssid;
        if(ssid[0] == '\0') ssid = "<hidden>";
        snprintf(buf, sizeof(buf), "CH%2u  %s", ap->primary, ssid);
        submenu_add_item(app->submenu, buf, i, wifi_scan_submenu_callback, app);
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, Nrf24ViewSubmenu);
}

void nrf24_app_scene_jam_wifi_scan_on_enter(void* context) {
    Nrf24App* app = context;

    /* "Scanning..." splash first; kick off the (blocking) scan on the next
     * event-loop tick. esp_wifi_* must run on the view-dispatcher thread. */
    widget_reset(app->widget);
    widget_add_text_box_element(
        app->widget, 0, 0, 128, 14, AlignCenter, AlignTop, "\e#WiFi Scan\e#", false);
    widget_add_text_box_element(
        app->widget, 0, 24, 128, 16, AlignCenter, AlignCenter, "Scanning...", false);
    widget_add_text_box_element(
        app->widget, 0, 44, 128, 12, AlignCenter, AlignCenter, "please wait", false);
    view_dispatcher_switch_to_view(app->view_dispatcher, Nrf24ViewWidget);

    view_dispatcher_send_custom_event(app->view_dispatcher, SCAN_EVENT_RUN);
}

bool nrf24_app_scene_jam_wifi_scan_on_event(void* context, SceneManagerEvent event) {
    Nrf24App* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == SCAN_EVENT_RUN) {
            if(app->wifi_aps) {
                free(app->wifi_aps);
                app->wifi_aps = NULL;
            }
            app->wifi_ap_count = 0;

            bool ok = nrf24_wifi_scan(&app->wifi_aps, &app->wifi_ap_count, NRF24_WIFI_SCAN_MAX);
            if(ok) {
                /* Cache only on success (even if 0 APs); a failed scan stays
                 * un-cached so the user can retry it. */
                app->jam.wifi_scanned = true;
                app->jam.wifi_index = 0;
                show_results(app);
            } else {
                widget_reset(app->widget);
                widget_add_text_box_element(
                    app->widget, 0, 0, 128, 14, AlignCenter, AlignTop, "\e#WiFi Scan\e#", false);
                widget_add_text_box_element(
                    app->widget, 0, 24, 128, 16, AlignCenter, AlignCenter, "Scan failed", false);
            }
            consumed = true;
        } else if(event.event >= SCAN_EVENT_PICK) {
            app->jam.wifi_index = (uint8_t)(event.event - SCAN_EVENT_PICK);
            /* Return to the jam engine, which rebuilds channels from this AP. */
            scene_manager_previous_scene(app->scene_manager);
            consumed = true;
        }
    }

    return consumed;
}

void nrf24_app_scene_jam_wifi_scan_on_exit(void* context) {
    Nrf24App* app = context;
    submenu_reset(app->submenu);
    widget_reset(app->widget);
}
