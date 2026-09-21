/* AirPrint config: Printernamen ändern (TextInput). Der Name wird persistiert
 * (wlan_airprint_set_name → /ext/wifi/printer/config.txt) und wirkt beim
 * nächsten Start des Servers (Rückkehr zur Print-Scene startet ihn neu). */

#include "../wlan_app.h"
#include "../wlan_airprint.h"

#include <string.h>

static void airprint_config_result_cb(void* context) {
    WlanApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, WlanAppCustomEventAirPrintNameDone);
}

void wlan_app_scene_airprint_config_on_enter(void* context) {
    WlanApp* app = context;
    TextInput* ti = app->text_input;

    /* aktuellen Namen vorbelegen */
    strncpy(app->airprint_name, wlan_airprint_get_name(), sizeof(app->airprint_name) - 1);
    app->airprint_name[sizeof(app->airprint_name) - 1] = 0;

    text_input_reset(ti);
    text_input_set_header_text(ti, "Printer Name");
    text_input_set_minimum_length(ti, 1);
    text_input_set_result_callback(
        ti, airprint_config_result_cb, app, app->airprint_name, sizeof(app->airprint_name), false);

    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewTextInput);
}

bool wlan_app_scene_airprint_config_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;
    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == WlanAppCustomEventAirPrintNameDone) {
            wlan_airprint_set_name(app->airprint_name);
            scene_manager_previous_scene(app->scene_manager);
            consumed = true;
        }
    }
    return consumed;
}

void wlan_app_scene_airprint_config_on_exit(void* context) {
    WlanApp* app = context;
    text_input_reset(app->text_input);
}
