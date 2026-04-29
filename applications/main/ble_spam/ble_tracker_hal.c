#include "ble_tracker_hal.h"

#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_gap_ble_api.h>
#include <esp_log.h>
#include <furi.h>
#include <string.h>

#define TAG "BleTracker"

static TrackerDevice s_devices[TRACKER_MAX_DEVICES];
static uint16_t s_device_count = 0;
static volatile bool s_scanning = false;

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

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool ble_tracker_hal_start_scan(void) {
    s_device_count = 0;
    memset(s_devices, 0, sizeof(s_devices));

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
}

void ble_tracker_hal_stop_scan(void) {
    if(s_scanning) {
        esp_ble_gap_stop_scanning();
        for(int i = 0; i < 20 && s_scanning; i++) {
            furi_delay_ms(5);
        }
    }
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
