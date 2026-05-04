#include "../ble_spam_app.h"

static const char* attack_names[] = {
    [BleSpamAttackAppleDevice] = "Apple Device Popup",
    [BleSpamAttackAppleAction] = "Apple Action Modal",
    [BleSpamAttackAppleNotYourDevice] = "Apple NotYourDevice",
    [BleSpamAttackFastPair] = "Google FastPair",
    [BleSpamAttackSwiftPair] = "MS SwiftPair",
    [BleSpamAttackSamsungBuds] = "Samsung Buds",
    [BleSpamAttackSamsungWatch] = "Samsung Watch",
    [BleSpamAttackXiaomi] = "Xiaomi QuickConnect",
    [BleSpamAttackPairSpam] = "Pair Spam",
};

static void spam_menu_callback(void* context, uint32_t index) {
    BleSpamApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void ble_spam_scene_spam_menu_on_enter(void* context) {
    BleSpamApp* app = context;

    for(int i = 0; i < BleSpamAttackCount; i++) {
        submenu_add_item(app->submenu, attack_names[i], i, spam_menu_callback, app);
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, BleSpamViewSubmenu);
}

bool ble_spam_scene_spam_menu_on_event(void* context, SceneManagerEvent event) {
    BleSpamApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event < BleSpamAttackCount) {
            app->attack_type = event.event;
            scene_manager_next_scene(app->scene_manager, BleSpamSceneRunning);
            consumed = true;
        }
    }

    return consumed;
}

void ble_spam_scene_spam_menu_on_exit(void* context) {
    BleSpamApp* app = context;
    submenu_reset(app->submenu);
}
