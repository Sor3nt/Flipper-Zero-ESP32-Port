#include "ble_hid.h"

#include <inttypes.h>
#include <string.h>
#include <stdlib.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

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
#include <nvs_flash.h>

#define TAG "ble_hid"

#define BLE_HID_REPORT_ID_KEYBOARD 1
#define BLE_HID_REPORT_ID_MOUSE    2
#define BLE_HID_REPORT_ID_CONSUMER 3
#define BLE_HID_SERVICE_UUID       0x1812
#define BLE_HID_ADV_TYPE_FLAGS     0x01
#define BLE_HID_ADV_TYPE_UUID16    0x03
#define BLE_HID_ADV_TYPE_NAME_SHORT 0x08
#define BLE_HID_ADV_TYPE_NAME_FULL  0x09

#define BLE_HID_KEYBOARD_KEYS_MAX 6

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
    bool adv_data_pending;
    bool rand_addr_pending;
    bool rand_addr_enabled;
    bool advertising_requested;
    bool advertising;
    SemaphoreHandle_t mutex;
    BleHid* active;
    esp_ble_adv_params_t adv_params;
} BleHidGlobalState;

static BleHidGlobalState ble_hid_state = {
    .mutex = NULL,
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

static const uint8_t ble_hid_report_map[] = {
    0x05, 0x01,
    0x09, 0x06,
    0xA1, 0x01,
    0x85, BLE_HID_REPORT_ID_KEYBOARD,
    0x05, 0x07,
    0x19, 0xE0,
    0x29, 0xE7,
    0x15, 0x00,
    0x25, 0x01,
    0x75, 0x01,
    0x95, 0x08,
    0x81, 0x02,
    0x95, 0x01,
    0x75, 0x08,
    0x81, 0x01,
    0x05, 0x08,
    0x95, 0x08,
    0x75, 0x01,
    0x19, 0x01,
    0x29, 0x08,
    0x91, 0x02,
    0x95, BLE_HID_KEYBOARD_KEYS_MAX,
    0x75, 0x08,
    0x15, 0x00,
    0x25, 0x65,
    0x05, 0x07,
    0x19, 0x00,
    0x29, 0x65,
    0x81, 0x00,
    0xC0,
    0x05, 0x01,
    0x09, 0x02,
    0xA1, 0x01,
    0x09, 0x01,
    0xA1, 0x00,
    0x85, BLE_HID_REPORT_ID_MOUSE,
    0x05, 0x09,
    0x19, 0x01,
    0x29, 0x03,
    0x15, 0x00,
    0x25, 0x01,
    0x95, 0x03,
    0x75, 0x01,
    0x81, 0x02,
    0x95, 0x01,
    0x75, 0x05,
    0x81, 0x03,
    0x05, 0x01,
    0x09, 0x30,
    0x09, 0x31,
    0x09, 0x38,
    0x15, 0x81,
    0x25, 0x7F,
    0x75, 0x08,
    0x95, 0x03,
    0x81, 0x06,
    0xC0,
    0xC0,
    0x05, 0x0C,
    0x09, 0x01,
    0xA1, 0x01,
    0x85, BLE_HID_REPORT_ID_CONSUMER,
    0x15, 0x00,
    0x26, 0xFF, 0x03,
    0x19, 0x00,
    0x2A, 0xFF, 0x03,
    0x95, 0x01,
    0x75, 0x10,
    0x81, 0x00,
    0xC0,
};

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

static esp_ble_auth_req_t ble_hid_auth_req(bool bonding) {
    return bonding ? ESP_LE_AUTH_REQ_SC_MITM_BOND : ESP_LE_AUTH_REQ_SC_MITM;
}

static esp_ble_io_cap_t ble_hid_io_cap(BleHidPairingMode pairing) {
    switch(pairing) {
    case BleHidPairingModeDisplayOnly:
    case BleHidPairingModeInputOnly:
        return ESP_IO_CAP_OUT;
    case BleHidPairingModeVerifyYesNo:
    default:
        return ESP_IO_CAP_IO;
    }
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
    case ESP_GAP_BLE_SEC_REQ_EVT:
        ESP_LOGI(TAG, "gap event: SEC_REQ");
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        break;
    case ESP_GAP_BLE_NC_REQ_EVT:
        ESP_LOGI(TAG, "gap event: NC_REQ");
        esp_ble_confirm_reply(param->ble_security.key_notif.bd_addr, true);
        break;
    case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
        ESP_LOGI(TAG, "BLE passkey: %06" PRIu32, param->ble_security.key_notif.passkey);
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

    ble_hid_lock_global();
    BleHid* ble_hid = ble_hid_state.active;
    ble_hid_unlock_global();

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
}

static esp_err_t ble_hid_stack_init_once(void) {
    esp_err_t err = ESP_OK;

    if(!ble_hid_state.mutex) {
        ble_hid_state.mutex = xSemaphoreCreateMutex();
        if(!ble_hid_state.mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    ble_hid_lock_global();
    if(ble_hid_state.initialized) {
        ble_hid_unlock_global();
        return ESP_OK;
    }

    ble_hid_unlock_global();

    err = nvs_flash_init();
    if((err == ESP_ERR_NVS_NO_FREE_PAGES) || (err == ESP_ERR_NVS_NEW_VERSION_FOUND)) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if(err != ESP_OK) {
        return err;
    }

    err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        return err;
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    err = esp_bt_controller_init(&bt_cfg);
    if((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        return err;
    }

    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        return err;
    }

    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    err = esp_bluedroid_init_with_cfg(&bluedroid_cfg);
    if((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        return err;
    }

    err = esp_bluedroid_enable();
    if((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        return err;
    }

    ble_hid_lock_global();

    if(!ble_hid_state.gap_registered) {
        err = esp_ble_gap_register_callback(ble_hid_gap_event_handler);
        if(err != ESP_OK) {
            ble_hid_unlock_global();
            return err;
        }
        ble_hid_state.gap_registered = true;
    }

    if(!ble_hid_state.gatts_registered) {
        err = esp_ble_gatts_register_callback(esp_hidd_gatts_event_handler);
        if(err != ESP_OK) {
            ble_hid_unlock_global();
            return err;
        }
        ble_hid_state.gatts_registered = true;
    }

    ble_hid_state.initialized = true;
    ble_hid_unlock_global();

    return ESP_OK;
}

static esp_err_t ble_hid_apply_security_config(const BleHidConfig* config) {
    esp_ble_auth_req_t auth_req = ble_hid_auth_req(config->bonding);
    esp_ble_io_cap_t io_cap = ble_hid_io_cap(config->pairing);
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t key_size = 16;
    uint32_t passkey = BLE_HID_PASSKEY_DEFAULT;

    esp_err_t err = esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, 1);
    if(err != ESP_OK) {
        return err;
    }

    err = esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &io_cap, 1);
    if(err != ESP_OK) {
        return err;
    }

    err = esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, 1);
    if(err != ESP_OK) {
        return err;
    }

    err = esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, 1);
    if(err != ESP_OK) {
        return err;
    }

    err = esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, 1);
    if(err != ESP_OK) {
        return err;
    }

    return esp_ble_gap_set_security_param(
        ESP_BLE_SM_SET_STATIC_PASSKEY, &passkey, sizeof(passkey));
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
        "alloc name=%s mac=%02x:%02x:%02x:%02x:%02x:%02x bonding=%d pairing=%d",
        ble_hid->config.device_name,
        ble_hid->config.mac[0],
        ble_hid->config.mac[1],
        ble_hid->config.mac[2],
        ble_hid->config.mac[3],
        ble_hid->config.mac[4],
        ble_hid->config.mac[5],
        ble_hid->config.bonding,
        ble_hid->config.pairing);

    ble_device_config.device_name = ble_hid->config.device_name;

    ble_hid_lock_global();
    if(ble_hid_state.active) {
        ble_hid_unlock_global();
        vSemaphoreDelete(ble_hid->mutex);
        free(ble_hid);
        return NULL;
    }
    ble_hid_state.active = ble_hid;
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
    ble_hid_unlock_global();

    if(ble_hid->mutex) {
        vSemaphoreDelete(ble_hid->mutex);
    }
    free(ble_hid);
    return NULL;
}

void ble_hid_free(BleHid* ble_hid) {
    if(!ble_hid) {
        return;
    }

    ble_hid_lock_global();
    if(ble_hid_state.advertising) {
        esp_ble_gap_stop_advertising();
        ble_hid_state.advertising = false;
    }
    ble_hid_state.advertising_requested = false;
    ble_hid_state.hidd_started = false;
    if(ble_hid_state.active == ble_hid) {
        ble_hid_state.active = NULL;
    }
    ble_hid_unlock_global();

    if(ble_hid->dev) {
        esp_hidd_dev_deinit(ble_hid->dev);
    }

    if(ble_hid->mutex) {
        vSemaphoreDelete(ble_hid->mutex);
    }

    free(ble_hid);
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

bool ble_hid_remove_pairing(void) {
    if(!ble_hid_state.initialized) {
        return false;
    }

    int device_count = esp_ble_get_bond_device_num();
    if(device_count <= 0) {
        return true;
    }

    esp_ble_bond_dev_t* devices = calloc(device_count, sizeof(esp_ble_bond_dev_t));
    if(!devices) {
        return false;
    }

    int requested = device_count;
    esp_err_t err = esp_ble_get_bond_device_list(&requested, devices);
    if(err != ESP_OK) {
        free(devices);
        return false;
    }

    bool success = true;
    for(int i = 0; i < requested; i++) {
        err = esp_ble_remove_bond_device(devices[i].bd_addr);
        if(err != ESP_OK) {
            success = false;
        }
    }

    free(devices);
    return success;
}
