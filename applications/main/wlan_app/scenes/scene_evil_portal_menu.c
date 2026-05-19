#include "../wlan_app.h"
#include <storage/storage.h>

#define EP_BRIDGE_CFG_PATH "/ext/apps_data/wifi/evil_portal_bridge.cfg"

// Load bridge settings from SD. Silently does nothing if file missing.
static void ep_bridge_settings_load(WlanApp* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* f = storage_file_alloc(storage);
    if(storage_file_open(f, EP_BRIDGE_CFG_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        uint64_t sz = storage_file_size(f);
        if(sz > 0 && sz < 4096) {
            char* buf = malloc((size_t)sz + 1);
            if(buf) {
                uint16_t got = storage_file_read(f, buf, (uint16_t)sz);
                buf[got] = '\0';
                const char* p = buf;
                while(*p) {
                    const char* nl = strchr(p, '\n');
                    size_t line_len = nl ? (size_t)(nl - p) : strlen(p);
                    if(line_len > 0 && p[0] != '#') {
                        const char* eq = memchr(p, '=', line_len);
                        if(eq) {
                            size_t klen = eq - p;
                            const char* val = eq + 1;
                            size_t vlen = line_len - klen - 1;
                            while(vlen && (val[vlen-1] == '\r' || val[vlen-1] == ' ')) vlen--;
                            if(klen == 13 && memcmp(p, "bridge_enable", 13) == 0) {
                                app->evil_portal_bridge_enable = (vlen > 0 && val[0] == '1');
                            } else if(klen == 11 && memcmp(p, "bridge_ssid", 11) == 0) {
                                size_t c = vlen < sizeof(app->evil_portal_bridge_ssid) - 1 ?
                                           vlen : sizeof(app->evil_portal_bridge_ssid) - 1;
                                memcpy(app->evil_portal_bridge_ssid, val, c);
                                app->evil_portal_bridge_ssid[c] = '\0';
                            } else if(klen == 15 && memcmp(p, "bridge_password", 15) == 0) {
                                size_t c = vlen < sizeof(app->evil_portal_bridge_password) - 1 ?
                                           vlen : sizeof(app->evil_portal_bridge_password) - 1;
                                memcpy(app->evil_portal_bridge_password, val, c);
                                app->evil_portal_bridge_password[c] = '\0';
                            }
                        }
                    }
                    p = nl ? nl + 1 : p + line_len;
                }
                free(buf);
            }
        }
        storage_file_close(f);
    }
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
}

static void ep_bridge_settings_save(const WlanApp* app) {
    char buf[256];
    int len = snprintf(buf, sizeof(buf),
                       "bridge_enable=%d\nbridge_ssid=%s\nbridge_password=%s\n",
                       app->evil_portal_bridge_enable ? 1 : 0,
                       app->evil_portal_bridge_ssid,
                       app->evil_portal_bridge_password);
    if(len <= 0 || len >= (int)sizeof(buf)) return;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, "/ext/apps_data");
    storage_simply_mkdir(storage, "/ext/apps_data/wifi");
    File* f = storage_file_alloc(storage);
    if(storage_file_open(f, EP_BRIDGE_CFG_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_write(f, buf, (uint16_t)len);
        storage_file_close(f);
    }
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
}

enum EvilPortalMenuIndex {
    EpMenuIdxSsid,
    EpMenuIdxChannel,
    EpMenuIdxTemplate,
    EpMenuIdxKarma,
    EpMenuIdxBridge,        // Off/On toggle
    EpMenuIdxBridgeSsid,    // text input scene
    EpMenuIdxBridgePwd,     // text input scene
    EpMenuIdxStart,
};

static const char* const karma_text[2] = {"Off", "On"};

#define EP_BRIDGE_COUNT 2
static const char* const bridge_text[EP_BRIDGE_COUNT] = { "Off", "On" };

#define EP_CHANNEL_COUNT 12
static const char* const channel_text[EP_CHANNEL_COUNT] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12",
};

// Index 0/1 = Builtin, ab 2 = SD-Templates (app->evil_portal_templates).
#define EP_TEMPLATE_BUILTIN 2
#define EP_TEMPLATE_SD_BASE EP_TEMPLATE_BUILTIN
static const char* const builtin_template_names[EP_TEMPLATE_BUILTIN] = {
    "Google",
    "Router",
};

// Gesamtzahl der wählbaren Templates (Builtin + SD).
static uint8_t ep_template_total(WlanApp* app) {
    return (uint8_t)(EP_TEMPLATE_BUILTIN + app->evil_portal_templates.count);
}

// Stabiler Label-Pointer für einen Template-Index (Builtin oder SD-Basename).
static const char* ep_template_label(WlanApp* app, uint8_t idx) {
    if(idx < EP_TEMPLATE_BUILTIN) return builtin_template_names[idx];
    uint8_t s = (uint8_t)(idx - EP_TEMPLATE_SD_BASE);
    if(s < app->evil_portal_templates.count) {
        return app->evil_portal_templates.items[s].name;
    }
    return builtin_template_names[0];
}

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
    uint8_t total = ep_template_total(app);
    if(idx >= total) idx = (uint8_t)(total - 1);
    app->evil_portal_template_index = idx;
    variable_item_set_current_value_text(item, ep_template_label(app, idx));
}

static void ep_menu_set_karma(VariableItem* item) {
    WlanApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item) ? 1 : 0;
    app->evil_portal_karma = (idx == 1);
    variable_item_set_current_value_text(item, karma_text[idx]);
}

static void ep_menu_set_bridge(VariableItem* item) {
    WlanApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    app->evil_portal_bridge_enable = (idx == 1);
    variable_item_set_current_value_text(item, bridge_text[idx]);
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
    wlan_evil_portal_templates_scan(&app->evil_portal_templates);
    if(app->evil_portal_template_index >= ep_template_total(app)) {
        app->evil_portal_template_index = 0;
    }

    // Load persisted bridge settings (no-op if file missing). Only loads on
    // first entry to the menu in this app session; bridge_enable can be
    // toggled in the menu and saved on exit.
    static bool bridge_loaded = false;
    if(!bridge_loaded) {
        ep_bridge_settings_load(app);
        bridge_loaded = true;
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
        app->variable_item_list, "Template", ep_template_total(app),
        ep_menu_set_template, app);
    {
        uint8_t idx = app->evil_portal_template_index;
        variable_item_set_current_value_index(item, idx);
        variable_item_set_current_value_text(item, ep_template_label(app, idx));
    }

    item = variable_item_list_add(
        app->variable_item_list, "Karma", 2, ep_menu_set_karma, app);
    {
        uint8_t idx = app->evil_portal_karma ? 1 : 0;
        variable_item_set_current_value_index(item, idx);
        variable_item_set_current_value_text(item, karma_text[idx]);
    }

    item = variable_item_list_add(
        app->variable_item_list, "Bridge", EP_BRIDGE_COUNT, ep_menu_set_bridge, app);
    {
        uint8_t idx = app->evil_portal_bridge_enable ? 1 : 0;
        variable_item_set_current_value_index(item, idx);
        variable_item_set_current_value_text(item, bridge_text[idx]);
    }

    item = variable_item_list_add(app->variable_item_list, "Bridge SSID", 1, NULL, app);
    variable_item_set_current_value_text(
        item, app->evil_portal_bridge_ssid[0] ? app->evil_portal_bridge_ssid : "(none)");

    item = variable_item_list_add(app->variable_item_list, "Bridge Pwd", 1, NULL, app);
    variable_item_set_current_value_text(
        item, app->evil_portal_bridge_password[0] ? "(set)" : "(none)");

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
        case EpMenuIdxBridgeSsid:
            scene_manager_set_scene_state(
                app->scene_manager, WlanAppSceneEvilPortalMenu, EpMenuIdxBridgeSsid);
            scene_manager_next_scene(app->scene_manager, WlanAppSceneEvilPortalBridgeSsid);
            consumed = true;
            break;
        case EpMenuIdxBridgePwd:
            scene_manager_set_scene_state(
                app->scene_manager, WlanAppSceneEvilPortalMenu, EpMenuIdxBridgePwd);
            scene_manager_next_scene(app->scene_manager, WlanAppSceneEvilPortalBridgePwd);
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
    ep_bridge_settings_save(app);
    variable_item_list_reset(app->variable_item_list);
}
