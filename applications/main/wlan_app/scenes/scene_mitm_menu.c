#include "../wlan_app.h"

// Menu-Layout ist dynamisch: das "Code"-Item erscheint nur, wenn als Payload
// "custom" gewählt ist. Damit der Enter-Callback weiss, was an einer gegebenen
// Position liegt, mappen wir Position → Action über eine kleine Tabelle, die
// beim Aufbau gefüllt wird.
typedef enum {
    MitmAct_Inject = 0,
    MitmAct_Payload,
    MitmAct_Code,
    MitmAct_StoreCred,
    MitmAct_Start,
    MitmAct_None = 0xFF,
} MitmAction;

#define MITM_MENU_MAX_ITEMS 5

static const char* const onoff_text[2] = {"Off", "On"};
static const char* const k_custom_label = "custom";

static MitmAction s_pos_to_act[MITM_MENU_MAX_ITEMS];
static uint8_t s_item_count;
static VariableItem* s_item_inject_code;

static bool payload_is_custom(WlanApp* app) {
    return app->mitm_payload_index >= app->mitm_payloads.count;
}

static const char* payload_label_for_index(WlanApp* app, uint8_t idx) {
    if(idx < app->mitm_payloads.count) return app->mitm_payloads.items[idx].name;
    return k_custom_label;
}

static void mitm_menu_refresh_code_item(WlanApp* app) {
    if(!s_item_inject_code) return;
    if(!app->mitm_inject_enabled) {
        variable_item_set_current_value_text(s_item_inject_code, "(disabled)");
    } else {
        variable_item_set_current_value_text(s_item_inject_code, app->mitm_inject_code);
    }
}

static void mitm_menu_set_inject(VariableItem* item) {
    WlanApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    app->mitm_inject_enabled = (idx == 1);
    variable_item_set_current_value_text(item, onoff_text[idx]);
    mitm_menu_refresh_code_item(app);
}

static void mitm_menu_set_payload(VariableItem* item) {
    WlanApp* app = variable_item_get_context(item);
    uint8_t new_idx = variable_item_get_current_value_index(item);
    bool was_custom = payload_is_custom(app);
    app->mitm_payload_index = new_idx;
    bool now_custom = payload_is_custom(app);
    variable_item_set_current_value_text(item, payload_label_for_index(app, new_idx));
    if(was_custom != now_custom) {
        // Liste-Layout ändert sich (Code-Item kommt/geht). Reset des Lists hier
        // wäre Use-after-free im laufenden VariableItem-Callback — daher per
        // Custom-Event aus dem View-Input-Stack rauspendeln und im
        // on_event-Handler rebuilden.
        view_dispatcher_send_custom_event(
            app->view_dispatcher, WlanAppCustomEventMitmMenuPayloadChanged);
    }
}

static void mitm_menu_set_store(VariableItem* item) {
    WlanApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    app->mitm_store_cred = (idx == 1);
    variable_item_set_current_value_text(item, onoff_text[idx]);
}

static void mitm_menu_enter_cb(void* context, uint32_t index) {
    WlanApp* app = context;
    if(index >= s_item_count) return;
    MitmAction act = s_pos_to_act[index];
    switch(act) {
    case MitmAct_Code:
        view_dispatcher_send_custom_event(
            app->view_dispatcher, WlanAppCustomEventMitmMenuInjectCode);
        break;
    case MitmAct_Start:
        view_dispatcher_send_custom_event(
            app->view_dispatcher, WlanAppCustomEventMitmMenuStart);
        break;
    default:
        break;
    }
}

// Baut die Variable-Item-Liste aus dem aktuellen App-State auf. Setzt
// s_pos_to_act / s_item_count / s_item_inject_code als Seiten-Effekt.
static void mitm_menu_build(WlanApp* app, uint8_t selected_pos) {
    variable_item_list_reset(app->variable_item_list);
    s_item_inject_code = NULL;
    s_item_count = 0;
    for(uint8_t i = 0; i < MITM_MENU_MAX_ITEMS; i++) s_pos_to_act[i] = MitmAct_None;

    VariableItem* item;

    item = variable_item_list_add(
        app->variable_item_list, "Inject", 2, mitm_menu_set_inject, app);
    variable_item_set_current_value_index(item, app->mitm_inject_enabled ? 1 : 0);
    variable_item_set_current_value_text(item, onoff_text[app->mitm_inject_enabled ? 1 : 0]);
    s_pos_to_act[s_item_count++] = MitmAct_Inject;

    // Payload-Selector: alle SD-Files plus "custom" als letzter Eintrag.
    uint8_t payload_options = (uint8_t)(app->mitm_payloads.count + 1);
    if(app->mitm_payload_index >= payload_options) {
        app->mitm_payload_index = (uint8_t)(payload_options - 1); // → custom
    }
    item = variable_item_list_add(
        app->variable_item_list, "Payload", payload_options, mitm_menu_set_payload, app);
    variable_item_set_current_value_index(item, app->mitm_payload_index);
    variable_item_set_current_value_text(
        item, payload_label_for_index(app, app->mitm_payload_index));
    s_pos_to_act[s_item_count++] = MitmAct_Payload;

    if(payload_is_custom(app)) {
        s_item_inject_code = variable_item_list_add(
            app->variable_item_list, "Code", 1, NULL, app);
        mitm_menu_refresh_code_item(app);
        s_pos_to_act[s_item_count++] = MitmAct_Code;
    }

    item = variable_item_list_add(
        app->variable_item_list, "Store Cred", 2, mitm_menu_set_store, app);
    variable_item_set_current_value_index(item, app->mitm_store_cred ? 1 : 0);
    variable_item_set_current_value_text(item, onoff_text[app->mitm_store_cred ? 1 : 0]);
    s_pos_to_act[s_item_count++] = MitmAct_StoreCred;

    variable_item_list_add(app->variable_item_list, "Start", 1, NULL, app);
    s_pos_to_act[s_item_count++] = MitmAct_Start;

    variable_item_list_set_enter_callback(app->variable_item_list, mitm_menu_enter_cb, app);

    if(selected_pos >= s_item_count) selected_pos = 0;
    variable_item_list_set_selected_item(app->variable_item_list, selected_pos);
}

void wlan_app_scene_mitm_menu_on_enter(void* context) {
    WlanApp* app = context;

    wlan_mitm_payloads_scan(&app->mitm_payloads);
    // Default beim ersten Betreten: "custom". Bleibt auch nach Re-Scan stehen,
    // wenn der User schon einen anderen Wert gewählt hatte (Index wird in
    // mitm_menu_build geclampt).
    if(app->mitm_payload_index == 0 && app->mitm_payloads.count > 0) {
        app->mitm_payload_index = app->mitm_payloads.count; // → "custom"
    }

    uint32_t selected = scene_manager_get_scene_state(app->scene_manager, WlanAppSceneMitmMenu);
    mitm_menu_build(app, (uint8_t)selected);

    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewVariableItemList);
}

bool wlan_app_scene_mitm_menu_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case WlanAppCustomEventMitmMenuPayloadChanged: {
            // Payload-Item ist immer an Position 1 — Selektion dort halten.
            mitm_menu_build(app, 1);
            consumed = true;
            break;
        }
        case WlanAppCustomEventMitmMenuInjectCode: {
            // Beim Verlassen den Payload-Index merken, sodass on_enter nach
            // Rückkehr nicht auf "custom" zurückspringt.
            uint8_t pos = variable_item_list_get_selected_item_index(app->variable_item_list);
            scene_manager_set_scene_state(app->scene_manager, WlanAppSceneMitmMenu, pos);
            scene_manager_next_scene(app->scene_manager, WlanAppSceneMitmInjectCode);
            consumed = true;
            break;
        }
        case WlanAppCustomEventMitmMenuStart: {
            uint8_t pos = variable_item_list_get_selected_item_index(app->variable_item_list);
            scene_manager_set_scene_state(app->scene_manager, WlanAppSceneMitmMenu, pos);
            scene_manager_next_scene(app->scene_manager, WlanAppSceneLiveCreds);
            consumed = true;
            break;
        }
        default:
            break;
        }
    }

    return consumed;
}

void wlan_app_scene_mitm_menu_on_exit(void* context) {
    WlanApp* app = context;
    variable_item_list_reset(app->variable_item_list);
    s_item_inject_code = NULL;
    s_item_count = 0;
    UNUSED(app);
}
