#include "../wifi_app.h"
#include <stdio.h>
#include <string.h>

enum EvilPortalMenuIndex {
    EvilPortalMenuIndexSsid,
    EvilPortalMenuIndexChannel,
    EvilPortalMenuIndexTemplate,
    EvilPortalMenuIndexStart,
};

#define CHANNEL_COUNT 12
static const char* const channel_text[CHANNEL_COUNT] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12",
};

static const char* const template_text[] = {"Google", "Router", "SD"};
#define TEMPLATE_COUNT 3

static uint8_t channel_index(uint8_t channel) {
    if(channel >= 1 && channel <= CHANNEL_COUNT) return (uint8_t)(channel - 1);
    return 5; // default 6
}

static void evil_portal_menu_set_channel(VariableItem* item) {
    WifiApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    app->evil_portal_channel = (uint8_t)(idx + 1);
    variable_item_set_current_value_text(item, channel_text[idx]);
}

static void evil_portal_menu_set_template(VariableItem* item) {
    WifiApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    app->evil_portal_template = (WifiAppEvilPortalTemplate)idx;
    variable_item_set_current_value_text(item, template_text[idx]);
    if(app->evil_portal_template == WifiAppEvilPortalTemplateSd &&
       app->evil_portal_sd_path[0] == '\0') {
        strncpy(
            app->evil_portal_sd_path,
            "/ext/wifi/evil_portal/custom.html",
            sizeof(app->evil_portal_sd_path) - 1);
        app->evil_portal_sd_path[sizeof(app->evil_portal_sd_path) - 1] = '\0';
    }
}

static void evil_portal_menu_enter_callback(void* context, uint32_t index) {
    WifiApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void wifi_app_scene_evil_portal_menu_on_enter(void* context) {
    WifiApp* app = context;

    if(app->evil_portal_ssid[0] == 0) {
        strcpy(app->evil_portal_ssid, "Free WiFi");
    }
    if(app->evil_portal_channel == 0) {
        app->evil_portal_channel = 6;
    }

    VariableItem* item;

    item = variable_item_list_add(app->variable_item_list, "SSID", 1, NULL, app);
    variable_item_set_current_value_text(item, app->evil_portal_ssid);

    item = variable_item_list_add(
        app->variable_item_list, "Channel", CHANNEL_COUNT, evil_portal_menu_set_channel, app);
    {
        uint8_t idx = channel_index(app->evil_portal_channel);
        variable_item_set_current_value_index(item, idx);
        variable_item_set_current_value_text(item, channel_text[idx]);
    }

    item = variable_item_list_add(
        app->variable_item_list, "Template", TEMPLATE_COUNT, evil_portal_menu_set_template, app);
    {
        uint8_t idx = (uint8_t)app->evil_portal_template;
        if(idx >= TEMPLATE_COUNT) idx = 0;
        variable_item_set_current_value_index(item, idx);
        variable_item_set_current_value_text(item, template_text[idx]);
    }

    variable_item_list_add(app->variable_item_list, "Start", 1, NULL, app);

    variable_item_list_set_enter_callback(
        app->variable_item_list, evil_portal_menu_enter_callback, app);

    uint32_t selected =
        scene_manager_get_scene_state(app->scene_manager, WifiAppSceneEvilPortalMenu);
    if(selected > EvilPortalMenuIndexStart) selected = 0;
    variable_item_list_set_selected_item(app->variable_item_list, (uint8_t)selected);

    view_dispatcher_switch_to_view(app->view_dispatcher, WifiAppViewVariableItemList);
}

bool wifi_app_scene_evil_portal_menu_on_event(void* context, SceneManagerEvent event) {
    WifiApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case EvilPortalMenuIndexSsid:
            scene_manager_set_scene_state(
                app->scene_manager, WifiAppSceneEvilPortalMenu, EvilPortalMenuIndexSsid);
            scene_manager_next_scene(app->scene_manager, WifiAppSceneEvilPortalSsid);
            consumed = true;
            break;
        case EvilPortalMenuIndexStart:
            scene_manager_set_scene_state(
                app->scene_manager, WifiAppSceneEvilPortalMenu, EvilPortalMenuIndexStart);
            scene_manager_next_scene(app->scene_manager, WifiAppSceneEvilPortalRun);
            consumed = true;
            break;
        }
    }

    return consumed;
}

void wifi_app_scene_evil_portal_menu_on_exit(void* context) {
    WifiApp* app = context;
    uint8_t selected = variable_item_list_get_selected_item_index(app->variable_item_list);
    scene_manager_set_scene_state(app->scene_manager, WifiAppSceneEvilPortalMenu, selected);
    variable_item_list_reset(app->variable_item_list);
}
