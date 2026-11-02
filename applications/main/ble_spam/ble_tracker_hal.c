#include "ble_tracker_hal.h"

#include <sdkconfig.h>

#include <esp_log.h>
#include <furi.h>
#include <string.h>

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

#define TAG "BleTracker"

static TrackerDevice s_devices[TRACKER_MAX_DEVICES];
static uint16_t s_device_count = 0;
static volatile bool s_scanning = false;
static bool s_host_owned = false;

// ---------------------------------------------------------------------------
// Apple Proximity Pairing model table — ported from Poseidon ble_db.cpp:154
// ---------------------------------------------------------------------------

typedef struct {
    uint8_t key;
    const char* name;
} apple_pp_model_t;

static const apple_pp_model_t APPLE_PP[] = {
    {0x01, "AirPods 1"},
    {0x02, "AirPods Pro"},
    {0x03, "AirPods Max"},
    {0x04, "AppleTV Setup"},
    {0x05, "Beats X"},
    {0x06, "Beats Solo 3"},
    {0x07, "Beats Studio 3"},
    {0x09, "Beats Studio Pro"},
    {0x0A, "Beats Fit Pro"},
    {0x0B, "Beats Flex"},
    {0x0C, "Beats Solo Pro"},
    {0x0D, "Beats Studio Buds"},
    {0x0E, "Beats Studio Buds+"},
    {0x0F, "AirPods 2"},
    {0x10, "AirPods 3"},
    {0x11, "AirPods 4"},
    {0x13, "AirPods Pro 2"},
    {0x14, "AirPods Pro 2 USB-C"},
    {0x19, "PowerBeats Pro"},
    {0x1A, "Beats Fit Pro 2"},
    {0x00, NULL},
};

static const char* apple_pp_lookup(uint8_t model) {
    for(const apple_pp_model_t* p = APPLE_PP; p->name; ++p) {
        if(p->key == model) return p->name;
    }
    return "Apple pairing";
}

// ---------------------------------------------------------------------------
// AD parsing helpers
// ---------------------------------------------------------------------------

// Walks one BLE advertisement TLV buffer and looks for any of:
//   - manufacturer data (type 0xFF) for tracker matchers
//   - 16-bit Service UUID lists (types 0x02/0x03) for Tile (0xFEED, 0xFD84)
// Outputs classification + optional model name into kind_out, name_out.
// Returns true if a tracker kind was identified.
static bool classify_buffer(
    const uint8_t* buf,
    uint8_t len,
    TrackerKind* kind_out,
    char* name_out,
    uint8_t name_out_sz) {
    uint8_t pos = 0;
    while(pos + 1 < len) {
        uint8_t fld_len = buf[pos];
        if(fld_len == 0) break;
        if(pos + fld_len >= len) break;
        uint8_t type = buf[pos + 1];
        const uint8_t* val = &buf[pos + 2];
        uint8_t val_len = fld_len - 1;

        if(type == 0xFF && val_len >= 2) {
            uint16_t cid = val[0] | (val[1] << 8);
            // Apple
            if(cid == 0x004C && val_len >= 3) {
                uint8_t sub = val[2];
                if(sub == 0x12) {
                    *kind_out = TrackerKindAirTag;
                    name_out[0] = '\0';
                    return true;
                }
                if(sub == 0x10) {
                    *kind_out = TrackerKindAppleNearby;
                    name_out[0] = '\0';
                    return true;
                }
                if(sub == 0x07 && val_len >= 4) {
                    *kind_out = TrackerKindApplePP;
                    strncpy(name_out, apple_pp_lookup(val[3]), name_out_sz - 1);
                    name_out[name_out_sz - 1] = '\0';
                    return true;
                }
            }
            // Samsung
            if(cid == 0x0075) {
                *kind_out = TrackerKindSmartTag;
                name_out[0] = '\0';
                return true;
            }
        } else if((type == 0x02 || type == 0x03) && val_len >= 2) {
            // Incomplete / Complete list of 16-bit Service Class UUIDs
            for(uint8_t i = 0; i + 1 < val_len; i += 2) {
                uint16_t u = val[i] | (val[i + 1] << 8);
                if(u == 0xFEED || u == 0xFD84) {
                    *kind_out = TrackerKindTile;
                    name_out[0] = '\0';
                    return true;
                }
            }
        }

        pos += fld_len + 1;
    }
    return false;
}

static bool classify_advert(
    const uint8_t* buf,
    uint8_t adv_len,
    uint8_t scan_rsp_len,
    TrackerKind* kind_out,
    char* name_out,
    uint8_t name_out_sz) {
    if(classify_buffer(buf, adv_len, kind_out, name_out, name_out_sz)) return true;
    if(scan_rsp_len > 0 &&
       classify_buffer(buf + adv_len, scan_rsp_len, kind_out, name_out, name_out_sz))
        return true;
    return false;
}

// ---------------------------------------------------------------------------
// GAP callback
// ---------------------------------------------------------------------------

#if CONFIG_BT_NIMBLE_ENABLED
static void tracker_store_match(
    const uint8_t address[6],
    uint8_t address_type,
    int8_t rssi,
    TrackerKind kind,
    const char* name) {
    const uint32_t now = furi_get_tick();
    int found = -1;
    for(int i = 0; i < s_device_count; ++i) {
        if(memcmp(s_devices[i].addr, address, 6) == 0) {
            found = i;
            break;
        }
    }

    if(found >= 0) {
        TrackerDevice* device = &s_devices[found];
        device->rssi = rssi;
        if(rssi > device->best_rssi) device->best_rssi = rssi;
        device->last_seen_ms = now;
        if(device->name[0] == '\0' && name[0] != '\0') {
            strlcpy(device->name, name, sizeof(device->name));
        }
        device->kind = kind;
        device->addr_type = address_type;
        return;
    }

    if(s_device_count >= TRACKER_MAX_DEVICES) return;
    TrackerDevice* device = &s_devices[s_device_count++];
    memset(device, 0, sizeof(*device));
    memcpy(device->addr, address, sizeof(device->addr));
    device->addr_type = address_type;
    device->kind = kind;
    device->rssi = rssi;
    device->best_rssi = rssi;
    device->last_seen_ms = now;
    if(name) strlcpy(device->name, name, sizeof(device->name));
}

static void tracker_process_report(
    const ble_addr_t* addr,
    int8_t rssi,
    const uint8_t* data,
    uint8_t data_len) {
    if(!addr || !data || data_len == 0) return;
    TrackerKind kind = TrackerKindUnknown;
    char name[24] = "";
    if(!classify_buffer(data, data_len, &kind, name, sizeof(name))) return;

    uint8_t address[6];
    for(size_t i = 0; i < sizeof(address); ++i) address[i] = addr->val[5U - i];
    tracker_store_match(address, addr->type, rssi, kind, name);
}

static int tracker_gap_event_handler(struct ble_gap_event* event, void* context) {
    (void)context;
    if(!event) return 0;
    switch(event->type) {
    case BLE_GAP_EVENT_DISC: {
        if(!s_scanning) break;
        const struct ble_gap_disc_desc* disc = &event->disc;
        tracker_process_report(&disc->addr, disc->rssi, disc->data, disc->length_data);
        break;
    }
#if CONFIG_BT_NIMBLE_EXT_ADV
    case BLE_GAP_EVENT_EXT_DISC: {
        if(!s_scanning || event->ext_disc.data_status == BLE_GAP_EXT_ADV_DATA_STATUS_INCOMPLETE) break;
        const struct ble_gap_ext_disc_desc* disc = &event->ext_disc;
        tracker_process_report(&disc->addr, disc->rssi, disc->data, disc->length_data);
        break;
    }
#endif
    case BLE_GAP_EVENT_DISC_COMPLETE:
        s_scanning = false;
        break;
    default:
        break;
    }
    return 0;
}
#else
static void tracker_gap_event_handler(
    esp_gap_ble_cb_event_t event,
    esp_ble_gap_cb_param_t* param) {
    switch(event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        if(param->scan_param_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            esp_ble_gap_start_scanning(0); // indefinite
        }
        break;

    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        s_scanning = (param->scan_start_cmpl.status == ESP_BT_STATUS_SUCCESS);
        break;

    case ESP_GAP_BLE_SCAN_RESULT_EVT:
        if(param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
            TrackerKind kind = TrackerKindUnknown;
            char name[24] = "";
            bool match = classify_advert(
                param->scan_rst.ble_adv,
                param->scan_rst.adv_data_len,
                param->scan_rst.scan_rsp_len,
                &kind,
                name,
                sizeof(name));
            if(!match) break;

            uint32_t now = furi_get_tick();

            // Dedup by MAC
            int found = -1;
            for(int i = 0; i < s_device_count; i++) {
                if(memcmp(s_devices[i].addr, param->scan_rst.bda, 6) == 0) {
                    found = i;
                    break;
                }
            }
            if(found >= 0) {
                TrackerDevice* d = &s_devices[found];
                d->rssi = param->scan_rst.rssi;
                if(param->scan_rst.rssi > d->best_rssi) d->best_rssi = param->scan_rst.rssi;
                d->last_seen_ms = now;
                // Upgrade name if PP gave us a model after a generic match
                if(d->name[0] == '\0' && name[0] != '\0') {
                    strncpy(d->name, name, sizeof(d->name) - 1);
                    d->name[sizeof(d->name) - 1] = '\0';
                }
                d->kind = kind;
            } else if(s_device_count < TRACKER_MAX_DEVICES) {
                TrackerDevice* d = &s_devices[s_device_count];
                memcpy(d->addr, param->scan_rst.bda, 6);
                d->addr_type = param->scan_rst.ble_addr_type;
                d->kind = kind;
                d->rssi = param->scan_rst.rssi;
                d->best_rssi = param->scan_rst.rssi;
                d->last_seen_ms = now;
                strncpy(d->name, name, sizeof(d->name) - 1);
                d->name[sizeof(d->name) - 1] = '\0';
                s_device_count++;
            }
        }
        break;

    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        s_scanning = false;
        break;

    default:
        break;
    }
}
#endif

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool ble_tracker_hal_start_scan(void) {
    s_device_count = 0;
    memset(s_devices, 0, sizeof(s_devices));

#if CONFIG_BT_NIMBLE_ENABLED
    s_host_owned = !ble_core_stack_is_up();
    if(s_host_owned) {
        esp_err_t start_err = ble_core_stack_up();
        if(start_err != ESP_OK) {
            ESP_LOGE(TAG, "NimBLE host start: %s", esp_err_to_name(start_err));
            s_host_owned = false;
            return false;
        }
    }

    uint8_t own_addr_type;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if(rc != 0) {
        ESP_LOGE(TAG, "cannot infer local address type: %d", rc);
        if(s_host_owned) (void)ble_core_stack_down();
        s_host_owned = false;
        return false;
    }

    struct ble_gap_disc_params scan_params = {
        .passive = 0,
        .limited = 0,
        .filter_duplicates = 0,
        .itvl = 0x50,
        .window = 0x50,
    };
    rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &scan_params, tracker_gap_event_handler, NULL);
    if(rc != 0) {
        ESP_LOGE(TAG, "NimBLE scan start: %d", rc);
        if(s_host_owned) (void)ble_core_stack_down();
        s_host_owned = false;
        return false;
    }
    s_scanning = true;
    return true;
#else
    // Take over GAP callback from whoever had it (e.g. ble_walk_hal)
    esp_err_t err = esp_ble_gap_register_callback(tracker_gap_event_handler);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "gap register: %s", esp_err_to_name(err));
        return false;
    }

    esp_ble_scan_params_t scan_params = {
        .scan_type = BLE_SCAN_TYPE_PASSIVE,
        .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval = 0x50, // 50ms
        .scan_window = 0x30,   // 30ms
    };

    err = esp_ble_gap_set_scan_params(&scan_params);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "set_scan_params: %s", esp_err_to_name(err));
        return false;
    }
    return true;
#endif
}

void ble_tracker_hal_stop_scan(void) {
#if CONFIG_BT_NIMBLE_ENABLED
    if(s_scanning) {
        int rc = ble_gap_disc_cancel();
        if(rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGW(TAG, "NimBLE scan cancel: %d", rc);
        }
        for(int i = 0; i < 40 && s_scanning; ++i) furi_delay_ms(5);
    }
    if(s_host_owned) {
        (void)ble_core_stack_down();
        s_host_owned = false;
    }
#else
    if(s_scanning) {
        esp_ble_gap_stop_scanning();
        for(int i = 0; i < 20 && s_scanning; i++) {
            furi_delay_ms(5);
        }
    }
#endif
}

TrackerDevice* ble_tracker_hal_get_devices(uint16_t* count) {
    *count = s_device_count;
    return s_devices;
}

const char* ble_tracker_kind_label(TrackerKind k) {
    switch(k) {
    case TrackerKindAirTag: return "AirTag";
    case TrackerKindSmartTag: return "SmartTag";
    case TrackerKindTile: return "Tile";
    case TrackerKindApplePP: return "Apple";
    case TrackerKindAppleNearby: return "iPhone/Watch";
    default: return "?";
    }
}
