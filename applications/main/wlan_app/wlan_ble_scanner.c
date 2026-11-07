#include "wlan_ble_scanner.h"
#include "wlan_oui.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <storage/storage.h>
#include <furi_hal_rtc.h>
#include <datetime/datetime.h>

// Proyek ini memakai Bluedroid (CONFIG_BT_BLUEDROID_ENABLED), bukan NimBLE.
// Scanner ini bring-up stack-nya sendiri — scene wardriving tidak memanggil
// HAL BLE lain lebih dulu — lalu kembalikan btshim saat berhenti, persis
// seperti ble_walk_hal / ble_spam_hal.
#include <sdkconfig.h>
#if CONFIG_BT_NIMBLE_ENABLED
#include <ble_core.h>
#include <host/ble_gap.h>
#include <host/ble_hs.h>
#include <host/ble_hs_id.h>
#include <nimble/ble.h>
#else
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_gap_ble_api.h>
#endif
#include <btshim.h>
#include <wifi.h>
#include <furi.h>

#define TAG "BleScan"

// ============================================================================
// STATE
// ============================================================================
static volatile bool         s_worker_alive = false;  // true selama worker hidup
static volatile bool         s_running = false;        // scanning aktif (stack up)
static volatile bool         s_pause = false;
static volatile bool         s_scan_active = false;    // controller lagi scanning
static volatile bool         s_cmd_inflight = false;   // start/stop command pending
static volatile bool         s_stop_requested = false;
static volatile bool         s_bringup_failed = false; // true kalau stack bring-up gagal
#if CONFIG_BT_NIMBLE_ENABLED
static bool                  s_nimble_host_takeover = false;
#endif

static BleDeviceEntry        s_devices[BLE_SCAN_MAX_DEVICES];
static volatile uint16_t     s_device_count = 0;

static SemaphoreHandle_t     s_mtx = NULL;
static WlanOuiTable*         s_oui = NULL;  // dimuat worker; NULL = file SD tak ada

static BleDeviceView         s_temp_view[BLE_SCAN_MAX_DEVICES];

// ============================================================================
// HELPERS
// ============================================================================
static inline uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool ensure_dir(Storage* storage, const char* path) {
    if(storage_common_exists(storage, path)) return true;
    FS_Error e = storage_common_mkdir(storage, path);
    if(e != FSE_OK && e != FSE_EXIST) {
        ESP_LOGE(TAG, "mkdir %s failed: %d", path, e);
        return false;
    }
    return true;
}

// ============================================================================
// AP STORE
// ============================================================================
static int find_device_nolock(const uint8_t* mac) {
    for(uint16_t i = 0; i < s_device_count; i++) {
        if(s_devices[i].valid && memcmp(s_devices[i].mac, mac, 6) == 0) return i;
    }
    return -1;
}

static int add_or_update_device(const uint8_t* mac, int8_t rssi,
                                  const char* name, bool has_name,
                                  uint16_t company_id, bool has_mfr,
                                  BleAddrType addr_type) {
    int idx = find_device_nolock(mac);
    uint32_t t = now_ms();

    if(idx >= 0) {
        BleDeviceEntry* d = &s_devices[idx];
        d->rssi = rssi;
        if(rssi > d->rssi_max) d->rssi_max = rssi;
        if(rssi < d->rssi_min) d->rssi_min = rssi;
        d->last_seen_ms = t;
        d->adv_count++;
        if(has_name && name && name[0] && !d->has_name) {
            strncpy(d->name, name, sizeof(d->name) - 1);
            d->name[sizeof(d->name) - 1] = 0;
            d->has_name = true;
        }
        if(has_mfr && !d->has_mfr_data) {
            d->company_id = company_id;
            d->has_mfr_data = true;
        }
        return idx;
    }

    if(s_device_count >= BLE_SCAN_MAX_DEVICES) return -1;
    idx = s_device_count;
    BleDeviceEntry* d = &s_devices[idx];
    memset(d, 0, sizeof(*d));

    memcpy(d->mac, mac, 6);
    d->rssi = rssi;
    d->rssi_max = rssi;
    d->rssi_min = rssi;
    d->first_seen_ms = t;
    d->last_seen_ms = t;
    d->adv_count = 1;
    d->addr_type = addr_type;
    d->valid = true;

    if(has_name && name && name[0]) {
        strncpy(d->name, name, sizeof(d->name) - 1);
        d->name[sizeof(d->name) - 1] = 0;
        d->has_name = true;
    }
    if(has_mfr) {
        d->company_id = company_id;
        d->has_mfr_data = true;
    }

    if(d->addr_type == BleAddrPublic) {
        const char* vendor = s_oui ? wlan_oui_lookup(s_oui, mac) : NULL;
        if(vendor) {
            strncpy(d->vendor, vendor, sizeof(d->vendor) - 1);
            d->vendor[sizeof(d->vendor) - 1] = 0;
        } // miss → vendor tetap kosong (d sudah di-memset 0)
    } else {
        strncpy(d->vendor, "Random", sizeof(d->vendor) - 1);
        d->vendor[sizeof(d->vendor) - 1] = 0;
    }

    s_device_count++;
    return idx;
}

// ============================================================================
// AD PARSER
// ============================================================================
static void parse_ad_data(const uint8_t* data, uint8_t len,
                            char* name_out, bool* has_name,
                            uint16_t* company_id, bool* has_mfr,
                            uint8_t* flags, bool* has_flags) {
    *has_name = false;
    *has_mfr = false;
    *has_flags = false;
    *company_id = 0;

    int pos = 0;
    while(pos + 2 <= len) {
        uint8_t ad_len = data[pos];
        if(ad_len == 0) break;
        if(pos + 1 + ad_len > len) break;
        uint8_t ad_type = data[pos + 1];
        const uint8_t* ad_data = &data[pos + 2];
        uint8_t ad_data_len = ad_len - 1;

        switch(ad_type) {
        case 0x01:
            if(ad_data_len >= 1) { *flags = ad_data[0]; *has_flags = true; }
            break;
        case 0x08:
        case 0x09:
            if(ad_data_len > 0) {
                int nl = ad_data_len > 31 ? 31 : ad_data_len;
                memcpy(name_out, ad_data, nl);
                name_out[nl] = 0;
                *has_name = true;
            }
            break;
        case 0xFF:
            if(ad_data_len >= 2) {
                *company_id = (uint16_t)ad_data[0] | ((uint16_t)ad_data[1] << 8);
                *has_mfr = true;
            }
            break;
        }
        pos += 1 + ad_len;
    }
}

// ============================================================================
// GAP EVENT HANDLER (Bluedroid)
// ============================================================================
#if CONFIG_BT_NIMBLE_ENABLED
static void ble_scan_process_report(
    const ble_addr_t* addr,
    int8_t rssi,
    const uint8_t* data,
    uint8_t data_len) {
    if(!addr || !data || data_len == 0 || !s_running || s_pause) return;

    uint8_t mac[6];
    for(size_t i = 0; i < sizeof(mac); ++i) mac[i] = addr->val[5U - i];

    char name[32] = {0};
    bool has_name = false;
    uint16_t company_id = 0;
    bool has_mfr = false;
    uint8_t flags = 0;
    bool has_flags = false;
    parse_ad_data(data, data_len, name, &has_name, &company_id, &has_mfr, &flags, &has_flags);
    if(s_mtx && xSemaphoreTake(s_mtx, 0) == pdTRUE) {
        add_or_update_device(mac, rssi, name, has_name, company_id, has_mfr,
                             addr->type == BLE_ADDR_PUBLIC ? BleAddrPublic : BleAddrRandom);
        xSemaphoreGive(s_mtx);
    }
}

static int ble_gap_event_handler(struct ble_gap_event* event, void* context) {
    (void)context;
    if(!event) return 0;
    switch(event->type) {
    case BLE_GAP_EVENT_DISC: {
        const struct ble_gap_disc_desc* disc = &event->disc;
        ble_scan_process_report(&disc->addr, disc->rssi, disc->data, disc->length_data);
        break;
    }
#if CONFIG_BT_NIMBLE_EXT_ADV
    case BLE_GAP_EVENT_EXT_DISC: {
        const struct ble_gap_ext_disc_desc* disc = &event->ext_disc;
        if(disc->data_status != BLE_GAP_EXT_ADV_DATA_STATUS_INCOMPLETE) {
            ble_scan_process_report(&disc->addr, disc->rssi, disc->data, disc->length_data);
        }
        break;
    }
#endif
    case BLE_GAP_EVENT_DISC_COMPLETE:
        s_scan_active = false;
        s_cmd_inflight = false;
        break;
    default:
        break;
    }
    return 0;
}
#else
static BleAddrType map_addr_type(esp_ble_addr_type_t t) {
    return (t == BLE_ADDR_TYPE_PUBLIC) ? BleAddrPublic : BleAddrRandom;
}

static void ble_gap_event_handler(esp_gap_ble_cb_event_t event,
                                   esp_ble_gap_cb_param_t* param) {
    switch(event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        if(param->scan_param_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            esp_ble_gap_start_scanning(0); // indefinite; SCAN_START_COMPLETE clears inflight
        } else {
            ESP_LOGW(TAG, "scan param set failed: %d", param->scan_param_cmpl.status);
            s_cmd_inflight = false;
        }
        break;

    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        s_scan_active = (param->scan_start_cmpl.status == ESP_BT_STATUS_SUCCESS);
        s_cmd_inflight = false;
        if(s_scan_active) ESP_LOGI(TAG, "BLE scan started");
        break;

    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        s_scan_active = false;
        s_cmd_inflight = false;
        break;

    case ESP_GAP_BLE_SCAN_RESULT_EVT:
        if(param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
            if(!s_running || s_pause) break;

            uint8_t mac[6];
            memcpy(mac, param->scan_rst.bda, 6);

            char name[32] = {0};
            bool has_name = false;
            uint16_t company_id = 0;
            bool has_mfr = false;
            uint8_t flags = 0;
            bool has_flags = false;

            uint16_t total = (uint16_t)param->scan_rst.adv_data_len +
                             (uint16_t)param->scan_rst.scan_rsp_len;
            if(total > 62) total = 62;
            if(total > 0) {
                parse_ad_data(param->scan_rst.ble_adv, (uint8_t)total,
                               name, &has_name, &company_id, &has_mfr,
                               &flags, &has_flags);
            }

            // try-lock 0 timeout — JANGAN block BT host task
            if(!s_mtx) break;
            if(xSemaphoreTake(s_mtx, 0) != pdTRUE) break;
            add_or_update_device(mac, param->scan_rst.rssi, name, has_name,
                                  company_id, has_mfr,
                                  map_addr_type(param->scan_rst.ble_addr_type));
            xSemaphoreGive(s_mtx);
        }
        break;

    default:
        break;
    }
}
#endif

// ============================================================================
// BLUEDROID STACK BRING-UP / TEARDOWN (dijalankan di worker, bukan GUI thread)
// ============================================================================
static bool ble_stack_up(void) {
    // BT & WiFi teilen sich Radio und internes DRAM. Ist WiFi global aktiv, würde
    // der Bluedroid-Bringup ein OOM riskieren (vgl. BLE_INIT: Malloc failed). Analog
    // zum Lock-Menü (desktop_scene_lock_menu.c: wifi_disable() vor bt_start_stack())
    // deshalb zuerst WiFi global abschalten. ble_spam_hal/ble_walk_hal betreiben
    // keinen eigenen WiFi-Umgang → dieser explizite wifi_disable() ist die
    // Verbesserung für diesen Scanner.
    Wifi* wifi = furi_record_open(RECORD_WIFI);
    if(wifi_is_enabled(wifi)) wifi_disable(wifi);
    furi_record_close(RECORD_WIFI);

    Bt* bt = furi_record_open(RECORD_BT);
    bt_stop_stack(bt);
    furi_record_close(RECORD_BT);
    furi_delay_ms(100);

#if CONFIG_BT_NIMBLE_ENABLED
    if(ble_core_stack_is_up()) {
        ESP_LOGE(TAG, "refusing scanner takeover while btshim still owns the NimBLE host");
        return false;
    }
    s_nimble_host_takeover = true;
    esp_err_t err = ble_core_stack_up();
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE host start: %s", esp_err_to_name(err));
        return false;
    }
    return true;
#else
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_bt_controller_init(&bt_cfg);
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "controller init: %s", esp_err_to_name(err));
        return false;
    }
    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "controller enable: %s", esp_err_to_name(err));
        return false;
    }

    esp_bluedroid_config_t bd_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    err = esp_bluedroid_init_with_cfg(&bd_cfg);
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "bluedroid init: %s", esp_err_to_name(err));
        return false;
    }
    err = esp_bluedroid_enable();
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "bluedroid enable: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_ble_gap_register_callback(ble_gap_event_handler);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "gap register: %s", esp_err_to_name(err));
        return false;
    }
    return true;
#endif
}

static bool ble_stop_scan_command(void);

static void ble_stack_down(void) {
#if CONFIG_BT_NIMBLE_ENABLED
    if(!s_nimble_host_takeover) return;
    if(s_scan_active) {
        (void)ble_stop_scan_command();
        for(int i = 0; i < 20 && s_scan_active; i++) furi_delay_ms(5);
    }
    esp_err_t err = ble_core_stack_down();
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE host teardown: %s", esp_err_to_name(err));
        return;
    }
    s_nimble_host_takeover = false;
    Bt* bt = furi_record_open(RECORD_BT);
    bt_start_stack(bt);
    furi_record_close(RECORD_BT);
#else
    if(s_scan_active) {
        esp_ble_gap_stop_scanning();
        for(int i = 0; i < 20 && s_scan_active; i++) furi_delay_ms(5);
    }

    if(esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_UNINITIALIZED) {
        esp_bluedroid_disable();
        furi_delay_ms(50);
        esp_bluedroid_deinit();
        furi_delay_ms(50);
    }
    if(esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_IDLE) {
        esp_bt_controller_disable();
        furi_delay_ms(50);
    }
    if(esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
        esp_bt_controller_deinit();
        furi_delay_ms(50);
    }

    // Kembalikan btshim (BLE serial / qFlipper) seperti semula.
    Bt* bt = furi_record_open(RECORD_BT);
    bt_start_stack(bt);
    furi_record_close(RECORD_BT);
#endif
}

static bool ble_start_scan_command(void) {
#if CONFIG_BT_NIMBLE_ENABLED
    if(s_scan_active || s_cmd_inflight || !s_running || s_pause) return true;
    uint8_t own_addr_type = BLE_OWN_ADDR_PUBLIC;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if(rc != 0) {
        ESP_LOGW(TAG, "cannot infer NimBLE scanner address type: %d", rc);
        return false;
    }
    const struct ble_gap_disc_params params = {
        .itvl = 0x60,
        .window = 0x30,
        .filter_policy = BLE_HCI_SCAN_FILT_NO_WL,
        .limited = 0,
        .passive = 1,
        .filter_duplicates = 0,
    };
    s_cmd_inflight = true;
    rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &params, ble_gap_event_handler, NULL);
    s_cmd_inflight = false;
    if(rc == 0) s_scan_active = true;
    else ESP_LOGW(TAG, "NimBLE scan start failed: %d", rc);
    return rc == 0;
#else
    s_cmd_inflight = true;
    esp_err_t err = esp_ble_gap_start_scanning(0);
    if(err != ESP_OK) s_cmd_inflight = false;
    return err == ESP_OK;
#endif
}

static bool ble_stop_scan_command(void) {
#if CONFIG_BT_NIMBLE_ENABLED
    if(!s_scan_active) return true;
    s_cmd_inflight = true;
    int rc = ble_gap_disc_cancel();
    if(rc == 0 || rc == BLE_HS_EALREADY) {
        s_scan_active = false;
        s_cmd_inflight = false;
        return true;
    }
    s_cmd_inflight = false;
    return false;
#else
    s_cmd_inflight = true;
    esp_err_t err = esp_ble_gap_stop_scanning();
    if(err != ESP_OK) s_cmd_inflight = false;
    return err == ESP_OK;
#endif
}

static void ble_apply_scan_params(void) {
#if CONFIG_BT_NIMBLE_ENABLED
    (void)ble_start_scan_command();
#else
    esp_ble_scan_params_t scan_params = {
        .scan_type = BLE_SCAN_TYPE_PASSIVE,
        .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval = 0x60, // 60ms
        .scan_window = 0x30,   // 30ms
    };
    s_cmd_inflight = true;
    esp_err_t err = esp_ble_gap_set_scan_params(&scan_params);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "set_scan_params: %s", esp_err_to_name(err));
        s_cmd_inflight = false;
    }
#endif
}

// ============================================================================
// WORKER — memegang seluruh lifecycle stack supaya GUI thread tak pernah blok
// ============================================================================
static void ble_scan_worker(void* arg) {
    (void)arg;
    ESP_LOGI(TAG, "BLE scan worker started");

    if(!ble_stack_up()) {
        ESP_LOGE(TAG, "stack bring-up failed");
        ble_stack_down(); // kembalikan btshim walau gagal
        s_bringup_failed = true; // sinyal ke UI: tampilkan error, bukan "Scanning..."
        s_running = false;
        s_scan_active = false;
        s_worker_alive = false;
        vTaskDelete(NULL);
        return;
    }

    s_oui = wlan_oui_load(); // NULL kalau /ext/apps_data/wifi/mac-vendor.txt tak ada

    s_running = true;
    ble_apply_scan_params(); // SCAN_PARAM_SET -> start_scanning di GAP callback

    while(!s_stop_requested) {
        bool want_scan = !s_pause;
        if(!s_cmd_inflight) {
            if(want_scan && !s_scan_active) {
                (void)ble_start_scan_command();
            } else if(!want_scan && s_scan_active) {
                (void)ble_stop_scan_command();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    ble_stack_down(); // scan sudah berhenti di sini -> aman free tabel OUI
    if(s_oui) { wlan_oui_free(s_oui); s_oui = NULL; }
    s_running = false;
    s_scan_active = false;
    ESP_LOGI(TAG, "BLE scan worker exit");
    s_worker_alive = false;
    vTaskDelete(NULL);
}

// ============================================================================
// PUBLIC API
// ============================================================================
bool wlan_ble_scan_start(void) {
    if(s_worker_alive) return true;
    if(!s_mtx) s_mtx = xSemaphoreCreateMutex();
    if(!s_mtx) return false;

    if(xSemaphoreTake(s_mtx, pdMS_TO_TICKS(200)) == pdTRUE) {
        memset(s_devices, 0, sizeof(s_devices));
        s_device_count = 0;
        xSemaphoreGive(s_mtx);
    }

    s_stop_requested = false;
    s_pause = false;
    s_scan_active = false;
    s_cmd_inflight = false;
    s_running = false;
    s_bringup_failed = false;
#if CONFIG_BT_NIMBLE_ENABLED
    s_nimble_host_takeover = false;
#endif
    s_worker_alive = true; // set sebelum create agar is_stopped() langsung false

    if(xTaskCreatePinnedToCore(ble_scan_worker, "BleScan",
                                4096, NULL, 8, NULL, FURI_CORE_ID_SECONDARY) != pdPASS) {
        s_worker_alive = false;
        s_bringup_failed = true; // task gagal dibuat → UI harus tahu
        return false;
    }

    ESP_LOGI(TAG, "Started");
    return true;
}

// ASYNC — hanya set flag; teardown nyata jalan di worker (blocking + delay).
void wlan_ble_scan_stop(void) {
    s_stop_requested = true;
    s_pause = true;
}

// true kalau worker sudah benar-benar exit (safe untuk pop scene / re-enter).
bool wlan_ble_scan_is_stopped(void) {
    return !s_worker_alive;
}

bool wlan_ble_scan_is_running(void) { return s_running; }

// true kalau bring-up stack (atau pembuatan task) gagal — UI pakai untuk
// menampilkan pesan error alih-alih "Scanning..." yang menggantung.
bool wlan_ble_scan_failed(void) { return s_bringup_failed; }

void wlan_ble_scan_clear(void) {
    if(!s_mtx) return;
    if(xSemaphoreTake(s_mtx, pdMS_TO_TICKS(100)) != pdTRUE) return;
    memset(s_devices, 0, sizeof(s_devices));
    s_device_count = 0;
    xSemaphoreGive(s_mtx);
}

void wlan_ble_scan_pause(void)  { s_pause = true; }
void wlan_ble_scan_resume(void) { s_pause = false; }
uint16_t wlan_ble_scan_get_count(void) { return s_device_count; }

// Snapshot pakai timeout 20ms (bukan portMAX_DELAY) supaya GUI tak hang.
static int cmp_view_rssi(const void* a, const void* b) {
    return (int)((const BleDeviceView*)b)->rssi -
           (int)((const BleDeviceView*)a)->rssi;
}

uint16_t wlan_ble_scan_snapshot_view(BleDeviceView* out, uint16_t max) {
    if(!out || max == 0) return 0;
    if(max > BLE_SCAN_MAX_DEVICES) max = BLE_SCAN_MAX_DEVICES;
    if(!s_mtx) return 0;

    if(xSemaphoreTake(s_mtx, pdMS_TO_TICKS(20)) != pdTRUE) {
        return 0;
    }

    uint16_t n = 0;
    for(uint16_t i = 0; i < s_device_count; i++) {
        if(!s_devices[i].valid) continue;
        if(n >= BLE_SCAN_MAX_DEVICES) break;
        BleDeviceEntry* d = &s_devices[i];
        BleDeviceView* v = &s_temp_view[n++];
        memcpy(v->mac, d->mac, 6);
        strncpy(v->name, d->name, sizeof(v->name) - 1);
        v->name[sizeof(v->name) - 1] = 0;
        v->rssi = d->rssi;
        v->company_id = d->company_id;
        v->addr_type = d->addr_type;
        v->has_name = d->has_name;
        v->has_mfr_data = d->has_mfr_data;
        v->adv_count = d->adv_count;
    }

    xSemaphoreGive(s_mtx);

    qsort(s_temp_view, n, sizeof(BleDeviceView), cmp_view_rssi);

    uint16_t copy_n = (n < max) ? n : max;
    memcpy(out, s_temp_view, sizeof(BleDeviceView) * copy_n);
    return copy_n;
}

bool wlan_ble_scan_get_full_by_mac(const uint8_t mac[6], BleDeviceEntry* out) {
    if(!out || !mac || !s_mtx) return false;
    if(xSemaphoreTake(s_mtx, pdMS_TO_TICKS(20)) != pdTRUE) return false;
    int idx = find_device_nolock(mac);
    if(idx < 0) { xSemaphoreGive(s_mtx); return false; }
    *out = s_devices[idx];
    xSemaphoreGive(s_mtx);
    return true;
}

// ============================================================================
// DEINIT — safety saat app keluar total. Blocking: tunggu worker teardown.
// ============================================================================
void wlan_ble_scan_deinit(void) {
    if(!s_worker_alive) return;
    ESP_LOGI(TAG, "deinit: waiting for worker teardown");
    s_stop_requested = true;
    s_pause = true;
    for(int i = 0; i < 300 && s_worker_alive; i++) {
        furi_delay_ms(10);
    }
    ESP_LOGI(TAG, "deinit done");
}

// ============================================================================
// CSV EXPORT
// ============================================================================
bool wlan_ble_scan_export_csv(const char* path, uint16_t* out_count) {
    if(!path) return false;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    if(!ensure_dir(storage, "/ext/apps_data")) {
        furi_record_close(RECORD_STORAGE);
        return false;
    }
    if(!ensure_dir(storage, "/ext/apps_data/wlan")) {
        furi_record_close(RECORD_STORAGE);
        return false;
    }

    File* file = storage_file_alloc(storage);
    if(!storage_file_open(file, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        ESP_LOGE(TAG, "Cannot open %s", path);
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return false;
    }

    const char* h1 =
        "WigleWifi-1.6,appRelease=1.0,model=FlipperZero,release=1.0,"
        "device=Flipper,display=Flipper,board=ESP32,brand=Flipper\n";
    const char* h2 =
        "MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,"
        "CurrentLatitude,CurrentLongitude,AltitudeMeters,"
        "AccuracyMeters,RCOIs,MfgrId,Type\n";

    if(storage_file_write(file, h1, strlen(h1)) != strlen(h1) ||
       storage_file_write(file, h2, strlen(h2)) != strlen(h2)) {
        ESP_LOGE(TAG, "Header write failed");
        storage_file_close(file);
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return false;
    }

    if(!s_mtx) {
        storage_file_close(file);
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return false;
    }
    if(xSemaphoreTake(s_mtx, pdMS_TO_TICKS(200)) != pdTRUE) {
        storage_file_close(file);
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return false;
    }

    uint16_t count = 0;
    char line[200];
    bool write_error = false;

    // FirstSeen ehrlich pro Gerät: echte Wall-Clock statt einer Konstante.
    // Basiszeit = jetzige RTC-Zeit; davon den Uptime-Offset (now - first_seen)
    // abziehen → der reale Zeitpunkt des ersten Advertisements. Ist die RTC nicht
    // gestellt, spiegelt der Wert ehrlich die (falsche) RTC wider, statt zu
    // suggerieren, es sei ein exakter Zeitstempel.
    DateTime rtc_now;
    furi_hal_rtc_get_datetime(&rtc_now);
    uint32_t now_epoch = datetime_datetime_to_timestamp(&rtc_now);
    uint32_t uptime_now = now_ms();

    for(uint16_t i = 0; i < s_device_count && !write_error; i++) {
        if(!s_devices[i].valid) continue;
        BleDeviceEntry* d = &s_devices[i];

        char name_escaped[40];
        int j = 0;
        const char* name = d->has_name ? d->name : "[unknown]";
        for(int k = 0; name[k] && j < (int)sizeof(name_escaped) - 3; k++) {
            if(name[k] == ',' || name[k] == '"') name_escaped[j++] = '\\';
            name_escaped[j++] = name[k];
        }
        name_escaped[j] = 0;

        char mfr_str[8] = {0};
        if(d->has_mfr_data) snprintf(mfr_str, sizeof(mfr_str), "%04X", d->company_id);

        // first_seen_ms → Wall-Clock (UTC): now_epoch - (uptime_now - first_seen)/1000.
        uint32_t age_ms = (uptime_now >= d->first_seen_ms) ? (uptime_now - d->first_seen_ms) : 0;
        uint32_t fs_epoch = (now_epoch > age_ms / 1000) ? (now_epoch - age_ms / 1000) : now_epoch;
        DateTime fs_dt;
        datetime_timestamp_to_datetime(fs_epoch, &fs_dt);
        char fs_str[32];
        snprintf(fs_str, sizeof(fs_str), "%04u-%02u-%02u %02u:%02u:%02u",
                 fs_dt.year, fs_dt.month, fs_dt.day, fs_dt.hour, fs_dt.minute, fs_dt.second);

        // Kein GPS angebunden → Lat/Lon/Alt/Accuracy bleiben LEER (nicht 0.0).
        // 0.0,0.0 ist "Null Island", eine echte Position vor Afrika und würde als
        // gültiger Fix missverstanden; leere Felder heißen eindeutig "keine Position".
        int len = snprintf(line, sizeof(line),
            "%02X:%02X:%02X:%02X:%02X:%02X,%s,[BLE],%s,0,%d"
            ",,,,,0,%s,BLE\n",
            d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5],
            name_escaped, fs_str, d->rssi,
            mfr_str[0] ? mfr_str : "");

        if(len > 0) {
            if(storage_file_write(file, line, len) != (size_t)len) {
                write_error = true;
                break;
            }
            count++;
        }
    }

    xSemaphoreGive(s_mtx);
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);

    if(write_error) return false;
    if(out_count) *out_count = count;
    ESP_LOGI(TAG, "Exported %u BLE devices to %s", count, path);
    return true;
}
