#include "../ble_spam_app.h"
#include "../ble_walk_hal.h"
#include "../ble_brute_log.h"
#include "../views/ble_brute_view.h"

#include <esp_gatt_defs.h>
#include <esp_log.h>
#include <furi.h>
#include <string.h>
#include <stdio.h>

#define TAG "BleBruteRun"

#define BRUTE_MIN_HANDLE        0x0001
#define BRUTE_MAX_HANDLE        0x00FF
#define BRUTE_READ_TIMEOUT_MS   800
#define BRUTE_PER_HANDLE_GAP_MS 10

static FuriThread* s_thread = NULL;
static volatile bool s_stop = false;
static BleBruteLog* s_log = NULL;

// Worker → UI shared status
static BleWalkDevice s_target;
static volatile uint16_t s_current_handle = 0;
static volatile uint16_t s_handles_done = 0;
static volatile uint16_t s_hit_count = 0;
static volatile uint16_t s_last_hit_handle = 0;
static volatile uint8_t s_last_hit_status = 0;
static volatile uint16_t s_last_hit_value_len = 0;
static volatile bool s_failed = false;
static volatile bool s_done = false;

static bool wait_read_ready(uint32_t timeout_ms) {
    uint32_t step = 25;
    uint32_t waited = 0;
    while(waited < timeout_ms) {
        if(s_stop) return false;
        if(ble_walk_hal_read_ready()) return true;
        furi_delay_ms(step);
        waited += step;
    }
    return ble_walk_hal_read_ready();
}

static int32_t brute_worker(void* ctx) {
    UNUSED(ctx);
    ESP_LOGI(TAG, "Brute worker started for %02X:%02X:%02X:%02X:%02X:%02X",
             s_target.addr[0], s_target.addr[1], s_target.addr[2],
             s_target.addr[3], s_target.addr[4], s_target.addr[5]);

    s_log = ble_brute_log_open();
    if(!s_log) {
        ESP_LOGE(TAG, "log open failed");
        s_failed = true;
        return -1;
    }

    ble_brute_log_session_marker(s_log, &s_target, "session_start");

    if(!ble_walk_hal_connect(&s_target)) {
        ESP_LOGW(TAG, "connect failed");
        ble_brute_log_session_marker(s_log, &s_target, "connect_failed");
        ble_brute_log_close(s_log);
        s_log = NULL;
        s_failed = true;
        return -1;
    }

    for(uint16_t h = BRUTE_MIN_HANDLE; h <= BRUTE_MAX_HANDLE; h++) {
        if(s_stop) break;
        if(!ble_walk_hal_is_connected()) break;

        s_current_handle = h;

        if(!ble_walk_hal_read_char(h)) {
            s_handles_done++;
            continue;
        }
        if(!wait_read_ready(BRUTE_READ_TIMEOUT_MS)) {
            s_handles_done++;
            continue;
        }

        uint8_t status = ble_walk_hal_get_read_status();
        if(status == ESP_GATT_INVALID_HANDLE) {
            s_handles_done++;
            furi_delay_ms(BRUTE_PER_HANDLE_GAP_MS);
            continue;
        }

        uint16_t value_len = 0;
        uint8_t* value = NULL;
        if(status == ESP_GATT_OK) {
            value = ble_walk_hal_get_read_value(&value_len);
        }

        ble_brute_log_hit(s_log, &s_target, h, status, value, value_len);

        s_hit_count++;
        s_last_hit_handle = h;
        s_last_hit_status = status;
        s_last_hit_value_len = value_len;
        s_handles_done++;

        furi_delay_ms(BRUTE_PER_HANDLE_GAP_MS);
    }

    if(ble_walk_hal_is_connected()) ble_walk_hal_disconnect();

    ble_brute_log_close(s_log);
    s_log = NULL;

    s_done = true;
    ESP_LOGI(TAG, "Brute worker done, hits=%u", s_hit_count);
    return 0;
}

void ble_spam_scene_brute_run_on_enter(void* context) {
    BleSpamApp* app = context;

    // Snapshot the chosen target from the HAL device list. The HAL list keeps
    // updating during the brute, so we copy now and use the local copy.
    uint16_t count;
    BleWalkDevice* devices = ble_walk_hal_get_devices(&count);
    if(app->brute_selected_device >= count) {
        // List shifted — fall back to first
        if(count == 0) {
            scene_manager_previous_scene(app->scene_manager);
            return;
        }
        app->brute_selected_device = 0;
    }
    memcpy(&s_target, &devices[app->brute_selected_device], sizeof(BleWalkDevice));

    // Init shared state
    s_stop = false;
    s_failed = false;
    s_done = false;
    s_current_handle = 0;
    s_handles_done = 0;
    s_hit_count = 0;
    s_last_hit_handle = 0;
    s_last_hit_status = 0;
    s_last_hit_value_len = 0;

    // Init view model
    BleBruteModel* model = view_get_model(app->view_brute_run);
    memset(model, 0, sizeof(*model));
    model->total_handles = BRUTE_MAX_HANDLE - BRUTE_MIN_HANDLE + 1;
    model->running = true;
    if(s_target.name[0]) {
        strncpy(model->target_name, s_target.name, sizeof(model->target_name) - 1);
    }
    snprintf(model->target_addr, sizeof(model->target_addr),
             "%02X:%02X:%02X:%02X:%02X:%02X",
             s_target.addr[0], s_target.addr[1], s_target.addr[2],
             s_target.addr[3], s_target.addr[4], s_target.addr[5]);
    view_commit_model(app->view_brute_run, true);

    view_dispatcher_switch_to_view(app->view_dispatcher, BleSpamViewBruteRun);

    s_thread = furi_thread_alloc();
    furi_thread_set_name(s_thread, "BleBruteRun");
    furi_thread_set_stack_size(s_thread, 8192);
    furi_thread_set_context(s_thread, app);
    furi_thread_set_callback(s_thread, brute_worker);
    furi_thread_start(s_thread);
}

bool ble_spam_scene_brute_run_on_event(void* context, SceneManagerEvent event) {
    BleSpamApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeTick) {
        BleBruteModel* model = view_get_model(app->view_brute_run);
        model->current_handle = s_current_handle;
        model->handles_done = s_handles_done;
        model->hit_count = s_hit_count;
        model->last_hit_handle = s_last_hit_handle;
        model->last_hit_status = s_last_hit_status;
        model->last_hit_value_len = s_last_hit_value_len;
        model->failed = s_failed;
        model->done = s_done;
        model->running = !s_done && !s_failed;
        view_commit_model(app->view_brute_run, true);
    }

    return consumed;
}

void ble_spam_scene_brute_run_on_exit(void* context) {
    UNUSED(context);

    s_stop = true;
    if(s_thread) {
        furi_thread_join(s_thread);
        furi_thread_free(s_thread);
        s_thread = NULL;
    }
    if(s_log) {
        ble_brute_log_close(s_log);
        s_log = NULL;
    }

    // Stop HAL — BruteScan started it, BruteRun is the last in this chain.
    if(ble_walk_hal_is_connected()) ble_walk_hal_disconnect();
    ble_walk_hal_stop();
}
