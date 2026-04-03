#include "../ble_spam_app.h"
#include "../ble_walk_hal.h"
#include "../views/ble_walk_detail_view.h"

#include <esp_log.h>
#include <stdio.h>
#include <string.h>

#define TAG "BleWalk"

static void uuid_to_str(esp_bt_uuid_t* uuid, char* buf, size_t buf_len) {
    if(uuid->len == ESP_UUID_LEN_16) {
        snprintf(buf, buf_len, "0x%04X", uuid->uuid.uuid16);
    } else if(uuid->len == ESP_UUID_LEN_128) {
        uint8_t* u = uuid->uuid.uuid128;
        snprintf(buf, buf_len, "%02X%02X..%02X%02X", u[15], u[14], u[1], u[0]);
    } else {
        snprintf(buf, buf_len, "?");
    }
}

void ble_spam_scene_walk_char_detail_on_enter(void* context) {
    BleSpamApp* app = context;

    uint16_t count;
    BleWalkChar* chars = ble_walk_hal_get_chars(&count);
    BleWalkChar* chr = (app->walk_selected_char < count) ? &chars[app->walk_selected_char] : NULL;

    BleWalkDetailModel* model = view_get_model(app->view_walk_detail);
    memset(model, 0, sizeof(BleWalkDetailModel));

    if(chr) {
        uuid_to_str(&chr->uuid, model->uuid_str, sizeof(model->uuid_str));
        model->properties = chr->properties;

        // Auto-read if readable
        if(chr->properties & 0x02) {
            model->read_pending = true;
            ble_walk_hal_read_char(chr->handle);
        }
    }
    view_commit_model(app->view_walk_detail, true);

    view_dispatcher_switch_to_view(app->view_dispatcher, BleSpamViewWalkDetail);
}

bool ble_spam_scene_walk_char_detail_on_event(void* context, SceneManagerEvent event) {
    BleSpamApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        uint16_t char_count;
        BleWalkChar* chars = ble_walk_hal_get_chars(&char_count);
        BleWalkChar* chr = (app->walk_selected_char < char_count) ?
                           &chars[app->walk_selected_char] : NULL;

        if(event.event == InputKeyOk && chr && (chr->properties & 0x02)) {
            // Read
            BleWalkDetailModel* model = view_get_model(app->view_walk_detail);
            model->read_pending = true;
            view_commit_model(app->view_walk_detail, true);
            ble_walk_hal_read_char(chr->handle);
            consumed = true;
        } else if(event.event == InputKeyDown && chr && (chr->properties & 0x08)) {
            // Write: send 0x01 as simple test value
            uint8_t val = 0x01;
            ble_walk_hal_write_char(chr->handle, &val, 1);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        if(ble_walk_hal_read_ready()) {
            uint16_t len;
            uint8_t* data = ble_walk_hal_get_read_value(&len);

            BleWalkDetailModel* model = view_get_model(app->view_walk_detail);
            model->read_pending = false;
            model->value_len = len;
            if(len > sizeof(model->value)) len = sizeof(model->value);
            memcpy(model->value, data, len);
            view_commit_model(app->view_walk_detail, true);
        }
    }

    return consumed;
}

void ble_spam_scene_walk_char_detail_on_exit(void* context) {
    UNUSED(context);
}
