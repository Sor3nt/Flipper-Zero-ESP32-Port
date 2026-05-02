// CVE-2025-20700 detector: probes BLE devices for the unauthenticated Airoha
// RACE GATT service. Pure detector — no read/write, no memory access.
#include "../ble_spam_app.h"
#include "../ble_walk_hal.h"
#include "../views/race_detector_view.h"

#include <esp_log.h>
#include <string.h>

#define TAG "RaceDetect"

// Airoha RACE GATT service UUIDs (Insinuator full disclosure, 2025-12).
// ESP-IDF stores 128-bit UUIDs in little-endian byte order, so the byte
// arrays are the textual UUID reversed.

// 5052494D-2DAB-0341-6972-6F6861424C45 ("PRIM..-AB..irohaBLE")
static const uint8_t RACE_UUID_DEFAULT[16] = {
    0x45, 0x4C, 0x42, 0x41, 0x68, 0x6F, 0x72, 0x69,
    0x41, 0x03, 0xAB, 0x2D, 0x4D, 0x49, 0x52, 0x50,
};

// dc405470-a351-4a59-97d8-2e2e3b207fbb (Sony variant)
static const uint8_t RACE_UUID_SONY[16] = {
    0xBB, 0x7F, 0x20, 0x3B, 0x2E, 0x2E, 0xD8, 0x97,
    0x59, 0x4A, 0x51, 0xA3, 0x70, 0x54, 0x40, 0xDC,
};

static bool uuid_is_race(const esp_bt_uuid_t* u) {
    if(u->len != ESP_UUID_LEN_128) return false;
    if(memcmp(u->uuid.uuid128, RACE_UUID_DEFAULT, 16) == 0) return true;
    if(memcmp(u->uuid.uuid128, RACE_UUID_SONY, 16) == 0) return true;
    return false;
}

static void refresh_device_list(BleSpamApp* app) {
    uint16_t count;
    BleWalkDevice* devices = ble_walk_hal_get_devices(&count);
    if(count > RACE_DETECTOR_MAX_DEVICES) count = RACE_DETECTOR_MAX_DEVICES;

    RaceDetectorModel* model = view_get_model(app->view_race_detector);

    // Merge: keep status of already-known MACs, append new ones.
    for(uint16_t i = 0; i < count; i++) {
        BleWalkDevice* src = &devices[i];

        bool found = false;
        for(uint16_t j = 0; j < model->count; j++) {
            if(memcmp(model->devices[j].addr, src->addr, sizeof(esp_bd_addr_t)) == 0) {
                model->devices[j].rssi = src->rssi;
                if(src->name[0]) {
                    strncpy(model->devices[j].name, src->name, sizeof(model->devices[j].name) - 1);
                    model->devices[j].name[sizeof(model->devices[j].name) - 1] = '\0';
                }
                found = true;
                break;
            }
        }
        if(!found && model->count < RACE_DETECTOR_MAX_DEVICES) {
            RaceDevice* dst = &model->devices[model->count];
            memcpy(dst->addr, src->addr, sizeof(esp_bd_addr_t));
            dst->addr_type = src->addr_type;
            dst->rssi = src->rssi;
            strncpy(dst->name, src->name, sizeof(dst->name) - 1);
            dst->name[sizeof(dst->name) - 1] = '\0';
            dst->status = RaceStatusUnknown;
            model->count++;
        }
    }

    if(model->count > 0 && model->selected >= model->count) {
        model->selected = model->count - 1;
    }
    view_commit_model(app->view_race_detector, true);
}

static RaceStatus probe_device(BleSpamApp* app, RaceDevice* target) {
    BleWalkDevice walk_dev = {0};
    memcpy(walk_dev.addr, target->addr, sizeof(esp_bd_addr_t));
    walk_dev.addr_type = target->addr_type;

    app->race_probe_abort = false;
    if(!ble_walk_hal_connect(&walk_dev, &app->race_probe_abort)) {
        ESP_LOGW(TAG, "connect failed");
        return RaceStatusConnectFail;
    }

    if(!ble_walk_hal_discover_services()) {
        ble_walk_hal_disconnect();
        return RaceStatusConnectFail;
    }

    // Poll up to 3s, like scene_walk_services does.
    for(int i = 0; i < 60 && !ble_walk_hal_services_ready(); i++) {
        if(app->race_probe_abort) {
            ble_walk_hal_disconnect();
            return RaceStatusConnectFail;
        }
        furi_delay_ms(50);
    }

    RaceStatus result = RaceStatusClean;
    if(ble_walk_hal_services_ready()) {
        uint16_t svc_count;
        BleWalkService* services = ble_walk_hal_get_services(&svc_count);
        for(uint16_t i = 0; i < svc_count; i++) {
            if(uuid_is_race(&services[i].uuid)) {
                result = RaceStatusVulnerable;
                break;
            }
        }
    } else {
        result = RaceStatusConnectFail;
    }

    ble_walk_hal_disconnect();
    return result;
}

void ble_spam_scene_race_detector_on_enter(void* context) {
    BleSpamApp* app = context;

    RaceDetectorModel* model = view_get_model(app->view_race_detector);
    memset(model, 0, sizeof(RaceDetectorModel));
    view_commit_model(app->view_race_detector, true);

    view_dispatcher_switch_to_view(app->view_dispatcher, BleSpamViewRaceDetector);

    if(!ble_walk_hal_start()) {
        ESP_LOGE(TAG, "BT stack acquire failed");
        scene_manager_previous_scene(app->scene_manager);
        return;
    }

    ble_walk_hal_start_scan();
}

bool ble_spam_scene_race_detector_on_event(void* context, SceneManagerEvent event) {
    BleSpamApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        RaceDetectorModel* model = view_get_model(app->view_race_detector);

        if(event.event == InputKeyUp) {
            if(model->selected > 0) {
                model->selected--;
                if(model->selected < model->window_offset)
                    model->window_offset = model->selected;
            }
            view_commit_model(app->view_race_detector, true);
            consumed = true;
        } else if(event.event == InputKeyDown) {
            if(model->count > 0 && model->selected < model->count - 1) {
                model->selected++;
                if(model->selected >= model->window_offset + RACE_DETECTOR_ITEMS_ON_SCREEN)
                    model->window_offset =
                        model->selected - RACE_DETECTOR_ITEMS_ON_SCREEN + 1;
            }
            view_commit_model(app->view_race_detector, true);
            consumed = true;
        } else if(event.event == InputKeyOk) {
            if(model->count == 0 || model->selected >= model->count) {
                view_commit_model(app->view_race_detector, false);
                return true;
            }

            RaceDevice target_copy = model->devices[model->selected];
            uint16_t selected_idx = model->selected;

            model->devices[selected_idx].status = RaceStatusProbing;
            model->probing = true;
            view_commit_model(app->view_race_detector, true);

            ble_walk_hal_stop_scan();

            RaceStatus result = probe_device(app, &target_copy);

            // Re-acquire model after the blocking probe (ticks may have run).
            RaceDetectorModel* m2 = view_get_model(app->view_race_detector);
            // The selected_idx may still point to the same device; verify by addr.
            for(uint16_t i = 0; i < m2->count; i++) {
                if(memcmp(m2->devices[i].addr, target_copy.addr, sizeof(esp_bd_addr_t)) == 0) {
                    m2->devices[i].status = result;
                    break;
                }
            }
            m2->probing = false;
            view_commit_model(app->view_race_detector, true);

            ble_walk_hal_start_scan();
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        app->race_probe_abort = true;
        ble_walk_hal_disconnect();
        ble_walk_hal_stop_scan();
        ble_walk_hal_stop();
        consumed = false;
    } else if(event.type == SceneManagerEventTypeTick) {
        RaceDetectorModel* model = view_get_model(app->view_race_detector);
        if(!model->probing) {
            view_commit_model(app->view_race_detector, false);
            refresh_device_list(app);
        } else {
            view_commit_model(app->view_race_detector, false);
        }
    }

    return consumed;
}

void ble_spam_scene_race_detector_on_exit(void* context) {
    BleSpamApp* app = context;
    app->race_probe_abort = true;
    ble_walk_hal_disconnect();
    ble_walk_hal_stop_scan();
    ble_walk_hal_stop();
}
