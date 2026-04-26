#include "../wifi_app.h"
#include <stdio.h>
#include <string.h>
#include <storage/storage.h>

#define LOGIN_TEMPLATES_DIR  "/ext/wifi/evil_portal/login_template"
#define ROUTER_TEMPLATES_DIR "/ext/wifi/evil_portal/router_template"

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

static uint8_t channel_index(uint8_t channel) {
    if(channel >= 1 && channel <= CHANNEL_COUNT) return (uint8_t)(channel - 1);
    return 5; // default 6
}

static bool ends_with_html_ci(const char* s) {
    size_t n = strlen(s);
    if(n <= 5) return false;
    const char* ext = s + n - 5;
    return (ext[0] == '.') && ((ext[1] | 0x20) == 'h') && ((ext[2] | 0x20) == 't') &&
           ((ext[3] | 0x20) == 'm') && ((ext[4] | 0x20) == 'l');
}

static void scan_dir(WifiApp* app, Storage* storage, const char* dir, bool verify) {
    File* f = storage_file_alloc(storage);
    if(storage_dir_open(f, dir)) {
        FileInfo info;
        char name[64];
        while(storage_dir_read(f, &info, name, sizeof(name))) {
            if(info.flags & FSF_DIRECTORY) continue;
            if(info.size == 0) continue;
            if(!ends_with_html_ci(name)) continue;
            if(app->evil_portal_template_count >= WIFI_APP_EVIL_PORTAL_MAX_TEMPLATES) break;

            WifiAppEvilPortalTemplateEntry* e =
                &app->evil_portal_templates[app->evil_portal_template_count];

            size_t nlen = strlen(name);
            size_t copy = nlen - 5; // strip .html
            if(copy >= sizeof(e->name)) copy = sizeof(e->name) - 1;
            memcpy(e->name, name, copy);
            e->name[copy] = 0;
            snprintf(e->path, sizeof(e->path), "%s/%s", dir, name);
            e->kind = WifiAppEvilPortalTemplateKindCustom;
            e->verify = verify;
            app->evil_portal_template_count++;
        }
        storage_dir_close(f);
    }
    storage_file_free(f);
}

// Build the dynamic template list:
//   built-ins (Google, Router) + every *.html under
//     /ext/wifi/evil_portal/login_template/  (verify off)
//     /ext/wifi/evil_portal/router_template/ (verify on)
static void scan_templates(WifiApp* app) {
    app->evil_portal_template_count = 0;

    WifiAppEvilPortalTemplateEntry* e =
        &app->evil_portal_templates[app->evil_portal_template_count++];
    strcpy(e->name, "Google");
    e->path[0] = 0;
    e->kind = WifiAppEvilPortalTemplateKindBuiltinGoogle;
    e->verify = false;

    e = &app->evil_portal_templates[app->evil_portal_template_count++];
    strcpy(e->name, "Router");
    e->path[0] = 0;
    e->kind = WifiAppEvilPortalTemplateKindBuiltinRouter;
    e->verify = true;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    scan_dir(app, storage, LOGIN_TEMPLATES_DIR, false);
    scan_dir(app, storage, ROUTER_TEMPLATES_DIR, true);
    furi_record_close(RECORD_STORAGE);

    if(app->evil_portal_template_index >= app->evil_portal_template_count) {
        app->evil_portal_template_index = 0;
    }
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
    app->evil_portal_template_index = idx;
    variable_item_set_current_value_text(item, app->evil_portal_templates[idx].name);
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

    scan_templates(app);

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
        app->variable_item_list, "Template",
        app->evil_portal_template_count, evil_portal_menu_set_template, app);
    {
        uint8_t idx = app->evil_portal_template_index;
        variable_item_set_current_value_index(item, idx);
        variable_item_set_current_value_text(item, app->evil_portal_templates[idx].name);
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
