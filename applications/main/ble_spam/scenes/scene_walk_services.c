#include "../ble_spam_app.h"
#include "../ble_walk_hal.h"
#include "../ble_uuid_db.h"

#include <esp_log.h>
#include <stdio.h>

#define TAG "BleWalk"

static void format_service_label(esp_bt_uuid_t* uuid, char* buf, size_t buf_len) {
    const char* name = ble_uuid_db_lookup_service(uuid);
    if(name) {
        snprintf(buf, buf_len, "%s", name);
        return;
    }
    if(uuid->len == ESP_UUID_LEN_16) {
        snprintf(buf, buf_len, "0x%04X", uuid->uuid.uuid16);
    } else if(uuid->len == ESP_UUID_LEN_32) {
        snprintf(buf, buf_len, "0x%08lX", (unsigned long)uuid->uuid.uuid32);
    } else if(uuid->len == ESP_UUID_LEN_128) {
        uint8_t* u = uuid->uuid.uuid128;
        snprintf(
            buf,
            buf_len,
            "%02X%02X%02X%02X-%02X%02X",
            u[15],
            u[14],
            u[13],
            u[12],
            u[11],
            u[10]);
    } else {
        snprintf(buf, buf_len, "?");
    }
}

static void services_callback(void* context, uint32_t index) {
    BleSpamApp* app = context;
    app->walk_selected_service = index;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void ble_spam_scene_walk_services_on_enter(void* context) {
    BleSpamApp* app = context;

    ble_walk_hal_discover_services();

    // Wait for service discovery (max 3s)
    for(int i = 0; i < 60 && !ble_walk_hal_services_ready(); i++) {
        furi_delay_ms(50);
    }

    uint16_t count;
    BleWalkService* services = ble_walk_hal_get_services(&count);

    for(int i = 0; i < count; i++) {
        char label[40];
        format_service_label(&services[i].uuid, label, sizeof(label));
        submenu_add_item(app->submenu, label, i, services_callback, app);
    }

    if(count == 0) {
        submenu_add_item(app->submenu, "(no services)", 0xFF, services_callback, app);
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, BleSpamViewSubmenu);
}

bool ble_spam_scene_walk_services_on_event(void* context, SceneManagerEvent event) {
    BleSpamApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event != 0xFF) {
            scene_manager_next_scene(app->scene_manager, BleSpamSceneWalkChars);
            consumed = true;
        }
    }

    return consumed;
}

void ble_spam_scene_walk_services_on_exit(void* context) {
    BleSpamApp* app = context;
    submenu_reset(app->submenu);

    // If going back (not forward to chars), disconnect and stop HAL
    if(!ble_walk_hal_is_connected()) return;
    // Check if next scene is WalkChars — if not, disconnect
    // SceneManager handles this: if back event, we disconnect
}

