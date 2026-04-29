#include "../ble_spam_app.h"
#include "../ble_walk_hal.h"
#include "../ble_tracker_hal.h"
#include "../views/tracker_list_view.h"

#include <esp_log.h>
#include <string.h>

#define TAG "BleTrackerScan"

void ble_spam_scene_tracker_scan_on_enter(void* context) {
    BleSpamApp* app = context;

    TrackerListModel* model = view_get_model(app->view_tracker_scan);
    memset(model, 0, sizeof(TrackerListModel));
    view_commit_model(app->view_tracker_scan, true);

    view_dispatcher_switch_to_view(app->view_dispatcher, BleSpamViewTrackerScan);

    if(!ble_walk_hal_start()) {
        ESP_LOGE(TAG, "BT stack acquire failed");
        scene_manager_previous_scene(app->scene_manager);
        return;
    }

    ble_tracker_hal_start_scan();
}

bool ble_spam_scene_tracker_scan_on_event(void* context, SceneManagerEvent event) {
    BleSpamApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        TrackerListModel* model = view_get_model(app->view_tracker_scan);

        if(event.event == InputKeyUp) {
            if(model->selected > 0) {
                model->selected--;
                if(model->selected < model->window_offset)
                    model->window_offset = model->selected;
            }
            view_commit_model(app->view_tracker_scan, true);
            consumed = true;
        } else if(event.event == InputKeyDown) {
            if(model->count > 0 && model->selected < model->count - 1) {
                model->selected++;
                if(model->selected >= model->window_offset + TRACKER_LIST_ITEMS_ON_SCREEN)
                    model->window_offset =
                        model->selected - TRACKER_LIST_ITEMS_ON_SCREEN + 1;
            }
            view_commit_model(app->view_tracker_scan, true);
            consumed = true;
        } else if(event.event == InputKeyOk) {
            if(model->count > 0 && model->selected < model->count) {
                memcpy(
                    &app->tracker_target,
                    &model->devices[model->selected],
                    sizeof(TrackerDevice));
                view_commit_model(app->view_tracker_scan, false);
                scene_manager_next_scene(app->scene_manager, BleSpamSceneTrackerGeiger);
            } else {
                view_commit_model(app->view_tracker_scan, false);
            }
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        ble_tracker_hal_stop_scan();
        ble_walk_hal_stop();
        consumed = false;
    } else if(event.type == SceneManagerEventTypeTick) {
        uint16_t count;
        TrackerDevice* devices = ble_tracker_hal_get_devices(&count);

        TrackerListModel* model = view_get_model(app->view_tracker_scan);
        model->count = count;
        if(count > 0) {
            memcpy(model->devices, devices, count * sizeof(TrackerDevice));
        }
        if(model->selected >= count && count > 0) model->selected = count - 1;
        view_commit_model(app->view_tracker_scan, true);
    }

    return consumed;
}

void ble_spam_scene_tracker_scan_on_exit(void* context) {
    UNUSED(context);
    ble_tracker_hal_stop_scan();
}
