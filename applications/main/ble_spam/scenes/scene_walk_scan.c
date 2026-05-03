#include "../ble_spam_app.h"
#include "../ble_walk_hal.h"
#include "../views/ble_walk_scan_view.h"

#include <esp_log.h>
#include <string.h>

#define TAG "BleWalk"

void ble_spam_scene_walk_scan_on_enter(void* context) {
    BleSpamApp* app = context;

    BleWalkScanModel* model = view_get_model(app->view_walk_scan);
    memset(model, 0, sizeof(BleWalkScanModel));
    model->scanning = true;
    model->connect_status = WalkScanStatusNone;
    view_commit_model(app->view_walk_scan, true);

    view_dispatcher_switch_to_view(app->view_dispatcher, BleSpamViewWalkScan);

    if(!ble_walk_hal_start()) {
        ESP_LOGE(TAG, "BLE Walk HAL init failed");
        scene_manager_previous_scene(app->scene_manager);
        return;
    }

    // Disconnect if still connected from previous session
    if(ble_walk_hal_is_connected()) {
        ble_walk_hal_disconnect();
    }

    // Always (re)start scanning
    ble_walk_hal_start_scan();
}

bool ble_spam_scene_walk_scan_on_event(void* context, SceneManagerEvent event) {
    BleSpamApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == InputKeyUp) {
            BleWalkScanModel* model = view_get_model(app->view_walk_scan);
            if(model->selected > 0) {
                model->selected--;
                if(model->selected < model->window_offset)
                    model->window_offset = model->selected;
            }
            view_commit_model(app->view_walk_scan, true);
            consumed = true;
        } else if(event.event == InputKeyDown) {
            BleWalkScanModel* model = view_get_model(app->view_walk_scan);
            if(model->count > 0 && model->selected < model->count - 1) {
                model->selected++;
                if(model->selected >= model->window_offset + WALK_SCAN_ITEMS_ON_SCREEN)
                    model->window_offset = model->selected - WALK_SCAN_ITEMS_ON_SCREEN + 1;
            }
            view_commit_model(app->view_walk_scan, true);
            consumed = true;
        } else if(event.event == InputKeyOk) {
            BleWalkScanModel* model = view_get_model(app->view_walk_scan);
            uint16_t selected = model->selected;
            uint16_t dev_count = model->count;
            if(dev_count > 0) {
                model->connect_status = WalkScanStatusConnecting;
            }
            view_commit_model(app->view_walk_scan, true);

            if(dev_count > 0) {
                app->walk_selected_device = selected;
                uint16_t count;
                BleWalkDevice* devices = ble_walk_hal_get_devices(&count);
                if(selected < count) {
                    if(ble_walk_hal_connect(&devices[selected], NULL)) {
                        // Show "Connected!" briefly
                        BleWalkScanModel* m = view_get_model(app->view_walk_scan);
                        m->connect_status = WalkScanStatusConnected;
                        view_commit_model(app->view_walk_scan, true);
                        furi_delay_ms(500);
                        scene_manager_next_scene(app->scene_manager, BleSpamSceneWalkServices);
                    } else {
                        BleWalkScanModel* m = view_get_model(app->view_walk_scan);
                        m->connect_status = WalkScanStatusFailed;
                        view_commit_model(app->view_walk_scan, true);
                        // Restart scanning after failed connect
                        ble_walk_hal_start_scan();
                    }
                }
            }
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        ble_walk_hal_stop_scan();
        ble_walk_hal_disconnect();
        ble_walk_hal_stop();
        consumed = false;
    } else if(event.type == SceneManagerEventTypeTick) {
        uint16_t count;
        BleWalkDevice* devices = ble_walk_hal_get_devices(&count);

        BleWalkScanModel* model = view_get_model(app->view_walk_scan);
        model->count = count;
        model->scanning = ble_walk_hal_is_scanning();
        if(count > 0) {
            memcpy(model->devices, devices, count * sizeof(BleWalkDevice));
        }
        // Clear failed status after a few ticks
        if(model->connect_status == WalkScanStatusFailed) {
            model->connect_status = WalkScanStatusNone;
        }
        view_commit_model(app->view_walk_scan, true);
    }

    return consumed;
}

void ble_spam_scene_walk_scan_on_exit(void* context) {
    UNUSED(context);
    ble_walk_hal_stop_scan();
}
