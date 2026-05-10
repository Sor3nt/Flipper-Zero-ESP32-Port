#include "../wlan_app.h"
#include "../wlan_netcut.h"

// Mit Goto verlinkte Items kommen via custom event in den Event-Handler.
// Toggle-Items mutieren direkt das App-State; Restore-Logik beim onExit
// kommt in Session 3.

enum AttackItemIndex {
    AtIdxBlockInternet,
    AtIdxThrottle,
    AtIdxPortScanner,
    AtIdxPackageSniffer,
    AtIdxHandshake,
    AtIdxSsidSpam,
    AtIdxEvilPortal,
    AtIdxCount,
};

static const char* const throttle_labels[WlanAppThrottleCount] = {
    "Off", "16", "32", "64", "128", "256", "512", "1024",
};

static const uint16_t throttle_kbps_values[WlanAppThrottleCount] = {
    0, 16, 32, 64, 128, 256, 512, 1024,
};

// Cache der VariableItems, damit sich Block/Throttle gegenseitig deaktivieren
// können (exklusive Modi). Werden beim on_enter gesetzt, beim on_exit gelöscht.
static VariableItem* s_block_item = NULL;
static VariableItem* s_throttle_item = NULL;

#define AT_RESTORING_TICKS 12 // ~3 s
static uint16_t s_at_restoring_ticks;

static void at_show_restoring(WlanApp* app) {
    widget_reset(app->widget);
    widget_add_rect_element(app->widget, 14, 22, 100, 20, 3, false);
    widget_add_string_element(
        app->widget, 64, 32, AlignCenter, AlignCenter, FontSecondary, "Restoring device...");
    s_at_restoring_ticks = AT_RESTORING_TICKS;
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewWidget);
}

static void at_finish_restoring(WlanApp* app) {
    widget_reset(app->widget);
    s_at_restoring_ticks = 0;
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewVariableItemList);
}

static void label_bool(VariableItem* item, bool v) {
    variable_item_set_current_value_text(item, v ? "ON" : "OFF");
}

// ON wendet auf Targets an; OFF cleart über alle Devices, damit der globale
// Toggle nicht im Konflikt mit per Long-OK gesetzten VIP-Blocks/Throttles
// stehenbleibt (Bug 8).
static void apply_block_to_targets(WlanApp* app, bool on) {
    for(uint16_t i = 0; i < app->device_count; ++i) {
        if(!on) {
            app->devices[i].block_internet = false;
            continue;
        }
        if(!app->devices[i].active) continue;
        app->devices[i].block_internet = true;
        app->devices[i].throttle_kbps = 0; // exklusiv
    }
}

static void apply_throttle_to_targets(WlanApp* app, uint16_t kbps) {
    for(uint16_t i = 0; i < app->device_count; ++i) {
        if(kbps == 0) {
            app->devices[i].throttle_kbps = 0;
            continue;
        }
        if(!app->devices[i].active) continue;
        app->devices[i].throttle_kbps = kbps;
        app->devices[i].block_internet = false; // exklusiv
    }
}

static void cb_block_internet(VariableItem* item) {
    WlanApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    bool on = (idx != 0);
    app->attack_block_internet = on;
    apply_block_to_targets(app, on);
    label_bool(item, on);

    // Exklusiv: Block aktiv → Throttle auf Off zurücksetzen.
    if(on && s_throttle_item) {
        variable_item_set_current_value_index(s_throttle_item, 0);
        variable_item_set_current_value_text(s_throttle_item, throttle_labels[0]);
        app->attack_throttle = WlanAppThrottleOff;
        // apply_block_to_targets hat throttle_kbps bereits auf 0 gesetzt.
    }

    {
        bool r = wlan_netcut_apply(app->netcut, app->devices, app->device_count);
        if(r) at_show_restoring(app);
    }
}

static void cb_throttle(VariableItem* item) {
    WlanApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    if(idx >= WlanAppThrottleCount) idx = WlanAppThrottleCount - 1;
    app->attack_throttle = (WlanAppThrottleLevel)idx;
    apply_throttle_to_targets(app, throttle_kbps_values[idx]);
    variable_item_set_current_value_text(item, throttle_labels[idx]);

    // Exklusiv: Throttle aktiv → Block auf Off zurücksetzen.
    if(idx != 0 && s_block_item) {
        variable_item_set_current_value_index(s_block_item, 0);
        label_bool(s_block_item, false);
        app->attack_block_internet = false;
        // apply_throttle_to_targets hat block_internet bereits auf false gesetzt.
    }

    {
        bool r = wlan_netcut_apply(app->netcut, app->devices, app->device_count);
        if(r) at_show_restoring(app);
    }
}

static void enter_cb(void* context, uint32_t index) {
    WlanApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void wlan_app_scene_attack_targets_on_enter(void* context) {
    WlanApp* app = context;
    VariableItemList* list = app->variable_item_list;
    variable_item_list_reset(list);

    // Initial-State aus den TARGET Devices ableiten, damit Status auch dann
    // korrekt ist, wenn ein Device-Status z.B. via LAN-Long-OK gesetzt wurde.
    bool any_block = false;
    uint16_t common_kbps = 0;
    bool kbps_seen = false;
    for(uint16_t i = 0; i < app->device_count; ++i) {
        if(!app->devices[i].active) continue;
        if(app->devices[i].block_internet) any_block = true;
        if(!kbps_seen) {
            common_kbps = app->devices[i].throttle_kbps;
            kbps_seen = true;
        } else if(app->devices[i].throttle_kbps != common_kbps) {
            common_kbps = 0; // gemischt → Off anzeigen
        }
    }
    app->attack_block_internet = any_block;
    app->attack_throttle = WlanAppThrottleOff;
    for(uint8_t i = 0; i < WlanAppThrottleCount; ++i) {
        if(throttle_kbps_values[i] == common_kbps) {
            app->attack_throttle = (WlanAppThrottleLevel)i;
            break;
        }
    }

    s_block_item = variable_item_list_add(list, "Block Internet", 2, cb_block_internet, app);
    variable_item_set_current_value_index(s_block_item, app->attack_block_internet ? 1 : 0);
    label_bool(s_block_item, app->attack_block_internet);

    s_throttle_item =
        variable_item_list_add(list, "Throttle", WlanAppThrottleCount, cb_throttle, app);
    variable_item_set_current_value_index(s_throttle_item, app->attack_throttle);
    variable_item_set_current_value_text(s_throttle_item, throttle_labels[app->attack_throttle]);

    variable_item_list_add(list, "Port Scanner", 0, NULL, app);
    variable_item_list_add(list, "Package Sniffer", 0, NULL, app);
    variable_item_list_add(list, "Capture Handshake", 0, NULL, app);

    variable_item_list_set_enter_callback(list, enter_cb, app);
    variable_item_list_set_selected_item(
        list, scene_manager_get_scene_state(app->scene_manager, WlanAppSceneAttackTargets));

    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewVariableItemList);
}

bool wlan_app_scene_attack_targets_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeTick) {
        if(s_at_restoring_ticks > 0) {
            s_at_restoring_ticks--;
            if(s_at_restoring_ticks == 0) at_finish_restoring(app);
        }
        return false;
    }

    if(event.type == SceneManagerEventTypeCustom) {
        scene_manager_set_scene_state(app->scene_manager, WlanAppSceneAttackTargets, event.event);
        switch(event.event) {
        case AtIdxPortScanner:
            scene_manager_next_scene(app->scene_manager, WlanAppScenePortScanner);
            consumed = true;
            break;
        case AtIdxPackageSniffer:
            scene_manager_next_scene(app->scene_manager, WlanAppScenePackageSniffer);
            consumed = true;
            break;
        case AtIdxHandshake:
            // Single-Target-Mode: Handshake auf konkretem AP, kein Channel-Hopping.
            app->channel_mode_active = false;
            scene_manager_next_scene(app->scene_manager, WlanAppSceneHandshake);
            consumed = true;
            break;
        case AtIdxSsidSpam:
            scene_manager_next_scene(app->scene_manager, WlanAppSceneSsidSpam);
            consumed = true;
            break;
        case AtIdxEvilPortal:
            scene_manager_next_scene(app->scene_manager, WlanAppSceneEvilPortalMenu);
            consumed = true;
            break;
        default:
            break;
        }
    }

    return consumed;
}

void wlan_app_scene_attack_targets_on_exit(void* context) {
    WlanApp* app = context;
    s_block_item = NULL;
    s_throttle_item = NULL;
    if(s_at_restoring_ticks > 0) {
        widget_reset(app->widget);
        s_at_restoring_ticks = 0;
    }
    variable_item_list_reset(app->variable_item_list);
}
