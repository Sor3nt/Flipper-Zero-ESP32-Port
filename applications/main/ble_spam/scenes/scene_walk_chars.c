#include "../ble_spam_app.h"
#include "../ble_walk_hal.h"
#include "../ble_uuid_db.h"

#include <esp_log.h>
#include <stdio.h>

#define TAG "BleWalk"

static void format_char_label(esp_bt_uuid_t* uuid, char* buf, size_t buf_len) {
    const char* name = ble_uuid_db_lookup_char(uuid);
    if(name) {
        snprintf(buf, buf_len, "%s", name);
        return;
    }
    if(uuid->len == ESP_UUID_LEN_16) {
        snprintf(buf, buf_len, "0x%04X", uuid->uuid.uuid16);
    } else if(uuid->len == ESP_UUID_LEN_128) {
        uint8_t* u = uuid->uuid.uuid128;
        snprintf(buf, buf_len, "%02X%02X..%02X%02X", u[15], u[14], u[1], u[0]);
    } else {
        snprintf(buf, buf_len, "?");
    }
}

static void chars_callback(void* context, uint32_t index) {
    BleSpamApp* app = context;
    app->walk_selected_char = index;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void ble_spam_scene_walk_chars_on_enter(void* context) {
    BleSpamApp* app = context;

    uint16_t svc_count;
    BleWalkService* services = ble_walk_hal_get_services(&svc_count);
    if(app->walk_selected_service < svc_count) {
        ble_walk_hal_discover_chars(&services[app->walk_selected_service]);
    }

    uint16_t count;
    BleWalkChar* chars = ble_walk_hal_get_chars(&count);

    for(int i = 0; i < count; i++) {
        char label[64];
        char name_str[32];
        format_char_label(&chars[i].uuid, name_str, sizeof(name_str));

        char props[16] = "";
        if(chars[i].properties & 0x02) strcat(props, "R");
        if(chars[i].properties & 0x08) strcat(props, "W");
        if(chars[i].properties & 0x10) strcat(props, "N");

        snprintf(label, sizeof(label), "%s [%s]", name_str, props);
        submenu_add_item(app->submenu, label, i, chars_callback, app);
    }

    if(count == 0) {
        submenu_add_item(app->submenu, "(no characteristics)", 0xFF, chars_callback, app);
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, BleSpamViewSubmenu);
}

bool ble_spam_scene_walk_chars_on_event(void* context, SceneManagerEvent event) {
    BleSpamApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event != 0xFF) {
            scene_manager_next_scene(app->scene_manager, BleSpamSceneWalkCharDetail);
            consumed = true;
        }
    }

    return consumed;
}

void ble_spam_scene_walk_chars_on_exit(void* context) {
    BleSpamApp* app = context;
    submenu_reset(app->submenu);
}
