#include "../bw16_app_i.h"

typedef enum {
    Bw16MenuScan,
    Bw16MenuDeauthAll,
    Bw16MenuTargeted,
    Bw16MenuRandomSsid,
    Bw16MenuBeaconDeauth,
    Bw16MenuNrfJammer,
    Bw16MenuDeauthNrf,
    Bw16MenuEvilPortal,
    Bw16MenuDeauthSniff,
    Bw16MenuAbout,
} Bw16MenuIndex;

typedef struct {
    const char* label;
    const char* title;
    const char* command;
    bool continuous;
} Bw16MenuEntry;

/* command == NULL  -> handled specially (Targeted / About). */
static const Bw16MenuEntry bw16_menu[] = {
    [Bw16MenuScan] = {"Scan Networks", "SCAN NETWORK", "SCAN", false},
    [Bw16MenuDeauthAll] = {"Deauth All", "DEAUTH ALL", "DEAUTH_ALL", true},
    [Bw16MenuTargeted] = {"Targeted Deauth", NULL, NULL, false},
    [Bw16MenuRandomSsid] = {"Random SSID", "RANDOM SSID", "RANDOM_SSID", true},
    [Bw16MenuBeaconDeauth] = {"Beacon + Deauth", "BEACON+DEAUTH", "BEACON_DEAUTH", true},
    [Bw16MenuNrfJammer] = {"NRF24 Jammer", "NRF24 JAMMER", "NRF_START", true},
    [Bw16MenuDeauthNrf] = {"Deauth + NRF", "DEAUTH+NRF", "DEAUTH_NRF_START", true},
    [Bw16MenuEvilPortal] = {"Evil Portal", "EVIL PORTAL", "EVIL_PORTAL_START", true},
    [Bw16MenuDeauthSniff] = {"Deauth Sniff", "DEAUTH SNIFF", "DEAUTH_SNIFF_START", true},
    [Bw16MenuAbout] = {"About", NULL, NULL, false},
};

#define BW16_MENU_COUNT (sizeof(bw16_menu) / sizeof(bw16_menu[0]))

/* Offset so a selection never collides with the global Bw16CustomEvent values (0..2),
 * which are posted to whatever scene is active by the always-on UART rx callback. */
#define BW16_MENU_ITEM_BASE (500u)

static void bw16_scene_start_submenu_callback(void* context, uint32_t index) {
    Bw16App* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, BW16_MENU_ITEM_BASE + index);
}

void bw16_scene_start_on_enter(void* context) {
    Bw16App* app = context;
    Submenu* submenu = app->submenu;

    submenu_reset(submenu);
    submenu_set_header(submenu, "R4TKN BW16");

    for(uint32_t i = 0; i < BW16_MENU_COUNT; i++) {
        submenu_add_item(
            submenu, bw16_menu[i].label, i, bw16_scene_start_submenu_callback, app);
    }

    submenu_set_selected_item(
        submenu, scene_manager_get_scene_state(app->scene_manager, Bw16SceneStart));

    view_dispatcher_switch_to_view(app->view_dispatcher, Bw16AppViewSubmenu);
}

bool bw16_scene_start_on_event(void* context, SceneManagerEvent event) {
    Bw16App* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event < BW16_MENU_ITEM_BASE) {
            /* Stray global event (RefreshConsole / *Done): not a menu selection. */
            return true;
        }
        uint32_t index = event.event - BW16_MENU_ITEM_BASE;
        if(index < BW16_MENU_COUNT) {
            scene_manager_set_scene_state(app->scene_manager, Bw16SceneStart, index);
            consumed = true;

            if(index == Bw16MenuTargeted) {
                scene_manager_next_scene(app->scene_manager, Bw16SceneApList);
            } else if(index == Bw16MenuAbout) {
                scene_manager_next_scene(app->scene_manager, Bw16SceneAbout);
            } else {
                bw16_app_set_command(
                    app,
                    bw16_menu[index].title,
                    bw16_menu[index].command,
                    bw16_menu[index].continuous);
                scene_manager_next_scene(app->scene_manager, Bw16SceneConsole);
            }
        }
    }

    return consumed;
}

void bw16_scene_start_on_exit(void* context) {
    Bw16App* app = context;
    submenu_reset(app->submenu);
}
