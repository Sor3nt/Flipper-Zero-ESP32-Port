#include "../wlan_app.h"
#include <wlan_hal.h>
#include <wlan_passwords.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>
#include <esp_heap_caps.h>
#include <stdlib.h>
#include <string.h>

#define CONNECT_MENU_HANDSHAKE 1
#define CONNECT_MENU_DEAUTH    2
#define CONNECT_MENU_SNIFFER   3
#define CONNECT_MENU_PORTAL    4

// ============================================================================
// STATE SCAN (di-share antara GUI thread dan worker thread)
// ============================================================================
typedef struct {
    uint32_t refs;
    bool cancelled;
    bool complete;
    bool success;
    uint16_t count;
    WlanApRecord records[WLAN_APP_MAX_APS];
} WlanConnectScan;

static WlanConnectScan* s_connect_scan;

// ============================================================================
// REFERENCE COUNTING
// ============================================================================
static void connect_scan_release(WlanConnectScan* scan) {
    if(!scan) return;
    // __atomic_sub_fetch mengembalikan nilai SETELAH subtraction
    if(__atomic_sub_fetch(&scan->refs, 1, __ATOMIC_ACQ_REL) == 0) {
        free(scan);
    }
}

// ============================================================================
// WORKER TASK: SCAN AP
// ============================================================================
static void connect_scan_task(void* context) {
    WlanConnectScan* scan = context;
    wifi_ap_record_t* raw = NULL;
    uint16_t found = 0;

    if(__atomic_load_n(&scan->cancelled, __ATOMIC_ACQUIRE)) goto complete;
    if(!wlan_hal_is_started() && !wlan_hal_start()) {
        FURI_LOG_E(WLAN_APP_TAG, "Unable to start WiFi for AP scan");
        goto complete;
    }
    if(__atomic_load_n(&scan->cancelled, __ATOMIC_ACQUIRE)) goto complete;

    wlan_hal_scan_cancellable(&raw, &found, WLAN_APP_MAX_APS, &scan->cancelled);

    if(!__atomic_load_n(&scan->cancelled, __ATOMIC_ACQUIRE)) {
        // FIX #3: bounds check defensive — jaga-jaga kalau HAL return > MAX
        for(uint16_t i = 0; i < found && scan->count < WLAN_APP_MAX_APS; ++i) {
            WlanApRecord* record = &scan->records[scan->count++];

            // FIX #4: null-terminate eksplisit (calloc sudah 0, tapi eksplisit lebih aman)
            strncpy(record->ssid, (const char*)raw[i].ssid, sizeof(record->ssid) - 1);
            record->ssid[sizeof(record->ssid) - 1] = '\0';

            memcpy(record->bssid, raw[i].bssid, sizeof(record->bssid));
            record->rssi = raw[i].rssi;
            record->channel = raw[i].primary;
            record->authmode = raw[i].authmode;
            record->is_open = (raw[i].authmode == WIFI_AUTH_OPEN);
        }
        scan->success = true;
    }

complete:
    if(raw) free(raw);
    __atomic_store_n(&scan->complete, true, __ATOMIC_RELEASE);
    connect_scan_release(scan);
    // Stack liegt im PSRAM (xTaskCreateWithCaps) → Self-Delete MUSS über
    // vTaskDeleteWithCaps laufen, sonst werden Stack + TCB nicht freigegeben.
    vTaskDeleteWithCaps(NULL);
}

// ============================================================================
// START SCAN WORKER
// ============================================================================
static bool connect_scan_start(void) {
    WlanConnectScan* scan = calloc(1, sizeof(*scan));
    if(!scan) {
        FURI_LOG_E(WLAN_APP_TAG, "Unable to allocate AP scan state");
        return false;
    }
    __atomic_store_n(&scan->refs, 2, __ATOMIC_RELAXED);  // 1 caller, 1 worker

    // Worker-Stack ins PSRAM legen (CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y).
    // Beim Öffnen der App ist der (geparkte) BT-Stack oft noch resident, der
    // interne Heap dann zu knapp für einen 8-KB-Task-Stack → pthread_create/
    // xTaskCreate scheiterten mit ENOMEM (errno 12), der Scan lief nie an.
    // xTaskCreateWithCaps legt den Stack ins PSRAM (4+ MB frei) und nur den
    // kleinen TCB intern; das gelingt auch bei residentem BT. Der Worker ruft
    // esp_wifi_* nie direkt auf (das läuft im WlanHal-Worker mit internem
    // Stack), daher ist ein PSRAM-Stack hier sicher. Per-Run-Alloc + Self-Delete
    // (vTaskDeleteWithCaps) behält die Lebensdauer-Semantik des alten pthreads
    // bei — kein geteilter Puffer, keine Reuse-Race.
    TaskHandle_t worker = NULL;
    BaseType_t rc = xTaskCreateWithCaps(
        connect_scan_task, "WlanApScan", 8192, scan, 4, &worker, MALLOC_CAP_SPIRAM);

    if(rc == pdPASS) {
        s_connect_scan = scan;
    } else {
        __atomic_store_n(&scan->refs, 1, __ATOMIC_RELAXED);
        connect_scan_release(scan);
        FURI_LOG_E(WLAN_APP_TAG, "Unable to start AP scan worker: %ld", (long)rc);
    }

    return rc == pdPASS;
}

// ============================================================================
// TAMPILKAN HASIL SCAN
// ============================================================================
static void connect_scan_show_results(WlanApp* app, WlanConnectScan* scan) {
    app->ap_count = 0;

    if(scan && scan->success) {
        // FIX #3 (lanjutan): bounds check di sisi consumer juga
        uint16_t n = scan->count;
        if(n > WLAN_APP_MAX_APS) n = WLAN_APP_MAX_APS;

        for(uint16_t i = 0; i < n; ++i) {
            WlanApRecord* record = &app->ap_records[app->ap_count++];
            *record = scan->records[i];
            record->has_password =
                !record->is_open && wlan_password_exists(record->ssid);
        }
    }

    wlan_connect_view_clear(app->view_connect);
    for(uint16_t i = 0; i < app->ap_count; ++i) {
        WlanApRecord* record = &app->ap_records[i];
        wlan_connect_view_add_ap(
            app->view_connect, record->ssid, record->is_open || record->has_password, i);
    }

    uint8_t restore = scene_manager_get_scene_state(app->scene_manager, WlanAppSceneConnect);
    wlan_connect_view_set_selected(app->view_connect, restore);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewConnect);
}

// ============================================================================
// LONG-PRESS OK: BUKA MENU KONTEKSTUAL
// ============================================================================
static void connect_open_menu_for_selected(WlanApp* app) {
    uint8_t sel = wlan_connect_view_get_selected(app->view_connect);
    if(sel >= app->ap_count) return;
    app->ap_selected_index = sel;

    wlan_connect_view_clear_menu(app->view_connect);
    wlan_connect_view_add_menu_item(app->view_connect, "Handshake", CONNECT_MENU_HANDSHAKE);
    wlan_connect_view_add_menu_item(app->view_connect, "Deauth", CONNECT_MENU_DEAUTH);
    wlan_connect_view_add_menu_item(app->view_connect, "Sniffer", CONNECT_MENU_SNIFFER);
    wlan_connect_view_add_menu_item(app->view_connect, "Evil Portal", CONNECT_MENU_PORTAL);
    wlan_connect_view_open_menu(app->view_connect);
}

// ============================================================================
// SCENE: on_enter
// ============================================================================
void wlan_app_scene_connect_on_enter(void* context) {
    WlanApp* app = context;

    app->ap_count = 0;
    s_connect_scan = NULL;

    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewLoading);

    if(!connect_scan_start()) {
        connect_scan_show_results(app, NULL);
    }
}

// ============================================================================
// SCENE: on_event
// ============================================================================
bool wlan_app_scene_connect_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    // Scan selesai? Ambil hasil.
    if(event.type == SceneManagerEventTypeTick &&
       s_connect_scan &&
       __atomic_load_n(&s_connect_scan->complete, __ATOMIC_ACQUIRE)) {
        WlanConnectScan* scan = s_connect_scan;
        s_connect_scan = NULL;
        connect_scan_show_results(app, scan);
        connect_scan_release(scan);
        consumed = true;
    } else if(event.type == SceneManagerEventTypeCustom &&
              event.event == WlanAppCustomEventApSelected) {
        uint8_t sel = wlan_connect_view_get_selected(app->view_connect);
        if(sel < app->ap_count) {
            app->ap_selected_index = sel;
            scene_manager_set_scene_state(app->scene_manager, WlanAppSceneConnect, sel);
            if(app->deauth_pick_flow) {
                // Deauth-Ziel-Picker: gewählten AP als Target übernehmen und
                // direkt in die Single-Target-Deauth-Scene. channel_mode bleibt
                // aus → Target-Mode in network_deauth. Das Flag NICHT hier
                // löschen: so bleibt nach Back aus der Deauth-Scene die Liste
                // weiter ein Deauth-Picker (erneut wählen → wieder Deauth). Es
                // wird erst in scene_main (on_enter) zurückgesetzt.
                memcpy(&app->target_ap, &app->ap_records[sel], sizeof(WlanApRecord));
                app->target_selected = true;
                app->channel_mode_active = false;
                app->deauth_auto = false;
                scene_manager_next_scene(app->scene_manager, WlanAppSceneNetworkDeauth);
            } else {
                scene_manager_next_scene(app->scene_manager, WlanAppSceneSsidScreen);
            }
        }
        consumed = true;
    } else if(event.type == SceneManagerEventTypeCustom &&
              event.event == WlanAppCustomEventConnectLongOk) {
        connect_open_menu_for_selected(app);
        consumed = true;
    } else if(event.type == SceneManagerEventTypeCustom &&
              event.event == WlanAppCustomEventConnectMenuOk) {
        uint8_t mi = wlan_connect_view_get_menu_selected(app->view_connect);
        WlanConnectMenuItem mit = wlan_connect_view_get_menu_item(app->view_connect, mi);
        wlan_connect_view_close_menu(app->view_connect);

        if(app->ap_selected_index < app->ap_count) {
            memcpy(&app->target_ap, &app->ap_records[app->ap_selected_index],
                sizeof(WlanApRecord));
            app->target_selected = true;
        }

        switch(mit.user_id) {
        case CONNECT_MENU_HANDSHAKE:
            scene_manager_next_scene(app->scene_manager, WlanAppSceneHandshake);
            break;
        case CONNECT_MENU_DEAUTH:
            scene_manager_next_scene(app->scene_manager, WlanAppSceneNetworkDeauth);
            break;
        case CONNECT_MENU_SNIFFER:
            scene_manager_next_scene(app->scene_manager, WlanAppScenePackageSniffer);
            break;
        case CONNECT_MENU_PORTAL:
            if(app->target_ap.ssid[0]) {
                strncpy(app->evil_portal_ssid, app->target_ap.ssid,
                    sizeof(app->evil_portal_ssid) - 1);
                app->evil_portal_ssid[sizeof(app->evil_portal_ssid) - 1] = '\0';
            }
            if(app->target_ap.channel) {
                app->evil_portal_channel = app->target_ap.channel;
            }
            scene_manager_next_scene(app->scene_manager, WlanAppSceneEvilPortalMenu);
            break;
        }
        consumed = true;
    }

    return consumed;
}

// ============================================================================
// SCENE: on_exit
// ============================================================================
void wlan_app_scene_connect_on_exit(void* context) {
    WlanApp* app = context;

    if(s_connect_scan) {
        WlanConnectScan* scan = s_connect_scan;
        s_connect_scan = NULL;
        __atomic_store_n(&scan->cancelled, true, __ATOMIC_RELEASE);
        connect_scan_release(scan);
    }

    wlan_connect_view_clear(app->view_connect);
}
