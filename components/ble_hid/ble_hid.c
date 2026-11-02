#include "ble_hid.h"

#include <inttypes.h>
#include <string.h>
#include <stdlib.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <esp_bt.h>
#include <esp_bt_defs.h>
#include <esp_bt_main.h>
#include <esp_err.h>
#include <esp_gap_ble_api.h>
#include <esp_gatts_api.h>
#include <esp_hid_common.h>
#include <esp_hidd.h>
#include <esp_hidd_gatts.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <ble_core.h>
#include "ble_hid_report_map.h"

#define TAG "ble_hid"

#define BLE_HID_SERVICE_UUID       0x1812
#define BLE_HID_APP_ID_BATTERY     0x180F
#define BLE_HID_APP_ID_DEVICE_INFO 0x180A
#define BLE_HID_APP_ID_HID         0x1812
#define BLE_SERIAL_APP_ID          0x0055
#define BLE_HID_ADV_TYPE_FLAGS     0x01
#define BLE_HID_ADV_TYPE_UUID16    0x03
#define BLE_HID_ADV_TYPE_NAME_SHORT 0x08
#define BLE_HID_ADV_TYPE_NAME_FULL  0x09

typedef struct {
    uint8_t modifiers;
    uint8_t reserved;
    uint8_t keys[BLE_HID_KEYBOARD_KEYS_MAX];
} __attribute__((packed)) BleHidKeyboardReport;

typedef struct {
    uint8_t buttons;
    int8_t x;
    int8_t y;
    int8_t wheel;
} __attribute__((packed)) BleHidMouseReport;

typedef struct {
    uint16_t key;
} __attribute__((packed)) BleHidConsumerReport;

struct BleHid {
    esp_hidd_dev_t* dev;
    BleHidConfig config;
    SemaphoreHandle_t mutex;
    BleHidStateCallback state_callback;
    void* state_context;
    BleHidKeyboardReport keyboard_report;
    BleHidMouseReport mouse_report;
    BleHidConsumerReport consumer_report;
    uint8_t led_state;
    bool connected;
};

typedef struct {
    bool initialized;
    bool gap_registered;
    bool gatts_registered;
    bool hidd_started;
    esp_gatt_if_t hid_if[3];
    uint8_t registration_pending_mask;
    uint8_t unregister_pending_mask;
    bool shutting_down;
    bool adv_data_pending;
    bool rand_addr_pending;
    bool rand_addr_enabled;
    bool advertising_requested;
    bool advertising;
    SemaphoreHandle_t mutex;
    SemaphoreHandle_t callbacks_drained;
    uint32_t callback_refs;
    BleHid* active;
    esp_ble_adv_params_t adv_params;
} BleHidGlobalState;

static BleHidGlobalState ble_hid_state = {
    .mutex = NULL,
    .hid_if = {ESP_GATT_IF_NONE, ESP_GATT_IF_NONE, ESP_GATT_IF_NONE},
    .adv_params =
        {
            .adv_int_min = 0x20,
            .adv_int_max = 0x30,
            .adv_type = ADV_TYPE_IND,
            .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
            .channel_map = ADV_CHNL_ALL,
            .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
        },
};

/* Must be called with ble_hid_state.mutex held. */
static void ble_hid_maybe_finish_teardown_locked(void) {
    if(ble_hid_state.shutting_down && !ble_hid_state.active &&
       !ble_hid_state.registration_pending_mask && !ble_hid_state.unregister_pending_mask) {
        ble_hid_state.shutting_down = false;
    }
}

static esp_hid_raw_report_map_t ble_report_maps[] = {
    {
        .data = ble_hid_report_map,
        .len = sizeof(ble_hid_report_map),
    },
};

static esp_hid_device_config_t ble_device_config = {
    .vendor_id = 0x16C0,
    .product_id = 0x05DF,
    .version = 0x0100,
    .device_name = NULL,
    .manufacturer_name = "Flipper Devices",
    .serial_number = "badusb",
    .report_maps = ble_report_maps,
    .report_maps_len = 1,
};

static void ble_hid_lock_global(void) {
    if(ble_hid_state.mutex) {
        xSemaphoreTake(ble_hid_state.mutex, portMAX_DELAY);
    }
}

static void ble_hid_unlock_global(void) {
    if(ble_hid_state.mutex) {
        xSemaphoreGive(ble_hid_state.mutex);
    }
}

static BleHid* ble_hid_callback_acquire(void) {
    ble_hid_lock_global();
    BleHid* ble_hid = ble_hid_state.active;
    if(ble_hid) {
        ble_hid_state.callback_refs++;
    }
    ble_hid_unlock_global();
    return ble_hid;
}

static void ble_hid_callback_release(BleHid* ble_hid) {
    if(!ble_hid) return;

    ble_hid_lock_global();
    if(ble_hid_state.callback_refs == 0) {
        ble_hid_unlock_global();
        ESP_LOGE(TAG, "Callback reference underflow");
        return;
    }
    ble_hid_state.callback_refs--;
    bool callbacks_drained = ble_hid_state.callback_refs == 0;
    if(callbacks_drained && ble_hid_state.callbacks_drained) {
        xSemaphoreGive(ble_hid_state.callbacks_drained);
    }
    ble_hid_unlock_global();
}

static void ble_hid_lock(BleHid* ble_hid) {
    if(ble_hid && ble_hid->mutex) {
        xSemaphoreTake(ble_hid->mutex, portMAX_DELAY);
    }
}

static void ble_hid_unlock(BleHid* ble_hid) {
    if(ble_hid && ble_hid->mutex) {
        xSemaphoreGive(ble_hid->mutex);
    }
}

static BleCorePairing ble_hid_core_pairing(BleHidPairingMode pairing) {
    /* No keyboard on the Flipper: InputOnly cannot type a passkey, show one instead. */
    return pairing == BleHidPairingModeVerifyYesNo ? BleCorePairingVerifyYesNo :
                                                     BleCorePairingDisplayOnly;
}

static void ble_hid_make_random_static_addr(uint8_t mac[6]) {
    mac[0] &= 0xFE;
    mac[0] |= 0xC0;
}

void ble_hid_get_default_mac(uint8_t mac[6]) {
    if(esp_read_mac(mac, ESP_MAC_BT) != ESP_OK) {
        memset(mac, 0, 6);
        return;
    }

    esp_derive_local_mac(mac, mac);
    ble_hid_make_random_static_addr(mac);
}

static bool ble_hid_try_start_advertising_locked(void) {
    if(!ble_hid_state.active) {
        ESP_LOGI(TAG, "adv_try skipped: no active profile");
        return false;
    }

    if(!ble_hid_state.hidd_started || !ble_hid_state.advertising_requested ||
       ble_hid_state.adv_data_pending || ble_hid_state.rand_addr_pending ||
       ble_hid_state.advertising) {
        ESP_LOGI(
            TAG,
            "adv_try blocked: hidd_started=%d requested=%d adv_pending=%d rand_pending=%d advertising=%d",
            ble_hid_state.hidd_started,
            ble_hid_state.advertising_requested,
            ble_hid_state.adv_data_pending,
            ble_hid_state.rand_addr_pending,
            ble_hid_state.advertising);
        return false;
    }

    ble_hid_state.adv_params.own_addr_type =
        ble_hid_state.rand_addr_enabled ? BLE_ADDR_TYPE_RANDOM : BLE_ADDR_TYPE_PUBLIC;

    esp_err_t err = esp_ble_gap_start_advertising(&ble_hid_state.adv_params);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ble_gap_start_advertising failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(
        TAG,
        "adv_start request own_addr_type=%d name=%s",
        ble_hid_state.adv_params.own_addr_type,
        ble_hid_state.active->config.device_name);
    ble_hid_state.advertising = true;
    return true;
}

static void ble_hid_update_connection(BleHid* ble_hid, bool connected) {
    if(!ble_hid) {
        return;
    }

    ble_hid_lock(ble_hid);
    ble_hid->connected = connected;
    BleHidStateCallback callback = ble_hid->state_callback;
    void* context = ble_hid->state_context;
    ble_hid_unlock(ble_hid);

    if(callback) {
        callback(connected, context);
    }
}

static void ble_hid_gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
    /* SMP events (security request, numeric comparison, passkey, auth result) are
     * handled once for all profiles in ble_core. Done before taking our lock so the
     * UI callbacks never run under it. */
    if(ble_core_handle_gap_event(event, param)) return;

    ble_hid_lock_global();

    switch(event) {
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
        ESP_LOGI(
            TAG,
            "gap event: ADV_DATA_RAW_SET_COMPLETE status=%d",
            param->adv_data_raw_cmpl.status);
        ble_hid_state.adv_data_pending = false;
        ble_hid_try_start_advertising_locked();
        break;
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        ESP_LOGI(TAG, "gap event: ADV_DATA_SET_COMPLETE");
        ble_hid_state.adv_data_pending = false;
        ble_hid_try_start_advertising_locked();
        break;
    case ESP_GAP_BLE_SET_STATIC_RAND_ADDR_EVT:
        ESP_LOGI(TAG, "gap event: SET_STATIC_RAND_ADDR");
        ble_hid_state.rand_addr_pending = false;
        ble_hid_try_start_advertising_locked();
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        ESP_LOGI(TAG, "gap event: ADV_START_COMPLETE status=%d", param->adv_start_cmpl.status);
        if(param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "advertising start failed: status=%d", param->adv_start_cmpl.status);
            ble_hid_state.advertising = false;
        }
        break;
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        ESP_LOGI(TAG, "gap event: ADV_STOP_COMPLETE");
        ble_hid_state.advertising = false;
        break;
    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        if(param->ble_security.auth_cmpl.success) {
            ESP_LOGI(TAG, "BLE authentication complete");
        } else {
            ESP_LOGW(
                TAG,
                "BLE authentication failed: 0x%x",
                param->ble_security.auth_cmpl.fail_reason);
        }
        break;
    default:
        break;
    }

    ble_hid_unlock_global();
}

static void ble_hid_device_event_handler(
    void* handler_arg,
    esp_event_base_t base,
    int32_t id,
    void* event_data) {
    (void)handler_arg;
    (void)base;
    esp_hidd_event_t event = (esp_hidd_event_t)id;
    esp_hidd_event_data_t* data = event_data;

    BleHid* ble_hid = ble_hid_callback_acquire();

    if(!ble_hid) {
        return;
    }

    switch(event) {
    case ESP_HIDD_START_EVENT:
        ESP_LOGI(TAG, "hidd event: START");
        ble_hid_lock_global();
        ble_hid_state.hidd_started = true;
        ble_hid_try_start_advertising_locked();
        ble_hid_unlock_global();
        break;
    case ESP_HIDD_CONNECT_EVENT:
        ESP_LOGI(TAG, "hidd event: CONNECT");
        ble_hid_lock_global();
        ble_hid_state.advertising = false;
        ble_hid_unlock_global();
        ble_hid_update_connection(ble_hid, true);
        break;
    case ESP_HIDD_DISCONNECT_EVENT:
        ESP_LOGI(TAG, "hidd event: DISCONNECT");
        ble_hid_update_connection(ble_hid, false);
        ble_hid_lock_global();
        ble_hid_try_start_advertising_locked();
        ble_hid_unlock_global();
        break;
    case ESP_HIDD_OUTPUT_EVENT:
        if(data && data->output.length > 0 && data->output.data) {
            ble_hid_lock(ble_hid);
            ble_hid->led_state = data->output.data[0];
            ble_hid_unlock(ble_hid);
        }
        break;
    default:
        break;
    }

    ble_hid_callback_release(ble_hid);
}

/* The serial profile replaces these global handlers on start. Route only events
 * for HID app interfaces to esp_hidd; link events still update the shared BLE core. */
static void ble_hid_gatts_event_handler(
    esp_gatts_cb_event_t event,
    esp_gatt_if_t gatts_if,
    esp_ble_gatts_cb_param_t* param) {
    int hid_app_index = -1;
    bool is_hid = false;

    if(param && (event == ESP_GATTS_REG_EVT)) {
        switch(param->reg.app_id) {
        case BLE_HID_APP_ID_BATTERY:
            hid_app_index = 0;
            break;
        case BLE_HID_APP_ID_DEVICE_INFO:
            hid_app_index = 1;
            break;
        case BLE_HID_APP_ID_HID:
            hid_app_index = 2;
            break;
        default:
            break;
        }
        is_hid = (hid_app_index >= 0);
        if(is_hid) {
            bool reject_late_registration;
            ble_hid_lock_global();
            ble_hid_state.registration_pending_mask &= (uint8_t)~(1U << hid_app_index);
            reject_late_registration =
                (param->reg.status == ESP_GATT_OK) && (gatts_if != ESP_GATT_IF_NONE) &&
                (ble_hid_state.shutting_down || !ble_hid_state.active);
            if(param->reg.status == ESP_GATT_OK && (gatts_if != ESP_GATT_IF_NONE)) {
                ble_hid_state.hid_if[hid_app_index] = gatts_if;
                if(reject_late_registration) {
                    ble_hid_state.unregister_pending_mask |= (uint8_t)(1U << hid_app_index);
                }
            }
            ble_hid_maybe_finish_teardown_locked();
            ble_hid_unlock_global();
            if(reject_late_registration) {
                ESP_LOGW(
                    TAG,
                    "gatts REG arrived during teardown app_id=0x%04x if=%u; unregistering",
                    (unsigned)param->reg.app_id,
                    (unsigned)gatts_if);
                esp_ble_gatts_app_unregister(gatts_if);
                return;
            }
        } else if((param->reg.app_id == BLE_SERIAL_APP_ID) &&
                  (param->reg.status == ESP_GATT_OK) &&
                  (gatts_if != ESP_GATT_IF_NONE)) {
            ESP_LOGW(
                TAG,
                "gatts REG serial app arrived at HID callback; requesting unregister if=%u",
                (unsigned)gatts_if);
            esp_ble_gatts_app_unregister(gatts_if);
            return;
        } else if(!is_hid && (gatts_if != ESP_GATT_IF_NONE)) {
            ble_hid_lock_global();
            for(size_t i = 0; i < sizeof(ble_hid_state.hid_if) / sizeof(ble_hid_state.hid_if[0]);
                i++) {
                if(ble_hid_state.hid_if[i] == gatts_if) {
                    ble_hid_state.hid_if[i] = ESP_GATT_IF_NONE;
                }
            }
            ble_hid_unlock_global();
        }
    } else if(gatts_if != ESP_GATT_IF_NONE) {
        ble_hid_lock_global();
        for(size_t i = 0; i < sizeof(ble_hid_state.hid_if) / sizeof(ble_hid_state.hid_if[0]); i++) {
            if(ble_hid_state.hid_if[i] == gatts_if) {
                is_hid = true;
                break;
            }
        }
        ble_hid_maybe_finish_teardown_locked();
        ble_hid_unlock_global();
    }

    if(param) {
        switch(event) {
        case ESP_GATTS_REG_EVT:
            ESP_LOGI(
                TAG,
                "gatts REG %s app_id=0x%04x if=%u status=%d",
                is_hid ? "hid" : "other",
                (unsigned)param->reg.app_id,
                (unsigned)gatts_if,
                param->reg.status);
            break;
        case ESP_GATTS_CREAT_ATTR_TAB_EVT:
            ESP_LOGI(
                TAG,
                "gatts CREAT_ATTR_TAB %s if=%u status=%d svc_uuid_len=%u inst=%u handles=%u",
                is_hid ? "hid" : "other",
                (unsigned)gatts_if,
                param->add_attr_tab.status,
                (unsigned)param->add_attr_tab.svc_uuid.len,
                (unsigned)param->add_attr_tab.svc_inst_id,
                (unsigned)param->add_attr_tab.num_handle);
            break;
        case ESP_GATTS_START_EVT:
            ESP_LOGI(
                TAG,
                "gatts START %s if=%u status=%d handle=%u",
                is_hid ? "hid" : "other",
                (unsigned)gatts_if,
                param->start.status,
                (unsigned)param->start.service_handle);
            break;
        case ESP_GATTS_CREATE_EVT:
            ESP_LOGI(
                TAG,
                "gatts CREATE %s if=%u status=%d handle=%u uuid_len=%u",
                is_hid ? "hid" : "other",
                (unsigned)gatts_if,
                param->create.status,
                (unsigned)param->create.service_handle,
                (unsigned)param->create.service_id.id.uuid.len);
            break;
        case ESP_GATTS_ADD_CHAR_EVT:
            ESP_LOGI(
                TAG,
                "gatts ADD_CHAR %s if=%u status=%d attr=%u service=%u uuid_len=%u",
                is_hid ? "hid" : "other",
                (unsigned)gatts_if,
                param->add_char.status,
                (unsigned)param->add_char.attr_handle,
                (unsigned)param->add_char.service_handle,
                (unsigned)param->add_char.char_uuid.len);
            break;
        case ESP_GATTS_STOP_EVT:
            ESP_LOGI(
                TAG,
                "gatts STOP %s if=%u status=%d",
                is_hid ? "hid" : "other",
                (unsigned)gatts_if,
                param->stop.status);
            break;
        case ESP_GATTS_DELETE_EVT:
            ESP_LOGI(
                TAG,
                "gatts DELETE %s if=%u status=%d",
                is_hid ? "hid" : "other",
                (unsigned)gatts_if,
                param->del.status);
            break;
        case ESP_GATTS_UNREG_EVT:
            ESP_LOGI(
                TAG,
                "gatts UNREG %s if=%u",
                is_hid ? "hid" : "other",
                (unsigned)gatts_if);
            break;
        default:
            break;
        }
    }

    if(param && (event == ESP_GATTS_CONNECT_EVT)) {
        ble_core_link_connected(param->connect.remote_bda, &param->connect.conn_params);
    } else if(param && (event == ESP_GATTS_DISCONNECT_EVT)) {
        ble_core_link_disconnected();
    }

    if(is_hid && param) {
        esp_hidd_gatts_event_handler(event, gatts_if, param);
    }

    if(param && (event == ESP_GATTS_UNREG_EVT)) {
        ble_hid_lock_global();
        for(size_t i = 0; i < sizeof(ble_hid_state.hid_if) / sizeof(ble_hid_state.hid_if[0]); i++) {
            if(ble_hid_state.hid_if[i] == gatts_if) {
                ble_hid_state.hid_if[i] = ESP_GATT_IF_NONE;
                ble_hid_state.unregister_pending_mask &= (uint8_t)~(1U << i);
            }
        }
        ble_hid_maybe_finish_teardown_locked();
        ble_hid_unlock_global();
    }
}

static esp_err_t ble_hid_stack_init_once(void) {
    esp_err_t err = ESP_OK;

    if(!ble_hid_state.mutex) {
        ble_hid_state.mutex = xSemaphoreCreateMutex();
        if(!ble_hid_state.mutex) {
            return ESP_ERR_NO_MEM;
        }
    }
    if(!ble_hid_state.callbacks_drained) {
        ble_hid_state.callbacks_drained = xSemaphoreCreateBinary();
        if(!ble_hid_state.callbacks_drained) {
            return ESP_ERR_NO_MEM;
        }
    }

    /* Status based (not a private flag): the stack may have been stopped for
     * WiFi / light sleep since the last HID session. */
    err = ble_core_stack_up();
    if(err != ESP_OK) {
        return err;
    }

    err = ble_core_register_gap_callback(ble_hid_gap_event_handler);
    if(err != ESP_OK) {
        return err;
    }

    err = esp_ble_gatts_register_callback(ble_hid_gatts_event_handler);
    if(err != ESP_OK) {
        return err;
    }

    ble_hid_lock_global();
    ble_hid_state.gap_registered = true;
    ble_hid_state.gatts_registered = true;
    ble_hid_state.initialized = true;
    ble_hid_unlock_global();

    return ESP_OK;
}

static esp_err_t ble_hid_apply_security_config(const BleHidConfig* config) {
    /* Ordinary HID profiles keep their existing passkey/numeric-comparison
     * behavior. BadUSB opts into the separate Just Works override below. */
    const BleCoreConfig core_config = {
        .pairing = ble_hid_core_pairing(config->pairing),
        .bonding = config->bonding,
        .require_authenticated = false,
        .link_auth_timeout_ms = 0,
        .fail_limit = 0,
        .fast_link = true,
    };
    esp_err_t err = ble_core_apply_config(&core_config);
    if(err != ESP_OK || !config->just_works) return err;

    /* BadUSB opts into Just Works explicitly. Keep this override local to the
     * BadUSB HID profile; ordinary HID and the firmware serial profile retain
     * their existing MITM/pairing configuration. Apply it before GATT startup
     * and advertising so the central never sees the previous Numeric Comparison
     * parameters for this profile. */
    esp_ble_auth_req_t auth_req =
        config->bonding ? ESP_LE_AUTH_REQ_SC_BOND : ESP_LE_AUTH_REQ_SC_ONLY;
    esp_ble_io_cap_t io_cap = ESP_IO_CAP_NONE;
    err = esp_ble_gap_set_security_param(
        ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(auth_req));
    if(err != ESP_OK) return err;
    return esp_ble_gap_set_security_param(
        ESP_BLE_SM_IOCAP_MODE, &io_cap, sizeof(io_cap));
}

static bool ble_hid_append_adv_field(
    uint8_t* raw_adv,
    size_t* offset,
    uint8_t type,
    const void* payload,
    size_t payload_len) {
    if(!raw_adv || !offset || ((*offset + payload_len + 2) > ESP_BLE_ADV_DATA_LEN_MAX)) {
        return false;
    }

    raw_adv[*offset] = payload_len + 1;
    raw_adv[*offset + 1] = type;
    if(payload_len && payload) {
        memcpy(&raw_adv[*offset + 2], payload, payload_len);
    }
    *offset += payload_len + 2;

    return true;
}

static esp_err_t ble_hid_configure_advertising(const BleHidConfig* config) {
    const uint8_t adv_flags = ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT;
    const uint8_t service_uuid[] = {BLE_HID_SERVICE_UUID & 0xFF, BLE_HID_SERVICE_UUID >> 8};
    const size_t device_name_len = strlen(config->device_name);
    uint8_t raw_adv[ESP_BLE_ADV_DATA_LEN_MAX] = {0};
    size_t raw_adv_len = 0;
    size_t advertised_name_len = device_name_len;
    uint8_t name_type = BLE_HID_ADV_TYPE_NAME_FULL;
    esp_err_t err = ESP_OK;

    if(!ble_hid_append_adv_field(&raw_adv[0], &raw_adv_len, BLE_HID_ADV_TYPE_FLAGS, &adv_flags, 1)) {
        return ESP_ERR_INVALID_SIZE;
    }

    if(!ble_hid_append_adv_field(
           &raw_adv[0], &raw_adv_len, BLE_HID_ADV_TYPE_UUID16, service_uuid, sizeof(service_uuid))) {
        return ESP_ERR_INVALID_SIZE;
    }

    while(advertised_name_len > 0 &&
          !ble_hid_append_adv_field(
              &raw_adv[0],
              &raw_adv_len,
              (advertised_name_len == device_name_len) ? BLE_HID_ADV_TYPE_NAME_FULL :
                                                          BLE_HID_ADV_TYPE_NAME_SHORT,
              config->device_name,
              advertised_name_len)) {
        advertised_name_len--;
        name_type = BLE_HID_ADV_TYPE_NAME_SHORT;
    }

    if(advertised_name_len == 0) {
        ESP_LOGW(TAG, "adv_config omitted device name: no space in raw ADV");
    }

    ESP_LOGI(
        TAG,
        "adv_config name_len=%u adv_name_len=%u uuid=0x%04x name_type=0x%02x raw_len=%u",
        (unsigned)device_name_len,
        (unsigned)advertised_name_len,
        BLE_HID_SERVICE_UUID,
        name_type,
        (unsigned)raw_adv_len);

    err = esp_ble_gap_set_device_name(config->device_name);
    if(err != ESP_OK) {
        return err;
    }

    ble_hid_lock_global();
    ble_hid_state.adv_data_pending = true;
    ble_hid_unlock_global();

    err = esp_ble_gap_config_adv_data_raw(raw_adv, raw_adv_len);
    if(err != ESP_OK) {
        ble_hid_lock_global();
        ble_hid_state.adv_data_pending = false;
        ble_hid_unlock_global();
    }

    return err;
}

static esp_err_t ble_hid_set_random_address(const BleHidConfig* config) {
    uint8_t mac[6];
    memcpy(mac, config->mac, sizeof(mac));
    ble_hid_make_random_static_addr(mac);

    ble_hid_lock_global();
    ble_hid_state.rand_addr_pending = true;
    ble_hid_state.rand_addr_enabled = true;
    ble_hid_unlock_global();

    return esp_ble_gap_set_rand_addr(mac);
}

static bool ble_hid_has_custom_mac(const BleHidConfig* config) {
    static const uint8_t zero_mac[6] = {0};
    return memcmp(config->mac, zero_mac, sizeof(zero_mac)) != 0;
}

static bool ble_hid_send_report(BleHid* ble_hid, uint8_t report_id, void* data, size_t length) {
    if(!ble_hid || !ble_hid->dev) {
        return false;
    }

    if(!esp_hidd_dev_connected(ble_hid->dev)) {
        return false;
    }

    return esp_hidd_dev_input_set(ble_hid->dev, 0, report_id, data, length) == ESP_OK;
}

BleHid* ble_hid_alloc(const BleHidConfig* config) {
    if(!config) {
        return NULL;
    }

    esp_err_t err = ble_hid_stack_init_once();
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "BLE stack init failed: %s", esp_err_to_name(err));
        return NULL;
    }

    BleHid* ble_hid = calloc(1, sizeof(BleHid));
    if(!ble_hid) {
        return NULL;
    }

    ble_hid->mutex = xSemaphoreCreateMutex();
    if(!ble_hid->mutex) {
        free(ble_hid);
        return NULL;
    }

    memcpy(&ble_hid->config, config, sizeof(ble_hid->config));
    ESP_LOGI(
        TAG,
        "alloc name=%s mac=%02x:%02x:%02x:%02x:%02x:%02x bonding=%d pairing=%d just_works=%d",
        ble_hid->config.device_name,
        ble_hid->config.mac[0],
        ble_hid->config.mac[1],
        ble_hid->config.mac[2],
        ble_hid->config.mac[3],
        ble_hid->config.mac[4],
        ble_hid->config.mac[5],
        ble_hid->config.bonding,
        ble_hid->config.pairing,
        ble_hid->config.just_works);

    ble_device_config.device_name = ble_hid->config.device_name;

    ble_hid_lock_global();
    if(ble_hid_state.active || ble_hid_state.shutting_down ||
       ble_hid_state.registration_pending_mask || ble_hid_state.unregister_pending_mask) {
        ESP_LOGE(
            TAG,
            "alloc rejected: previous HID GATTS lifecycle is not clean (reg=0x%02x unreg=0x%02x shutdown=%d)",
            ble_hid_state.registration_pending_mask,
            ble_hid_state.unregister_pending_mask,
            ble_hid_state.shutting_down);
        ble_hid_unlock_global();
        vSemaphoreDelete(ble_hid->mutex);
        free(ble_hid);
        return NULL;
    }
    ble_hid_state.active = ble_hid;
    ble_hid_state.shutting_down = false;
    ble_hid_state.registration_pending_mask = 0;
    ble_hid_state.unregister_pending_mask = 0;
    for(size_t i = 0; i < sizeof(ble_hid_state.hid_if) / sizeof(ble_hid_state.hid_if[0]); i++) {
        ble_hid_state.hid_if[i] = ESP_GATT_IF_NONE;
    }
    ble_hid_state.advertising = false;
    ble_hid_state.advertising_requested = false;
    ble_hid_state.adv_data_pending = false;
    ble_hid_state.hidd_started = false;
    ble_hid_state.rand_addr_pending = false;
    ble_hid_state.rand_addr_enabled = false;
    ble_hid_unlock_global();

    err = ble_hid_apply_security_config(&ble_hid->config);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "BLE security config failed: %s", esp_err_to_name(err));
        goto error;
    }

    if(ble_hid_has_custom_mac(&ble_hid->config)) {
        err = ble_hid_set_random_address(&ble_hid->config);
        if(err != ESP_OK) {
            ESP_LOGE(TAG, "BLE random address setup failed: %s", esp_err_to_name(err));
            goto error;
        }
    }

    err = ble_hid_configure_advertising(&ble_hid->config);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "BLE advertising config failed: %s", esp_err_to_name(err));
        goto error;
    }

    ble_hid_lock_global();
    ble_hid_state.registration_pending_mask = 0x07;
    ble_hid_unlock_global();
    err = esp_hidd_dev_init(
        &ble_device_config, ESP_HID_TRANSPORT_BLE, ble_hid_device_event_handler, &ble_hid->dev);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "BLE HID init failed: %s", esp_err_to_name(err));
        goto error;
    }

    return ble_hid;

error:
    ble_hid_lock_global();
    if(ble_hid_state.active == ble_hid) {
        ble_hid_state.active = NULL;
    }
    ble_hid_state.shutting_down = ble_hid_state.registration_pending_mask ||
                                  ble_hid_state.unregister_pending_mask;
    ble_hid_maybe_finish_teardown_locked();
    ble_hid_unlock_global();

    if(ble_hid->mutex) {
        vSemaphoreDelete(ble_hid->mutex);
    }
    free(ble_hid);
    return NULL;
}

bool ble_hid_free(BleHid* ble_hid) {
    if(!ble_hid) {
        return true;
    }

    ble_hid_lock_global();
    if(ble_hid_state.advertising) {
        esp_ble_gap_stop_advertising();
        ble_hid_state.advertising = false;
    }
    ble_hid_state.advertising_requested = false;
    ble_hid_state.hidd_started = false;
    ble_hid_state.shutting_down = true;
    for(size_t i = 0; i < sizeof(ble_hid_state.hid_if) / sizeof(ble_hid_state.hid_if[0]); i++) {
        if(ble_hid_state.hid_if[i] != ESP_GATT_IF_NONE) {
            ble_hid_state.unregister_pending_mask |= (uint8_t)(1U << i);
        }
    }
    if(ble_hid_state.active == ble_hid) {
        ble_hid_state.active = NULL;
    }
    uint32_t callback_refs = ble_hid_state.callback_refs;
    ble_hid_unlock_global();

    while(callback_refs > 0) {
        if(xSemaphoreTake(ble_hid_state.callbacks_drained, portMAX_DELAY) != pdTRUE) {
            ESP_LOGE(TAG, "Failed waiting for BLE HID callbacks to drain");
            return false;
        }
        ble_hid_lock_global();
        callback_refs = ble_hid_state.callback_refs;
        ble_hid_unlock_global();
    }

    bool teardown_clean = true;
    if(ble_hid->dev) {
        esp_err_t err = esp_hidd_dev_deinit(ble_hid->dev);
        if(err != ESP_OK) {
            ESP_LOGE(TAG, "HID deinit failed: %s", esp_err_to_name(err));
            teardown_clean = false;
        }

        /* esp_hidd_dev_deinit() only queues GATTS stop/delete/unregister calls.
         * Do not let the BT service install the next profile callback until all
         * HID app interfaces have actually emitted UNREG. Late REG events are
         * rejected and unregistered by ble_hid_gatts_event_handler(). */
        const TickType_t unregister_timeout = pdMS_TO_TICKS(3000);
        TickType_t unregister_started = xTaskGetTickCount();
        while(true) {
            ble_hid_lock_global();
            uint8_t unregister_pending = ble_hid_state.unregister_pending_mask;
            uint8_t registration_pending = ble_hid_state.registration_pending_mask;
            ble_hid_unlock_global();
            uint8_t pending = unregister_pending | registration_pending;
            if(pending == 0) break;

            TickType_t elapsed = xTaskGetTickCount() - unregister_started;
            if(elapsed >= unregister_timeout) {
                ESP_LOGE(
                    TAG,
                    "Timed out waiting for HID GATTS teardown reg=0x%02x unreg=0x%02x",
                    registration_pending,
                    unregister_pending);
                teardown_clean = false;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    ble_hid_lock_global();
    uint8_t registration_pending = ble_hid_state.registration_pending_mask;
    uint8_t unregister_pending = ble_hid_state.unregister_pending_mask;
    if(registration_pending || unregister_pending) teardown_clean = false;
    /* Keep the tombstone and interface map when teardown is incomplete. A late
     * REG/UNREG callback can still drain it, and a new HID allocation is refused
     * until then. */
    if(teardown_clean) ble_hid_state.shutting_down = false;
    ble_hid_unlock_global();

    if(ble_hid->mutex) {
        vSemaphoreDelete(ble_hid->mutex);
    }

    free(ble_hid);
    return teardown_clean;
}

void ble_hid_set_state_callback(BleHid* ble_hid, BleHidStateCallback callback, void* context) {
    if(!ble_hid) {
        return;
    }

    ble_hid_lock(ble_hid);
    ble_hid->state_callback = callback;
    ble_hid->state_context = context;
    bool connected = ble_hid->connected;
    ble_hid_unlock(ble_hid);

    if(callback) {
        callback(connected, context);
    }
}

bool ble_hid_is_connected(BleHid* ble_hid) {
    if(!ble_hid) {
        return false;
    }

    ble_hid_lock(ble_hid);
    bool connected = ble_hid->connected;
    ble_hid_unlock(ble_hid);
    return connected;
}

bool ble_hid_kb_press(BleHid* ble_hid, uint16_t button) {
    if(!ble_hid) {
        return false;
    }

    ble_hid_lock(ble_hid);
    for(size_t i = 0; i < BLE_HID_KEYBOARD_KEYS_MAX; i++) {
        if(ble_hid->keyboard_report.keys[i] == (button & 0xFF)) {
            break;
        }
        if(ble_hid->keyboard_report.keys[i] == 0) {
            ble_hid->keyboard_report.keys[i] = button & 0xFF;
            break;
        }
    }
    ble_hid->keyboard_report.modifiers |= (button >> 8);
    bool result = ble_hid_send_report(
        ble_hid,
        BLE_HID_REPORT_ID_KEYBOARD,
        &ble_hid->keyboard_report,
        sizeof(ble_hid->keyboard_report));
    ble_hid_unlock(ble_hid);
    return result;
}

bool ble_hid_kb_release(BleHid* ble_hid, uint16_t button) {
    if(!ble_hid) {
        return false;
    }

    ble_hid_lock(ble_hid);
    for(size_t i = 0; i < BLE_HID_KEYBOARD_KEYS_MAX; i++) {
        if(ble_hid->keyboard_report.keys[i] == (button & 0xFF)) {
            ble_hid->keyboard_report.keys[i] = 0;
            break;
        }
    }
    ble_hid->keyboard_report.modifiers &= ~(button >> 8);
    bool result = ble_hid_send_report(
        ble_hid,
        BLE_HID_REPORT_ID_KEYBOARD,
        &ble_hid->keyboard_report,
        sizeof(ble_hid->keyboard_report));
    ble_hid_unlock(ble_hid);
    return result;
}

bool ble_hid_kb_release_all(BleHid* ble_hid) {
    if(!ble_hid) {
        return false;
    }

    ble_hid_lock(ble_hid);
    memset(&ble_hid->keyboard_report, 0, sizeof(ble_hid->keyboard_report));
    bool result = ble_hid_send_report(
        ble_hid,
        BLE_HID_REPORT_ID_KEYBOARD,
        &ble_hid->keyboard_report,
        sizeof(ble_hid->keyboard_report));
    ble_hid_unlock(ble_hid);
    return result;
}

bool ble_hid_mouse_move(BleHid* ble_hid, int8_t dx, int8_t dy) {
    if(!ble_hid) {
        return false;
    }

    ble_hid_lock(ble_hid);
    ble_hid->mouse_report.x = dx;
    ble_hid->mouse_report.y = dy;
    bool result = ble_hid_send_report(
        ble_hid, BLE_HID_REPORT_ID_MOUSE, &ble_hid->mouse_report, sizeof(ble_hid->mouse_report));
    ble_hid->mouse_report.x = 0;
    ble_hid->mouse_report.y = 0;
    ble_hid_unlock(ble_hid);
    return result;
}

bool ble_hid_mouse_press(BleHid* ble_hid, uint8_t button) {
    if(!ble_hid) {
        return false;
    }

    ble_hid_lock(ble_hid);
    ble_hid->mouse_report.buttons |= button;
    bool result = ble_hid_send_report(
        ble_hid, BLE_HID_REPORT_ID_MOUSE, &ble_hid->mouse_report, sizeof(ble_hid->mouse_report));
    ble_hid_unlock(ble_hid);
    return result;
}

bool ble_hid_mouse_release(BleHid* ble_hid, uint8_t button) {
    if(!ble_hid) {
        return false;
    }

    ble_hid_lock(ble_hid);
    ble_hid->mouse_report.buttons &= ~button;
    bool result = ble_hid_send_report(
        ble_hid, BLE_HID_REPORT_ID_MOUSE, &ble_hid->mouse_report, sizeof(ble_hid->mouse_report));
    ble_hid_unlock(ble_hid);
    return result;
}

bool ble_hid_mouse_release_all(BleHid* ble_hid) {
    if(!ble_hid) {
        return false;
    }

    ble_hid_lock(ble_hid);
    ble_hid->mouse_report.buttons = 0;
    ble_hid->mouse_report.x = 0;
    ble_hid->mouse_report.y = 0;
    ble_hid->mouse_report.wheel = 0;
    bool result = ble_hid_send_report(
        ble_hid, BLE_HID_REPORT_ID_MOUSE, &ble_hid->mouse_report, sizeof(ble_hid->mouse_report));
    ble_hid_unlock(ble_hid);
    return result;
}

bool ble_hid_mouse_scroll(BleHid* ble_hid, int8_t delta) {
    if(!ble_hid) {
        return false;
    }

    ble_hid_lock(ble_hid);
    ble_hid->mouse_report.wheel = delta;
    bool result = ble_hid_send_report(
        ble_hid, BLE_HID_REPORT_ID_MOUSE, &ble_hid->mouse_report, sizeof(ble_hid->mouse_report));
    ble_hid->mouse_report.wheel = 0;
    ble_hid_unlock(ble_hid);
    return result;
}

bool ble_hid_consumer_press(BleHid* ble_hid, uint16_t button) {
    if(!ble_hid) {
        return false;
    }

    ble_hid_lock(ble_hid);
    ble_hid->consumer_report.key = button;
    bool result = ble_hid_send_report(
        ble_hid,
        BLE_HID_REPORT_ID_CONSUMER,
        &ble_hid->consumer_report,
        sizeof(ble_hid->consumer_report));
    ble_hid_unlock(ble_hid);
    return result;
}

bool ble_hid_consumer_release(BleHid* ble_hid, uint16_t button) {
    if(!ble_hid) {
        return false;
    }

    ble_hid_lock(ble_hid);
    if(ble_hid->consumer_report.key == button) {
        ble_hid->consumer_report.key = 0;
    }
    bool result = ble_hid_send_report(
        ble_hid,
        BLE_HID_REPORT_ID_CONSUMER,
        &ble_hid->consumer_report,
        sizeof(ble_hid->consumer_report));
    ble_hid_unlock(ble_hid);
    return result;
}

bool ble_hid_consumer_release_all(BleHid* ble_hid) {
    if(!ble_hid) {
        return false;
    }

    ble_hid_lock(ble_hid);
    ble_hid->consumer_report.key = 0;
    bool result = ble_hid_send_report(
        ble_hid,
        BLE_HID_REPORT_ID_CONSUMER,
        &ble_hid->consumer_report,
        sizeof(ble_hid->consumer_report));
    ble_hid_unlock(ble_hid);
    return result;
}

uint8_t ble_hid_get_led_state(BleHid* ble_hid) {
    if(!ble_hid) {
        return 0;
    }

    ble_hid_lock(ble_hid);
    uint8_t led_state = ble_hid->led_state;
    ble_hid_unlock(ble_hid);
    return led_state;
}

bool ble_hid_start_advertising(void) {
    bool accepted = false;

    ble_hid_lock_global();
    if(ble_hid_state.active) {
        ESP_LOGI(TAG, "start_advertising requested");
        ble_hid_state.advertising_requested = true;
        ble_hid_try_start_advertising_locked();
        accepted = true;
    }
    ble_hid_unlock_global();

    return accepted;
}

void ble_hid_stop_advertising(void) {
    ble_hid_lock_global();
    ESP_LOGI(TAG, "stop_advertising requested");
    ble_hid_state.advertising_requested = false;
    if(ble_hid_state.advertising) {
        esp_ble_gap_stop_advertising();
        ble_hid_state.advertising = false;
    }
    ble_hid_unlock_global();
}

bool ble_hid_is_advertising(void) {
    bool advertising = false;

    ble_hid_lock_global();
    advertising = ble_hid_state.advertising_requested && ble_hid_state.active != NULL &&
                  !ble_hid_state.active->connected;
    ble_hid_unlock_global();

    return advertising;
}

bool ble_hid_is_active(void) {
    bool active = false;

    ble_hid_lock_global();
    active = ble_hid_state.active != NULL;
    ble_hid_unlock_global();

    return active;
}

bool ble_hid_is_teardown_pending(void) {
    bool pending = false;
    ble_hid_lock_global();
    pending = ble_hid_state.shutting_down || ble_hid_state.registration_pending_mask ||
              ble_hid_state.unregister_pending_mask;
    ble_hid_unlock_global();
    return pending;
}

void ble_hid_reset_gatts_state(void) {
    if(!ble_hid_state.mutex) return;

    ble_hid_lock_global();
    /* This is called only after Bluedroid/controller deinit. No GATTS callbacks
     * from the previous stack instance can still own these interface IDs. */
    ble_hid_state.active = NULL;
    ble_hid_state.registration_pending_mask = 0;
    ble_hid_state.unregister_pending_mask = 0;
    ble_hid_state.shutting_down = false;
    ble_hid_state.advertising = false;
    ble_hid_state.advertising_requested = false;
    ble_hid_state.adv_data_pending = false;
    ble_hid_state.hidd_started = false;
    ble_hid_state.rand_addr_pending = false;
    ble_hid_state.rand_addr_enabled = false;
    ble_hid_state.callback_refs = 0;
    for(size_t i = 0; i < sizeof(ble_hid_state.hid_if) / sizeof(ble_hid_state.hid_if[0]); i++) {
        ble_hid_state.hid_if[i] = ESP_GATT_IF_NONE;
    }
    ble_hid_unlock_global();
}

bool ble_hid_remove_pairing(void) {
    /* Bonds are shared by every profile. The old guard on this module's own
     * "initialized" flag made "forget devices" a silent no-op whenever only the
     * serial profile had brought the stack up. */
    return ble_core_remove_all_bonds();
}
