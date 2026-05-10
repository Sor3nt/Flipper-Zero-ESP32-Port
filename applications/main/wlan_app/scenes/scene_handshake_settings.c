#include "../wlan_app.h"

enum HsSettingsIndex {
    HsSetIdxChannel,
    HsSetIdxHopping,
    HsSetIdxSavePath,
};

#define HS_CHANNEL_COUNT 13
static const char* const channel_text[HS_CHANNEL_COUNT] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13",
};

#define ON_OFF_COUNT 2
static const char* const on_off_text[ON_OFF_COUNT] = {"OFF", "ON"};

static void hs_set_channel_cb(VariableItem* item) {
    WlanApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    if(idx >= HS_CHANNEL_COUNT) idx = HS_CHANNEL_COUNT - 1;
    app->hs_settings.channel = (uint8_t)(idx + 1);
    variable_item_set_current_value_text(item, channel_text[idx]);
    wlan_handshake_settings_save(&app->hs_settings);
}

static void hs_set_hopping_cb(VariableItem* item) {
    WlanApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    if(idx >= ON_OFF_COUNT) idx = ON_OFF_COUNT - 1;
    app->hs_settings.hopping = (idx == 1);
    variable_item_set_current_value_text(item, on_off_text[idx]);
    wlan_handshake_settings_save(&app->hs_settings);
}

static void hs_settings_enter_cb(void* context, uint32_t index) {
    WlanApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void wlan_app_scene_handshake_settings_on_enter(void* context) {
    WlanApp* app = context;
    VariableItemList* list = app->variable_item_list;
    variable_item_list_reset(list);

    VariableItem* it;

    // Channel
    it = variable_item_list_add(list, "Channel", HS_CHANNEL_COUNT, hs_set_channel_cb, app);
    {
        uint8_t ch = app->hs_settings.channel;
        if(ch < 1) ch = 1;
        if(ch > HS_CHANNEL_COUNT) ch = HS_CHANNEL_COUNT;
        variable_item_set_current_value_index(it, ch - 1);
        variable_item_set_current_value_text(it, channel_text[ch - 1]);
    }

    // Hopping
    it = variable_item_list_add(list, "Hopping", ON_OFF_COUNT, hs_set_hopping_cb, app);
    {
        uint8_t idx = app->hs_settings.hopping ? 1 : 0;
        variable_item_set_current_value_index(it, idx);
        variable_item_set_current_value_text(it, on_off_text[idx]);
    }

    // Save to (Klick → TextInput-Scene)
    variable_item_list_add(list, "Save to", 1, NULL, app);

    variable_item_list_set_enter_callback(list, hs_settings_enter_cb, app);
    variable_item_list_set_selected_item(
        list, scene_manager_get_scene_state(app->scene_manager, WlanAppSceneHandshakeSettings));

    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewVariableItemList);
}

bool wlan_app_scene_handshake_settings_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case HsSetIdxSavePath:
            scene_manager_set_scene_state(
                app->scene_manager, WlanAppSceneHandshakeSettings, HsSetIdxSavePath);
            scene_manager_next_scene(app->scene_manager, WlanAppSceneHandshakeSavePath);
            consumed = true;
            break;
        default:
            break;
        }
    }
    return consumed;
}

void wlan_app_scene_handshake_settings_on_exit(void* context) {
    WlanApp* app = context;
    variable_item_list_reset(app->variable_item_list);
}
