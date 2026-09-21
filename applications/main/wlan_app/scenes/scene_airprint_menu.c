/* AirPrint entry menu: derzeit nur „Print to Flipper" → startet die
 * Print-/Status-Scene (scene_airprint_info). Weitere Einträge folgen bei Bedarf. */

#include "../wlan_app.h"

enum AirPrintMenuIndex {
    AirPrintMenuPrint,
    AirPrintMenuSpam,
    AirPrintMenuHijack,
};

static void airprint_menu_cb(void* context, uint32_t index) {
    WlanApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void wlan_app_scene_airprint_menu_on_enter(void* context) {
    WlanApp* app = context;
    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, "AirPrint");
    submenu_add_item(app->submenu, "Print to Flipper", AirPrintMenuPrint, airprint_menu_cb, app);
    submenu_add_item(app->submenu, "Spam Printers", AirPrintMenuSpam, airprint_menu_cb, app);
    submenu_add_item(app->submenu, "Hijack Printer", AirPrintMenuHijack, airprint_menu_cb, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewSubmenu);
}

bool wlan_app_scene_airprint_menu_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;
    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == AirPrintMenuPrint) {
            /* Scene-State 0 = Normalbetrieb (ein Drucker) */
            scene_manager_set_scene_state(app->scene_manager, WlanAppSceneAirPrintInfo, 0);
            scene_manager_next_scene(app->scene_manager, WlanAppSceneAirPrintInfo);
            consumed = true;
        } else if(event.event == AirPrintMenuSpam) {
            /* Scene-State 1 = Spam-Modus (viele Drucker aus spam.txt) */
            scene_manager_set_scene_state(app->scene_manager, WlanAppSceneAirPrintInfo, 1);
            scene_manager_next_scene(app->scene_manager, WlanAppSceneAirPrintInfo);
            consumed = true;
        } else if(event.event == AirPrintMenuHijack) {
            scene_manager_next_scene(app->scene_manager, WlanAppSceneAirPrintHijackScan);
            consumed = true;
        }
    }
    return consumed;
}

void wlan_app_scene_airprint_menu_on_exit(void* context) {
    WlanApp* app = context;
    submenu_reset(app->submenu);
}
