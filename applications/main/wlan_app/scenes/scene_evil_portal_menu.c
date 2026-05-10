#include "../wlan_app.h"

enum EvilPortalMenuIndex {
    EpMenuIdxSsid,
    EpMenuIdxChannel,
    EpMenuIdxTemplate,
    EpMenuIdxStart,
};

#define EP_CHANNEL_COUNT 12
static const char* const channel_text[EP_CHANNEL_COUNT] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12",
};

#define EP_TEMPLATE_COUNT 2
static const char* const template_names[EP_TEMPLATE_COUNT] = {
    "Google",
    "Router",
};

static uint8_t channel_index(uint8_t channel) {
    if(channel >= 1 && channel <= EP_CHANNEL_COUNT) return (uint8_t)(channel - 1);
    return 5; // default 6
}

static void ep_menu_set_channel(VariableItem* item) {
    WlanApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    app->evil_portal_channel = (uint8_t)(idx + 1);
    variable_item_set_current_value_text(item, channel_text[idx]);
}

static void ep_menu_set_template(VariableItem* item) {
    WlanApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    if(idx >= EP_TEMPLATE_COUNT) idx = EP_TEMPLATE_COUNT - 1;
    app->evil_portal_template_index = idx;
    variable_item_set_current_value_text(item, template_names[idx]);
}

static void ep_menu_enter_cb(void* context, uint32_t index) {
    WlanApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void wlan_app_scene_evil_portal_menu_on_enter(void* context) {
    WlanApp* app = context;

    if(app->evil_portal_ssid[0] == 0) {
        strcpy(app->evil_portal_ssid, "Free WiFi");
    }
    if(app->evil_portal_channel == 0) {
        app->evil_portal_channel = 6;
    }
    if(app->evil_portal_template_index >= EP_TEMPLATE_COUNT) {
        app->evil_portal_template_index = 0;
    }

    VariableItem* item;

    item = variable_item_list_add(app->variable_item_list, "SSID", 1, NULL, app);
    variable_item_set_current_value_text(item, app->evil_portal_ssid);

    item = variable_item_list_add(
        app->variable_item_list, "Channel", EP_CHANNEL_COUNT, ep_menu_set_channel, app);
    {
        uint8_t idx = channel_index(app->evil_portal_channel);
        variable_item_set_current_value_index(item, idx);
        variable_item_set_current_value_text(item, channel_text[idx]);
    }

    item = variable_item_list_add(
        app->variable_item_list, "Template", EP_TEMPLATE_COUNT, ep_menu_set_template, app);
    {
        uint8_t idx = app->evil_portal_template_index;
        variable_item_set_current_value_index(item, idx);
        variable_item_set_current_value_text(item, template_names[idx]);
    }

    variable_item_list_add(app->variable_item_list, "Start", 1, NULL, app);

    variable_item_list_set_enter_callback(
        app->variable_item_list, ep_menu_enter_cb, app);

    uint32_t selected =
        scene_manager_get_scene_state(app->scene_manager, WlanAppSceneEvilPortalMenu);
    if(selected > EpMenuIdxStart) selected = 0;
    variable_item_list_set_selected_item(app->variable_item_list, (uint8_t)selected);

    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewVariableItemList);
}

bool wlan_app_scene_evil_portal_menu_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case EpMenuIdxSsid:
            scene_manager_set_scene_state(
                app->scene_manager, WlanAppSceneEvilPortalMenu, EpMenuIdxSsid);
            scene_manager_next_scene(app->scene_manager, WlanAppSceneEvilPortalSsid);
            consumed = true;
            break;
        case EpMenuIdxStart:
            scene_manager_set_scene_state(
                app->scene_manager, WlanAppSceneEvilPortalMenu, EpMenuIdxStart);
            scene_manager_next_scene(app->scene_manager, WlanAppSceneEvilPortal);
            consumed = true;
            break;
        }
    }

    return consumed;
}

void wlan_app_scene_evil_portal_menu_on_exit(void* context) {
    WlanApp* app = context;
    variable_item_list_reset(app->variable_item_list);
}
