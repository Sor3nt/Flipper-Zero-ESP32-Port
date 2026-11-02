#include "../bw16_app_i.h"

#define BW16_ACT_ITEM_BASE (100u)

typedef enum {
    Bw16ActDeauth = 0,
    Bw16ActListClients,
} Bw16StationAct;

static void bw16_scene_station_action_submenu_callback(void* context, uint32_t index) {
    Bw16App* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void bw16_scene_station_action_on_enter(void* context) {
    Bw16App* app = context;
    Submenu* submenu = app->submenu;

    submenu_reset(submenu);

    char header[40];
    const char* ssid = (app->selected_station >= 0 &&
                        (size_t)app->selected_station < app->ap_count) ?
                           app->aps[app->selected_station].ssid :
                           "AP";
    snprintf(header, sizeof(header), "AP: %s", ssid);
    submenu_set_header(submenu, header);

    submenu_add_item(
        submenu,
        "Deauth This AP",
        BW16_ACT_ITEM_BASE + Bw16ActDeauth,
        bw16_scene_station_action_submenu_callback,
        app);
    submenu_add_item(
        submenu,
        "List Clients",
        BW16_ACT_ITEM_BASE + Bw16ActListClients,
        bw16_scene_station_action_submenu_callback,
        app);

    view_dispatcher_switch_to_view(app->view_dispatcher, Bw16AppViewSubmenu);
}

bool bw16_scene_station_action_on_event(void* context, SceneManagerEvent event) {
    Bw16App* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        consumed = true;
        if(event.event < BW16_ACT_ITEM_BASE) {
            return true; /* stray RefreshConsole / *Done: ignore */
        }
        uint32_t act = event.event - BW16_ACT_ITEM_BASE;
        if(act == Bw16ActDeauth) {
            char cmd[BW16_TX_CMD_SIZE];
            snprintf(cmd, sizeof(cmd), "DEAUTH_STATION %d", app->selected_station);
            bw16_app_set_command(app, "TARGETED DEAUTH", cmd, true);
            scene_manager_next_scene(app->scene_manager, Bw16SceneConsole);
        } else if(act == Bw16ActListClients) {
            scene_manager_next_scene(app->scene_manager, Bw16SceneClientList);
        }
    }

    return consumed;
}

void bw16_scene_station_action_on_exit(void* context) {
    Bw16App* app = context;
    submenu_reset(app->submenu);
}
