#include "../ble_spam_app.h"
#include "../ble_walk_hal.h"
#include "../ble_auto_walk_log.h"
#include "../views/ble_auto_walk_view.h"

#include <esp_log.h>
#include <furi.h>
#include <string.h>
#include <stdio.h>

#define TAG "BleAutoWalk"

#define AUTO_WALK_SCAN_PICK_INTERVAL_MS 1500
#define AUTO_WALK_SERVICE_DISCOVERY_TIMEOUT_MS 3000
#define AUTO_WALK_CHAR_READ_TIMEOUT_MS 1500

// Minimum signal strength to attempt a connect. Devices below this stay
// candidates — they get another shot once they come closer.
#define AUTO_WALK_MIN_RSSI -75

static FuriThread* s_thread = NULL;
static volatile bool s_stop = false;
static BleAutoWalkSeenSet s_seen;
static BleAutoWalkLog* s_log = NULL;

// Worker → UI status (simple shared struct, written by worker, read by UI tick)
static volatile AutoWalkStatus s_status = AutoWalkStatusIdle;
static char s_last_name[32];
static char s_last_addr[18];
static volatile uint16_t s_last_services = 0;
static volatile uint16_t s_last_chars = 0;

static void set_status(AutoWalkStatus s) {
    s_status = s;
}

static void update_last_device(const BleWalkDevice* dev) {
    if(dev->name[0]) {
        strncpy(s_last_name, dev->name, sizeof(s_last_name) - 1);
        s_last_name[sizeof(s_last_name) - 1] = '\0';
    } else {
        s_last_name[0] = '\0';
    }
    snprintf(s_last_addr, sizeof(s_last_addr),
             "%02X:%02X:%02X:%02X:%02X:%02X",
             dev->addr[0], dev->addr[1], dev->addr[2],
             dev->addr[3], dev->addr[4], dev->addr[5]);
    s_last_services = 0;
    s_last_chars = 0;
}

static bool wait_for(bool (*pred)(void), uint32_t timeout_ms) {
    uint32_t step = 50;
    uint32_t waited = 0;
    while(waited < timeout_ms) {
        if(s_stop) return false;
        if(pred()) return true;
        furi_delay_ms(step);
        waited += step;
    }
    return pred();
}

// Walk through current device's services + chars and log every readable value.
static void crawl_current_device(BleWalkDevice* dev) {
    set_status(AutoWalkStatusDiscover);

    if(!ble_walk_hal_discover_services()) {
        ble_auto_walk_log_device_marker(s_log, dev, "discover_failed");
        return;
    }
    if(!wait_for(ble_walk_hal_services_ready, AUTO_WALK_SERVICE_DISCOVERY_TIMEOUT_MS)) {
        ble_auto_walk_log_device_marker(s_log, dev, "discover_timeout");
        return;
    }

    uint16_t svc_count;
    BleWalkService* services = ble_walk_hal_get_services(&svc_count);
    s_last_services = svc_count;

    if(svc_count == 0) {
        ble_auto_walk_log_device_marker(s_log, dev, "no_services");
        return;
    }

    // Copy services so we can iterate even if HAL state changes.
    BleWalkService local_services[BLE_WALK_MAX_SERVICES];
    memcpy(local_services, services, svc_count * sizeof(BleWalkService));

    bool any_char_logged = false;
    uint16_t total_chars = 0;

    for(uint16_t si = 0; si < svc_count; si++) {
        if(s_stop || !ble_walk_hal_is_connected()) break;

        if(!ble_walk_hal_discover_chars(&local_services[si])) continue;
        if(!wait_for(ble_walk_hal_chars_ready, AUTO_WALK_SERVICE_DISCOVERY_TIMEOUT_MS)) continue;

        uint16_t char_count;
        BleWalkChar* chars = ble_walk_hal_get_chars(&char_count);
        if(char_count == 0) continue;

        BleWalkChar local_chars[BLE_WALK_MAX_CHARS];
        memcpy(local_chars, chars, char_count * sizeof(BleWalkChar));

        total_chars += char_count;
        s_last_chars = total_chars;

        for(uint16_t ci = 0; ci < char_count; ci++) {
            if(s_stop || !ble_walk_hal_is_connected()) break;
            BleWalkChar* chr = &local_chars[ci];

            // Only chars with READ property (0x02)
            if(!(chr->properties & 0x02)) {
                ble_auto_walk_log_char_value(s_log, dev, &local_services[si], chr, NULL, 0);
                continue;
            }

            set_status(AutoWalkStatusRead);
            if(!ble_walk_hal_read_char(chr->handle)) {
                ble_auto_walk_log_char_value(s_log, dev, &local_services[si], chr, NULL, 0);
                continue;
            }
            if(!wait_for(ble_walk_hal_read_ready, AUTO_WALK_CHAR_READ_TIMEOUT_MS)) {
                ble_auto_walk_log_char_value(s_log, dev, &local_services[si], chr, NULL, 0);
                continue;
            }

            uint16_t value_len;
            uint8_t* value = ble_walk_hal_get_read_value(&value_len);
            ble_auto_walk_log_char_value(s_log, dev, &local_services[si], chr, value, value_len);
            any_char_logged = true;
        }
    }

    if(!any_char_logged && total_chars == 0) {
        ble_auto_walk_log_device_marker(s_log, dev, "no_chars");
    }
}

// Pick the strongest unseen device above the RSSI threshold. Returns false if
// none qualify. Devices below the threshold are deliberately not added to the
// seen-set so they get another attempt when the signal improves.
static bool pick_unseen_device(BleWalkDevice* out) {
    uint16_t count;
    BleWalkDevice* devices = ble_walk_hal_get_devices(&count);

    int best = -1;
    int8_t best_rssi = AUTO_WALK_MIN_RSSI - 1;
    for(uint16_t i = 0; i < count; i++) {
        if(devices[i].rssi < AUTO_WALK_MIN_RSSI) continue;
        if(ble_auto_walk_seen_contains(&s_seen, devices[i].addr)) continue;
        if(devices[i].rssi > best_rssi) {
            best = i;
            best_rssi = devices[i].rssi;
        }
    }

    if(best < 0) return false;
    memcpy(out, &devices[best], sizeof(BleWalkDevice));
    return true;
}

static int32_t auto_walk_worker(void* ctx) {
    UNUSED(ctx);
    ESP_LOGI(TAG, "Auto-Walk worker started");

    if(!ble_walk_hal_start()) {
        ESP_LOGE(TAG, "HAL start failed");
        return -1;
    }

    s_log = ble_auto_walk_log_open(&s_seen);
    if(!s_log) {
        ESP_LOGE(TAG, "Log open failed");
        ble_walk_hal_stop();
        return -1;
    }

    while(!s_stop) {
        // Scan phase
        set_status(AutoWalkStatusScan);
        ble_walk_hal_start_scan();

        BleWalkDevice picked;
        bool have_pick = false;

        // Let scanner gather adverts for ~1.5s, then re-check on each tick
        uint32_t waited = 0;
        while(waited < AUTO_WALK_SCAN_PICK_INTERVAL_MS && !s_stop) {
            if(pick_unseen_device(&picked)) {
                have_pick = true;
                break;
            }
            furi_delay_ms(100);
            waited += 100;
        }

        if(s_stop) break;

        if(!have_pick) {
            // No new devices yet — keep scanning, brief idle to avoid hot loop
            furi_delay_ms(200);
            continue;
        }

        // Mark as seen BEFORE attempting connect, so failures don't requeue
        ble_auto_walk_seen_add(&s_seen, picked.addr);
        update_last_device(&picked);

        // Connect
        set_status(AutoWalkStatusConnect);
        if(!ble_walk_hal_connect(&picked)) {
            ble_auto_walk_log_device_marker(s_log, &picked, "connect_failed");
            continue;
        }

        crawl_current_device(&picked);

        // Disconnect — always
        ble_walk_hal_disconnect();
    }

    ble_walk_hal_stop_scan();
    if(ble_walk_hal_is_connected()) ble_walk_hal_disconnect();

    ble_auto_walk_log_close(s_log);
    s_log = NULL;

    ble_walk_hal_stop();

    set_status(AutoWalkStatusDone);
    ESP_LOGI(TAG, "Auto-Walk worker stopped");
    return 0;
}

// ---------------------------------------------------------------------------
// Scene callbacks
// ---------------------------------------------------------------------------

void ble_spam_scene_auto_walk_on_enter(void* context) {
    BleSpamApp* app = context;

    BleAutoWalkModel* model = view_get_model(app->view_auto_walk);
    memset(model, 0, sizeof(*model));
    model->status = AutoWalkStatusIdle;
    view_commit_model(app->view_auto_walk, true);

    view_dispatcher_switch_to_view(app->view_dispatcher, BleSpamViewAutoWalk);

    s_stop = false;
    ble_auto_walk_seen_reset(&s_seen);
    s_last_name[0] = '\0';
    s_last_addr[0] = '\0';
    s_last_services = 0;
    s_last_chars = 0;
    s_status = AutoWalkStatusIdle;

    s_thread = furi_thread_alloc();
    furi_thread_set_name(s_thread, "BleAutoWalk");
    furi_thread_set_stack_size(s_thread, 8192);
    furi_thread_set_context(s_thread, app);
    furi_thread_set_callback(s_thread, auto_walk_worker);
    furi_thread_start(s_thread);
}

bool ble_spam_scene_auto_walk_on_event(void* context, SceneManagerEvent event) {
    BleSpamApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeTick) {
        BleAutoWalkModel* model = view_get_model(app->view_auto_walk);
        model->seen_count = s_seen.count;
        model->seen_full = (s_seen.count >= BLE_AUTO_WALK_SEEN_MAX);
        model->status = s_status;
        model->last_services = s_last_services;
        model->last_chars = s_last_chars;
        strncpy(model->last_name, s_last_name, sizeof(model->last_name) - 1);
        model->last_name[sizeof(model->last_name) - 1] = '\0';
        strncpy(model->last_addr, s_last_addr, sizeof(model->last_addr) - 1);
        model->last_addr[sizeof(model->last_addr) - 1] = '\0';
        view_commit_model(app->view_auto_walk, true);
    }

    return consumed;
}

void ble_spam_scene_auto_walk_on_exit(void* context) {
    UNUSED(context);

    s_stop = true;
    if(s_thread) {
        furi_thread_join(s_thread);
        furi_thread_free(s_thread);
        s_thread = NULL;
    }
    // Worker closes the log + stops HAL itself; nothing else to do here.
}
