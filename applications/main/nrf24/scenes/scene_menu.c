#include "../nrf24_app.h"

enum SubmenuIndex {
    SubmenuIndexInfo,
    SubmenuIndexSpectrum,
    SubmenuIndexJammer,
    SubmenuIndexWifiJammer,
};

static void nrf24_scene_menu_submenu_callback(void* context, uint32_t index) {
    Nrf24App* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void nrf24_app_scene_menu_on_enter(void* context) {
    Nrf24App* app = context;

    submenu_set_header(app->submenu, "NRF24");
    submenu_add_item(
        app->submenu, "Information", SubmenuIndexInfo, nrf24_scene_menu_submenu_callback, app);
    submenu_add_item(
        app->submenu, "Spectrum", SubmenuIndexSpectrum, nrf24_scene_menu_submenu_callback, app);
    submenu_add_item(
        app->submenu, "Jammer", SubmenuIndexJammer, nrf24_scene_menu_submenu_callback, app);
    submenu_add_item(
        app->submenu,
        "WiFi Jammer",
        SubmenuIndexWifiJammer,
        nrf24_scene_menu_submenu_callback,
        app);

    view_dispatcher_switch_to_view(app->view_dispatcher, Nrf24ViewSubmenu);
}

bool nrf24_app_scene_menu_on_event(void* context, SceneManagerEvent event) {
    Nrf24App* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case SubmenuIndexInfo:
            scene_manager_next_scene(app->scene_manager, Nrf24AppSceneInfo);
            consumed = true;
            break;
        case SubmenuIndexSpectrum:
            scene_manager_next_scene(app->scene_manager, Nrf24AppSceneSpectrum);
            consumed = true;
            break;
        case SubmenuIndexJammer:
            scene_manager_next_scene(app->scene_manager, Nrf24AppSceneChJammer);
            consumed = true;
            break;
        case SubmenuIndexWifiJammer:
            scene_manager_next_scene(app->scene_manager, Nrf24AppSceneWifiScan);
            consumed = true;
            break;
        default:
            break;
        }
    }

    return consumed;
}

void nrf24_app_scene_menu_on_exit(void* context) {
    Nrf24App* app = context;
    submenu_reset(app->submenu);
}
