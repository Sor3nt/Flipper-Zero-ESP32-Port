#include "../ble_spam_app.h"

enum MainMenuIndex {
    MainMenuIndexBleSpam,
    MainMenuIndexBleWalk,
    MainMenuIndexBleAutoWalk,
    MainMenuIndexBleBrute,
    MainMenuIndexBleClone,
    MainMenuIndexBleTracker,
};

static void main_menu_callback(void* context, uint32_t index) {
    BleSpamApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void ble_spam_scene_main_on_enter(void* context) {
    BleSpamApp* app = context;

    submenu_add_item(app->submenu, "Spam", MainMenuIndexBleSpam, main_menu_callback, app);
    submenu_add_item(app->submenu, "Walk", MainMenuIndexBleWalk, main_menu_callback, app);
    submenu_add_item(
        app->submenu, "Auto Walk", MainMenuIndexBleAutoWalk, main_menu_callback, app);
    submenu_add_item(
        app->submenu, "Brute", MainMenuIndexBleBrute, main_menu_callback, app);
    submenu_add_item(
        app->submenu, "Tracker", MainMenuIndexBleTracker, main_menu_callback, app);
    submenu_add_item(app->submenu, "Clone", MainMenuIndexBleClone, main_menu_callback, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, BleSpamViewSubmenu);
}

bool ble_spam_scene_main_on_event(void* context, SceneManagerEvent event) {
    BleSpamApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case MainMenuIndexBleSpam:
            scene_manager_next_scene(app->scene_manager, BleSpamSceneSpamMenu);
            consumed = true;
            break;
        case MainMenuIndexBleWalk:
            scene_manager_next_scene(app->scene_manager, BleSpamSceneWalkScan);
            consumed = true;
            break;
        case MainMenuIndexBleAutoWalk:
            scene_manager_next_scene(app->scene_manager, BleSpamSceneAutoWalk);
            consumed = true;
            break;
        case MainMenuIndexBleBrute:
            scene_manager_next_scene(app->scene_manager, BleSpamSceneBruteScan);
            consumed = true;
            break;
        case MainMenuIndexBleClone:
            scene_manager_next_scene(app->scene_manager, BleSpamSceneCloneScan);
            consumed = true;
            break;
        case MainMenuIndexBleTracker:
            scene_manager_next_scene(app->scene_manager, BleSpamSceneTrackerScan);
            consumed = true;
            break;
        }
    }

    return consumed;
}

void ble_spam_scene_main_on_exit(void* context) {
    BleSpamApp* app = context;
    submenu_reset(app->submenu);
}
