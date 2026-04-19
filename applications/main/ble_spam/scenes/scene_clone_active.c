#include "../ble_spam_app.h"
#include "../ble_spam_hal.h"
#include "../ble_walk_hal.h"
#include "../views/ble_spam_view.h"

#include <esp_log.h>
#include <string.h>

#define TAG "BleClone"

static FuriThread* s_clone_thread = NULL;

// Store clone target info (copied from walk device list before HAL switch)
static uint8_t s_clone_addr[6];
static uint8_t s_clone_adv_data[31];
static uint8_t s_clone_adv_len;
static char s_clone_name[48];

static int32_t clone_thread_fn(void* arg) {
    BleSpamApp* app = arg;
    ESP_LOGI(TAG, "Clone thread started: %s", s_clone_name);

    // Set the cloned device's MAC address
    ble_spam_hal_set_addr(s_clone_addr);

    while(app->clone_active) {
        // Advertise with exact same data as the original device
        ble_spam_hal_set_adv_data(s_clone_adv_data, s_clone_adv_len);
        app->packet_count++;
        furi_delay_ms(100);
    }

    ble_spam_hal_stop_adv();
    ESP_LOGI(TAG, "Clone thread stopped, %lu packets", (unsigned long)app->packet_count);
    return 0;
}

void ble_spam_scene_clone_active_on_enter(void* context) {
    BleSpamApp* app = context;

    // Get the scanned device info (walk HAL data is still in memory)
    uint16_t count;
    BleWalkDevice* devices = ble_walk_hal_get_devices(&count);
    BleWalkDevice* target = NULL;
    if(app->clone_selected_device < count) {
        target = &devices[app->clone_selected_device];
    }

    if(!target || target->adv_data_len == 0) {
        ESP_LOGE(TAG, "No valid clone target");
        scene_manager_previous_scene(app->scene_manager);
        return;
    }

    // Copy target data before switching HAL
    memcpy(s_clone_addr, target->addr, 6);
    memcpy(s_clone_adv_data, target->adv_data, target->adv_data_len);
    s_clone_adv_len = target->adv_data_len;
    if(target->name[0]) {
        snprintf(s_clone_name, sizeof(s_clone_name), "%s", target->name);
    } else {
        snprintf(s_clone_name, sizeof(s_clone_name), "%02X:%02X:%02X:%02X:%02X:%02X",
                 target->addr[0], target->addr[1], target->addr[2],
                 target->addr[3], target->addr[4], target->addr[5]);
    }

    ESP_LOGI(TAG, "Cloning: %s (adv_len=%d)", s_clone_name, s_clone_adv_len);

    // Start spam HAL for advertising
    if(!ble_spam_hal_start()) {
        ESP_LOGE(TAG, "Spam HAL init failed");
        scene_manager_previous_scene(app->scene_manager);
        return;
    }

    // Init view
    BleSpamRunningModel* model = view_get_model(app->view_running);
    strncpy(model->attack_name, "BLE Clone", sizeof(model->attack_name) - 1);
    strncpy(model->device_name, s_clone_name, sizeof(model->device_name) - 1);
    model->packet_count = 0;
    model->delay_ms = 100;
    model->running = true;
    view_commit_model(app->view_running, true);

    view_dispatcher_switch_to_view(app->view_dispatcher, BleSpamViewRunning);

    // Start clone thread
    app->clone_active = true;
    app->packet_count = 0;

    s_clone_thread = furi_thread_alloc();
    furi_thread_set_name(s_clone_thread, "BleClone");
    furi_thread_set_stack_size(s_clone_thread, 4096);
    furi_thread_set_context(s_clone_thread, app);
    furi_thread_set_callback(s_clone_thread, clone_thread_fn);
    furi_thread_start(s_clone_thread);
}

bool ble_spam_scene_clone_active_on_event(void* context, SceneManagerEvent event) {
    BleSpamApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeTick) {
        BleSpamRunningModel* model = view_get_model(app->view_running);
        model->packet_count = app->packet_count;
        model->running = app->clone_active;
        view_commit_model(app->view_running, true);
    }

    return consumed;
}

void ble_spam_scene_clone_active_on_exit(void* context) {
    BleSpamApp* app = context;

    app->clone_active = false;
    if(s_clone_thread) {
        furi_thread_join(s_clone_thread);
        furi_thread_free(s_clone_thread);
        s_clone_thread = NULL;
    }

    ble_spam_hal_stop();
}
