/**
 * @file ble_serial.c
 * BLE Serial Service — ESP32 implementation
 *
 * Uses ESP-IDF Bluedroid GATTS API to create a custom GATT service
 * with the same UUIDs as the STM32 Flipper Zero serial service.
 */

#include "ble_serial.h"

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
#include <esp_gatt_common_api.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <nvs_flash.h>

#include <furi_ble/gap.h>

/* Defined in furi_hal_bt.c — bridges BLE events to the BT service */
extern void furi_hal_bt_emit_gap_event(GapEvent event);

#define TAG "ble_serial"

#define BLE_SERIAL_APP_ID         0x55
#define BLE_SERIAL_ADV_TYPE_FLAGS 0x01
#define BLE_SERIAL_ADV_TYPE_UUID128 0x07
#define BLE_SERIAL_ADV_TYPE_NAME_FULL 0x09
#define BLE_SERIAL_ADV_TYPE_NAME_SHORT 0x08

/* ---- UUIDs (little-endian byte order for BLE) ---- */

/* Service: 8fe5b3d5-2e7f-4a98-2a48-7acc60fe0000 */
static const uint8_t serial_svc_uuid[16] = {
    0x00, 0x00, 0xFE, 0x60, 0xCC, 0x7A, 0x48, 0x2A,
    0x98, 0x4A, 0x7F, 0x2E, 0xD5, 0xB3, 0xE5, 0x8F
};

/* TX (Flipper→Client): 19ed82ae-ed21-4c9d-4145-228e61fe0000 */
static const uint8_t serial_tx_uuid[16] = {
    0x00, 0x00, 0xFE, 0x61, 0x8E, 0x22, 0x45, 0x41,
    0x9D, 0x4C, 0x21, 0xED, 0xAE, 0x82, 0xED, 0x19
};

/* RX (Client→Flipper): 19ed82ae-ed21-4c9d-4145-228e62fe0000 */
static const uint8_t serial_rx_uuid[16] = {
    0x00, 0x00, 0xFE, 0x62, 0x8E, 0x22, 0x45, 0x41,
    0x9D, 0x4C, 0x21, 0xED, 0xAE, 0x82, 0xED, 0x19
};

/* Flow Control: 19ed82ae-ed21-4c9d-4145-228e63fe0000 */
static const uint8_t serial_flow_uuid[16] = {
    0x00, 0x00, 0xFE, 0x63, 0x8E, 0x22, 0x45, 0x41,
    0x9D, 0x4C, 0x21, 0xED, 0xAE, 0x82, 0xED, 0x19
};

/* RPC Status: 19ed82ae-ed21-4c9d-4145-228e64fe0000 */
static const uint8_t serial_rpc_uuid[16] = {
    0x00, 0x00, 0xFE, 0x64, 0x8E, 0x22, 0x45, 0x41,
    0x9D, 0x4C, 0x21, 0xED, 0xAE, 0x82, 0xED, 0x19
};

/* Standard 16-bit UUID for CCCD */
static const uint8_t cccd_uuid[2] = {0x02, 0x29};  /* 0x2902 */

/* ---- Attribute table indices (Serial Service) ---- */
enum {
    IDX_SVC,

    IDX_TX_CHAR,
    IDX_TX_VAL,
    IDX_TX_CCCD,

    IDX_RX_CHAR,
    IDX_RX_VAL,

    IDX_FLOW_CHAR,
    IDX_FLOW_VAL,
    IDX_FLOW_CCCD,

    IDX_RPC_CHAR,
    IDX_RPC_VAL,
    IDX_RPC_CCCD,

    IDX_NB,
};

/* ---- Advertising parameters ---- */
static esp_ble_adv_params_t serial_adv_params = {
    .adv_int_min = 0x20,
    .adv_int_max = 0x40,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

/* ---- Instance ---- */

struct BleSerial {
    SemaphoreHandle_t mutex;
    BleSerialConfig config;
    bool connected;
    uint16_t conn_id;
    esp_gatt_if_t gatts_if;

    uint16_t handle_table[IDX_NB];

    SerialServiceEventCallback event_callback;
    void* event_context;
    uint16_t buff_size;
    uint16_t bytes_ready;

    BleSerialStateCallback state_callback;
    void* state_context;
};

/* ---- Global state ---- */
static struct {
    SemaphoreHandle_t mutex;
    SemaphoreHandle_t ready_sem;  /* signaled when GATT service is started */
    bool initialized;
    bool gap_registered;
    bool gatts_registered;
    bool advertising;
    bool advertising_requested;
    bool adv_data_pending;
    bool rand_addr_pending;
    bool rand_addr_enabled;
    bool app_registered;
    bool service_started;
    BleSerial* active;
} serial_state = {0};

/* ---- Helpers ---- */

static void serial_lock(BleSerial* s) {
    xSemaphoreTake(s->mutex, portMAX_DELAY);
}
static void serial_unlock(BleSerial* s) {
    xSemaphoreGive(s->mutex);
}
static void serial_lock_global(void) {
    xSemaphoreTake(serial_state.mutex, portMAX_DELAY);
}
static void serial_unlock_global(void) {
    xSemaphoreGive(serial_state.mutex);
}

static void serial_update_connection(BleSerial* s, bool connected) {
    BleSerialStateCallback cb = NULL;
    void* ctx = NULL;

    serial_lock(s);
    s->connected = connected;
    cb = s->state_callback;
    ctx = s->state_context;
    serial_unlock(s);

    if(cb) cb(connected, ctx);
}

static esp_ble_auth_req_t serial_auth_req(bool bonding) {
    /* Use legacy pairing (no Secure Connections) with MITM + bond.
     * SC forces Numeric Comparison when phone IO is DisplayYesNo,
     * but the Flipper app expects Passkey Entry (PIN display → user enters on phone).
     * Legacy pairing with DisplayOnly IO → Passkey Entry. */
    return bonding ? ESP_LE_AUTH_REQ_BOND_MITM : ESP_LE_AUTH_REQ_MITM;
}

static esp_ble_io_cap_t serial_io_cap(BleSerialPairingMode mode) {
    switch(mode) {
    case BleSerialPairingDisplayOnly:
        return ESP_IO_CAP_OUT;
    case BleSerialPairingInputOnly:
        return ESP_IO_CAP_IN;
    case BleSerialPairingPinCodeVerifyYesNo:
    default:
        return ESP_IO_CAP_IO;
    }
}

static void serial_make_random_static_addr(uint8_t mac[6]) {
    mac[5] |= 0xC0;  /* Set top 2 bits for random static address */
}

static void serial_try_start_advertising_locked(void) {
    ESP_LOGI(TAG, "try_start_adv: requested=%d adv_pending=%d rand_pending=%d advertising=%d active=%p connected=%d svc_started=%d",
        serial_state.advertising_requested,
        serial_state.adv_data_pending,
        serial_state.rand_addr_pending,
        serial_state.advertising,
        (void*)serial_state.active,
        serial_state.active ? serial_state.active->connected : -1,
        serial_state.service_started);

    if(!serial_state.advertising_requested) { ESP_LOGW(TAG, "  blocked: not requested"); return; }
    if(serial_state.adv_data_pending) { ESP_LOGW(TAG, "  blocked: adv_data_pending"); return; }
    if(serial_state.rand_addr_pending) { ESP_LOGW(TAG, "  blocked: rand_addr_pending"); return; }
    if(serial_state.advertising) { ESP_LOGW(TAG, "  blocked: already advertising"); return; }
    if(!serial_state.active) { ESP_LOGW(TAG, "  blocked: no active instance"); return; }
    if(serial_state.active->connected) { ESP_LOGW(TAG, "  blocked: connected"); return; }

    ESP_LOGI(TAG, "  -> calling esp_ble_gap_start_advertising");
    esp_err_t err = esp_ble_gap_start_advertising(&serial_adv_params);
    if(err == ESP_OK) {
        serial_state.advertising = true;
        ESP_LOGI(TAG, "  -> advertising started OK");
    } else {
        ESP_LOGE(TAG, "  -> start_advertising failed: %s", esp_err_to_name(err));
    }
}

/* ---- GATT attribute table ---- */

static const uint16_t primary_service_uuid = ESP_GATT_UUID_PRI_SERVICE;
static const uint16_t char_declaration_uuid = ESP_GATT_UUID_CHAR_DECLARE;
static const uint8_t char_prop_read_indicate = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_INDICATE;
static const uint8_t char_prop_read_write_wwr = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
static const uint8_t char_prop_read_notify = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY;
static const uint8_t char_prop_read_write_notify = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_NOTIFY;
static uint16_t cccd_value = 0x0000;

static const esp_gatts_attr_db_t serial_gatt_db[IDX_NB] = {
    /* Service Declaration */
    [IDX_SVC] = {
        {ESP_GATT_AUTO_RSP},
        {
            ESP_UUID_LEN_16, (uint8_t*)&primary_service_uuid,
            ESP_GATT_PERM_READ,
            sizeof(serial_svc_uuid), sizeof(serial_svc_uuid), (uint8_t*)serial_svc_uuid
        }
    },

    /* TX Characteristic Declaration */
    [IDX_TX_CHAR] = {
        {ESP_GATT_AUTO_RSP},
        {
            ESP_UUID_LEN_16, (uint8_t*)&char_declaration_uuid,
            ESP_GATT_PERM_READ,
            1, 1, (uint8_t*)&char_prop_read_indicate
        }
    },
    /* TX Characteristic Value — encrypted read triggers client-initiated pairing */
    [IDX_TX_VAL] = {
        {ESP_GATT_RSP_BY_APP},
        {
            ESP_UUID_LEN_128, (uint8_t*)serial_tx_uuid,
            ESP_GATT_PERM_READ,
            BLE_SVC_SERIAL_DATA_LEN_MAX, 0, NULL
        }
    },
    /* TX CCCD */
    [IDX_TX_CCCD] = {
        {ESP_GATT_AUTO_RSP},
        {
            ESP_UUID_LEN_16, (uint8_t*)cccd_uuid,
            ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
            sizeof(uint16_t), sizeof(uint16_t), (uint8_t*)&cccd_value
        }
    },

    /* RX Characteristic Declaration */
    [IDX_RX_CHAR] = {
        {ESP_GATT_AUTO_RSP},
        {
            ESP_UUID_LEN_16, (uint8_t*)&char_declaration_uuid,
            ESP_GATT_PERM_READ,
            1, 1, (uint8_t*)&char_prop_read_write_wwr
        }
    },
    /* RX Characteristic Value */
    [IDX_RX_VAL] = {
        {ESP_GATT_RSP_BY_APP},
        {
            ESP_UUID_LEN_128, (uint8_t*)serial_rx_uuid,
            ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
            BLE_SVC_SERIAL_DATA_LEN_MAX, 0, NULL
        }
    },

    /* Flow Control Characteristic Declaration */
    [IDX_FLOW_CHAR] = {
        {ESP_GATT_AUTO_RSP},
        {
            ESP_UUID_LEN_16, (uint8_t*)&char_declaration_uuid,
            ESP_GATT_PERM_READ,
            1, 1, (uint8_t*)&char_prop_read_notify
        }
    },
    /* Flow Control Characteristic Value */
    [IDX_FLOW_VAL] = {
        {ESP_GATT_AUTO_RSP},
        {
            ESP_UUID_LEN_128, (uint8_t*)serial_flow_uuid,
            ESP_GATT_PERM_READ,
            sizeof(uint32_t), sizeof(uint32_t), (uint8_t*)&(uint32_t){0}
        }
    },
    /* Flow Control CCCD */
    [IDX_FLOW_CCCD] = {
        {ESP_GATT_AUTO_RSP},
        {
            ESP_UUID_LEN_16, (uint8_t*)cccd_uuid,
            ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
            sizeof(uint16_t), sizeof(uint16_t), (uint8_t*)&cccd_value
        }
    },

    /* RPC Status Characteristic Declaration */
    [IDX_RPC_CHAR] = {
        {ESP_GATT_AUTO_RSP},
        {
            ESP_UUID_LEN_16, (uint8_t*)&char_declaration_uuid,
            ESP_GATT_PERM_READ,
            1, 1, (uint8_t*)&char_prop_read_write_notify
        }
    },
    /* RPC Status Characteristic Value */
    [IDX_RPC_VAL] = {
        {ESP_GATT_AUTO_RSP},
        {
            ESP_UUID_LEN_128, (uint8_t*)serial_rpc_uuid,
            ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
            sizeof(uint32_t), sizeof(uint32_t), (uint8_t*)&(uint32_t){0}
        }
    },
    /* RPC Status CCCD */
    [IDX_RPC_CCCD] = {
        {ESP_GATT_AUTO_RSP},
        {
            ESP_UUID_LEN_16, (uint8_t*)cccd_uuid,
            ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
            sizeof(uint16_t), sizeof(uint16_t), (uint8_t*)&cccd_value
        }
    },
};

/* ---- Device Information Service (0x180A) ----
 * Required by the Flipper Android App's FirstPairBleManager. */
static const uint16_t dis_svc_uuid = 0x180A;
static const uint16_t dis_manufacturer_uuid = 0x2A29;
static const uint16_t dis_hw_revision_uuid = 0x2A27;
static const uint16_t dis_sw_revision_uuid = 0x2A28;

/* Custom Flipper API Version characteristic: 03f6666d-ae5e-47c8-8e1a-5d873eb5a933 */
static const uint8_t dis_api_version_uuid[16] = {
    0x33, 0xa9, 0xb5, 0x3e, 0x87, 0x5d, 0x1a, 0x8e,
    0xc8, 0x47, 0x5e, 0xae, 0x6d, 0x66, 0xf6, 0x03
};

static const char dis_manufacturer[] = "Flipper Devices Inc.";
static const char dis_hw_revision[] = "ESP32-C6 1.0";
static const char dis_sw_revision[] = "0.1.0";
/* API version format: "major.minor" — matches STM32 firmware API */
static const char dis_api_version[] = "0.3";

enum {
    DIS_IDX_SVC,
    DIS_IDX_MANUFACTURER_CHAR, DIS_IDX_MANUFACTURER_VAL,
    DIS_IDX_HW_CHAR, DIS_IDX_HW_VAL,
    DIS_IDX_SW_CHAR, DIS_IDX_SW_VAL,
    DIS_IDX_API_CHAR, DIS_IDX_API_VAL,
    DIS_IDX_NB,
};

static const uint8_t dis_char_prop_read = ESP_GATT_CHAR_PROP_BIT_READ;

static const esp_gatts_attr_db_t dis_gatt_db[DIS_IDX_NB] = {
    [DIS_IDX_SVC] = {{ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t*)&primary_service_uuid, ESP_GATT_PERM_READ,
         sizeof(uint16_t), sizeof(uint16_t), (uint8_t*)&dis_svc_uuid}},
    [DIS_IDX_MANUFACTURER_CHAR] = {{ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t*)&char_declaration_uuid, ESP_GATT_PERM_READ,
         1, 1, (uint8_t*)&dis_char_prop_read}},
    [DIS_IDX_MANUFACTURER_VAL] = {{ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t*)&dis_manufacturer_uuid, ESP_GATT_PERM_READ,
         sizeof(dis_manufacturer) - 1, sizeof(dis_manufacturer) - 1, (uint8_t*)dis_manufacturer}},
    [DIS_IDX_HW_CHAR] = {{ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t*)&char_declaration_uuid, ESP_GATT_PERM_READ,
         1, 1, (uint8_t*)&dis_char_prop_read}},
    [DIS_IDX_HW_VAL] = {{ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t*)&dis_hw_revision_uuid, ESP_GATT_PERM_READ,
         sizeof(dis_hw_revision) - 1, sizeof(dis_hw_revision) - 1, (uint8_t*)dis_hw_revision}},
    [DIS_IDX_SW_CHAR] = {{ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t*)&char_declaration_uuid, ESP_GATT_PERM_READ,
         1, 1, (uint8_t*)&dis_char_prop_read}},
    [DIS_IDX_SW_VAL] = {{ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t*)&dis_sw_revision_uuid, ESP_GATT_PERM_READ,
         sizeof(dis_sw_revision) - 1, sizeof(dis_sw_revision) - 1, (uint8_t*)dis_sw_revision}},
    /* Custom Flipper API Version */
    [DIS_IDX_API_CHAR] = {{ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t*)&char_declaration_uuid, ESP_GATT_PERM_READ,
         1, 1, (uint8_t*)&dis_char_prop_read}},
    [DIS_IDX_API_VAL] = {{ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_128, (uint8_t*)dis_api_version_uuid, ESP_GATT_PERM_READ,
         sizeof(dis_api_version) - 1, sizeof(dis_api_version) - 1, (uint8_t*)dis_api_version}},
};

static uint16_t dis_handle_table[DIS_IDX_NB];
static bool dis_service_started = false;

/* ---- Battery Service (0x180F) ----
 * Required by the Flipper Android App to show DeviceStatus.Connected.
 * Without battery level, the app stays in NoDeviceInformation state
 * and hides the screen streaming button. */
static const uint16_t bas_svc_uuid = 0x180F;
static const uint16_t bas_battery_level_uuid = 0x2A19;

static uint8_t bas_battery_level = 100; /* Default: 100% (no real battery on ESP32 dev board) */

enum {
    BAS_IDX_SVC,
    BAS_IDX_BATTERY_LEVEL_CHAR,
    BAS_IDX_BATTERY_LEVEL_VAL,
    BAS_IDX_BATTERY_LEVEL_CCCD,
    BAS_IDX_NB,
};

static const uint8_t bas_char_prop_read_notify = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY;

static const esp_gatts_attr_db_t bas_gatt_db[BAS_IDX_NB] = {
    [BAS_IDX_SVC] = {{ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t*)&primary_service_uuid, ESP_GATT_PERM_READ,
         sizeof(uint16_t), sizeof(uint16_t), (uint8_t*)&bas_svc_uuid}},
    [BAS_IDX_BATTERY_LEVEL_CHAR] = {{ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t*)&char_declaration_uuid, ESP_GATT_PERM_READ,
         1, 1, (uint8_t*)&bas_char_prop_read_notify}},
    [BAS_IDX_BATTERY_LEVEL_VAL] = {{ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t*)&bas_battery_level_uuid, ESP_GATT_PERM_READ,
         sizeof(uint8_t), sizeof(uint8_t), &bas_battery_level}},
    [BAS_IDX_BATTERY_LEVEL_CCCD] = {{ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t*)&cccd_uuid, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
         sizeof(uint16_t), 0, NULL}},
};

static uint16_t bas_handle_table[BAS_IDX_NB];
static bool bas_service_started = false;

/* ---- GAP event handler ---- */

static void serial_gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
    switch(event) {
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
        ESP_LOGI(TAG, "gap: adv data set complete");
        serial_lock_global();
        serial_state.adv_data_pending = false;
        serial_try_start_advertising_locked();
        serial_unlock_global();
        break;

    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if(param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "gap: adv start failed: %d", param->adv_start_cmpl.status);
            serial_lock_global();
            serial_state.advertising = false;
            serial_unlock_global();
        } else {
            ESP_LOGI(TAG, "gap: advertising started");
        }
        break;

    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        ESP_LOGI(TAG, "gap: advertising stopped");
        break;

    case ESP_GAP_BLE_SET_LOCAL_PRIVACY_COMPLETE_EVT:
        break;

    case ESP_GAP_BLE_SEC_REQ_EVT:
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        break;

    case ESP_GAP_BLE_NC_REQ_EVT: {
        uint32_t nc_code = param->ble_security.key_notif.passkey;
        ESP_LOGI(TAG, "gap: numeric comparison: %06lu", (unsigned long)nc_code);
        /* Show the NC code on the Flipper display so user can verify */
        GapEvent gap_ev = {
            .type = GapEventTypePinCodeShow,
            .data.pin_code = nc_code,
        };
        furi_hal_bt_emit_gap_event(gap_ev);
        /* Auto-confirm (no physical button to reject) */
        esp_ble_confirm_reply(param->ble_security.key_notif.bd_addr, true);
        break;
    }

    case ESP_GAP_BLE_PASSKEY_NOTIF_EVT: {
        uint32_t passkey = param->ble_security.key_notif.passkey;
        ESP_LOGI(TAG, "gap: passkey notify: %06lu", (unsigned long)passkey);
        /* Show passkey on Flipper display for user to enter on phone */
        GapEvent gap_ev = {
            .type = GapEventTypePinCodeShow,
            .data.pin_code = passkey,
        };
        furi_hal_bt_emit_gap_event(gap_ev);
        break;
    }

    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        ESP_LOGI(TAG, "gap: auth complete addr=%02x:%02x:%02x:%02x:%02x:%02x success=%d",
            param->ble_security.auth_cmpl.bd_addr[0],
            param->ble_security.auth_cmpl.bd_addr[1],
            param->ble_security.auth_cmpl.bd_addr[2],
            param->ble_security.auth_cmpl.bd_addr[3],
            param->ble_security.auth_cmpl.bd_addr[4],
            param->ble_security.auth_cmpl.bd_addr[5],
            param->ble_security.auth_cmpl.success);

        /* After auth completes, re-send RPC Status and Flow Control notifications.
         * The initial notifications sent at connection time are typically lost because
         * the client hasn't subscribed to CCCDs yet. */
        if(param->ble_security.auth_cmpl.success) {
            BleSerial* s = serial_state.active;
            if(s && s->connected) {
                /* Re-send Flow Control if callback was set */
                if(s->handle_table[IDX_FLOW_VAL] && s->event_callback) {
                    uint32_t flow_be = __builtin_bswap32((uint32_t)s->buff_size);
                    esp_ble_gatts_send_indicate(s->gatts_if, s->conn_id,
                        s->handle_table[IDX_FLOW_VAL], sizeof(flow_be), (uint8_t*)&flow_be, false);
                }
                /* Re-send RPC Status (read stored value and re-notify) */
                if(s->handle_table[IDX_RPC_VAL]) {
                    const uint8_t* stored_val = NULL;
                    uint16_t stored_len = 0;
                    if(esp_ble_gatts_get_attr_value(s->handle_table[IDX_RPC_VAL],
                            &stored_len, &stored_val) == ESP_GATT_OK && stored_len > 0) {
                        esp_ble_gatts_send_indicate(s->gatts_if, s->conn_id,
                            s->handle_table[IDX_RPC_VAL], stored_len, (uint8_t*)stored_val, false);
                    }
                }
            }
        }
        break;

    case ESP_GAP_BLE_SET_STATIC_RAND_ADDR_EVT:
        serial_lock_global();
        serial_state.rand_addr_pending = false;
        serial_try_start_advertising_locked();
        serial_unlock_global();
        break;

    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
        ESP_LOGI(TAG, "gap: conn params updated status=%d min=%d max=%d",
            param->update_conn_params.status,
            param->update_conn_params.min_int,
            param->update_conn_params.max_int);
        break;

    default:
        break;
    }
}

/* ---- GATTS event handler ---- */

static void serial_gatts_event_handler(
    esp_gatts_cb_event_t event,
    esp_gatt_if_t gatts_if,
    esp_ble_gatts_cb_param_t* param) {

    BleSerial* serial = serial_state.active;

    switch(event) {
    case ESP_GATTS_REG_EVT:
        if(param->reg.status == ESP_GATT_OK) {
            ESP_LOGI(TAG, "gatts: app registered, gatts_if=%d", gatts_if);
            if(serial) {
                serial->gatts_if = gatts_if;
            }
            serial_lock_global();
            serial_state.app_registered = true;
            serial_unlock_global();
            /* Create attribute table */
            esp_ble_gatts_create_attr_tab(serial_gatt_db, gatts_if, IDX_NB, 0);
        } else {
            ESP_LOGE(TAG, "gatts: app register failed: %d", param->reg.status);
        }
        break;

    case ESP_GATTS_CREAT_ATTR_TAB_EVT:
        if(param->add_attr_tab.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "gatts: create attr table failed: %d", param->add_attr_tab.status);
            break;
        }
        if(param->add_attr_tab.num_handle == IDX_NB) {
            /* Serial service table created */
            ESP_LOGI(TAG, "gatts: serial attr table created, starting service");
            if(serial) {
                memcpy(serial->handle_table, param->add_attr_tab.handles, sizeof(serial->handle_table));
                esp_ble_gatts_start_service(serial->handle_table[IDX_SVC]);
            }
        } else if(param->add_attr_tab.num_handle == DIS_IDX_NB) {
            /* DIS table created */
            ESP_LOGI(TAG, "gatts: DIS attr table created, starting service");
            memcpy(dis_handle_table, param->add_attr_tab.handles, sizeof(dis_handle_table));
            esp_ble_gatts_start_service(dis_handle_table[DIS_IDX_SVC]);
        } else if(param->add_attr_tab.num_handle == BAS_IDX_NB) {
            /* Battery service table created */
            ESP_LOGI(TAG, "gatts: BAS attr table created, starting service");
            memcpy(bas_handle_table, param->add_attr_tab.handles, sizeof(bas_handle_table));
            esp_ble_gatts_start_service(bas_handle_table[BAS_IDX_SVC]);
        } else {
            ESP_LOGE(TAG, "gatts: attr table handle count unexpected: %d", param->add_attr_tab.num_handle);
        }
        break;

    case ESP_GATTS_START_EVT:
        ESP_LOGI(TAG, "gatts: service started, status=%d", param->start.status);
        if(!serial_state.service_started) {
            /* Serial service just started — now create DIS */
            serial_lock_global();
            serial_state.service_started = true;
            serial_unlock_global();
            ESP_LOGI(TAG, "gatts: creating DIS attribute table...");
            esp_ble_gatts_create_attr_tab(dis_gatt_db, serial->gatts_if, DIS_IDX_NB, 1);
        } else if(!dis_service_started) {
            /* DIS just started — now create Battery Service */
            dis_service_started = true;
            ESP_LOGI(TAG, "gatts: creating BAS attribute table...");
            esp_ble_gatts_create_attr_tab(bas_gatt_db, serial->gatts_if, BAS_IDX_NB, 2);
        } else if(!bas_service_started) {
            /* Battery service just started — all services ready */
            bas_service_started = true;
            ESP_LOGI(TAG, "gatts: BAS started, all services ready");
            if(serial_state.ready_sem) {
                xSemaphoreGive(serial_state.ready_sem);
            }
        }
        break;

    case ESP_GATTS_CONNECT_EVT:
        ESP_LOGI(TAG, "gatts: connected, conn_id=%d", param->connect.conn_id);
        serial_lock_global();
        serial_state.advertising = false;
        serial_unlock_global();
        if(serial) {
            serial->conn_id = param->connect.conn_id;
            serial_update_connection(serial, true);
        }
        /* Request encryption — triggers Passkey Entry with legacy pairing */
        esp_ble_set_encryption(param->connect.remote_bda, ESP_BLE_SEC_ENCRYPT_MITM);
        break;

    case ESP_GATTS_DISCONNECT_EVT:
        ESP_LOGI(TAG, "gatts: disconnected, reason=0x%x", param->disconnect.reason);
        if(serial) {
            serial_update_connection(serial, false);
        }
        serial_lock_global();
        serial_try_start_advertising_locked();
        serial_unlock_global();
        break;

    case ESP_GATTS_WRITE_EVT:
        if(!serial) break;
        ESP_LOGI(TAG, "gatts: WRITE handle=%d len=%d need_rsp=%d is_prep=%d",
            param->write.handle, param->write.len,
            param->write.need_rsp, param->write.is_prep);
        if(param->write.handle == serial->handle_table[IDX_RX_VAL]) {
            /* RX data received from client */
            ESP_LOGI(TAG, "  -> RX data %d bytes", param->write.len);
            SerialServiceEventCallback cb = NULL;
            void* ctx = NULL;
            serial_lock(serial);
            cb = serial->event_callback;
            ctx = serial->event_context;
            if(serial->bytes_ready >= param->write.len) {
                serial->bytes_ready -= param->write.len;
            } else {
                serial->bytes_ready = 0;
            }
            serial_unlock(serial);

            uint16_t new_bytes_ready = 0;
            if(cb) {
                SerialServiceEvent ev = {
                    .event = SerialServiceEventTypeDataReceived,
                    .data = {
                        .buffer = param->write.value,
                        .size = param->write.len,
                    },
                };
                new_bytes_ready = cb(ev, ctx);
            }

            /* Update internal flow tracking.
             * Don't send a notification here — STM32 only sends flow
             * notifications via notify_buffer_is_empty callback. */
            if(new_bytes_ready > 0) {
                serial_lock(serial);
                serial->bytes_ready = new_bytes_ready;
                serial_unlock(serial);
            }

            if(!param->write.is_prep) {
                if(param->write.need_rsp) {
                    esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                        param->write.trans_id, ESP_GATT_OK, NULL);
                }
            }
        } else if(param->write.handle == serial->handle_table[IDX_RPC_VAL]) {
            /* RPC status write from client */
            ESP_LOGI(TAG, "  -> RPC Status write, len=%d", param->write.len);
            if(param->write.len >= 4) {
                uint32_t val = 0;
                memcpy(&val, param->write.value, 4);
                ESP_LOGI(TAG, "  -> RPC Status value=0x%08lx", (unsigned long)val);
                if(val == FuriHalBtSerialRpcStatusNotActive) {
                    SerialServiceEventCallback cb = NULL;
                    void* ctx = NULL;
                    serial_lock(serial);
                    cb = serial->event_callback;
                    ctx = serial->event_context;
                    serial_unlock(serial);
                    if(cb) {
                        SerialServiceEvent ev = {
                            .event = SerialServiceEventTypesBleResetRequest,
                        };
                        cb(ev, ctx);
                    }
                }
            }
            if(param->write.need_rsp) {
                esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                    param->write.trans_id, ESP_GATT_OK, NULL);
            }
        } else if(param->write.handle == serial->handle_table[IDX_RPC_CCCD]) {
            /* Client subscribed to RPC Status notifications */
            uint16_t cccd_val = 0;
            if(param->write.len == 2) memcpy(&cccd_val, param->write.value, 2);
            ESP_LOGI(TAG, "  -> RPC Status CCCD write: 0x%04x", cccd_val);
            /* Re-send current RPC Status value when client subscribes */
            if(cccd_val != 0) {
                const uint8_t* stored_val = NULL;
                uint16_t stored_len = 0;
                if(esp_ble_gatts_get_attr_value(serial->handle_table[IDX_RPC_VAL],
                        &stored_len, &stored_val) == ESP_GATT_OK && stored_len > 0) {
                    ESP_LOGI(TAG, "  -> re-sending RPC Status notification (%d bytes)", stored_len);
                    esp_ble_gatts_send_indicate(serial->gatts_if, serial->conn_id,
                        serial->handle_table[IDX_RPC_VAL], stored_len, (uint8_t*)stored_val, false);
                }
            }
        } else if(param->write.handle == serial->handle_table[IDX_FLOW_CCCD]) {
            /* Client subscribed to Flow Control notifications */
            uint16_t cccd_val = 0;
            if(param->write.len == 2) memcpy(&cccd_val, param->write.value, 2);
            ESP_LOGI(TAG, "  -> Flow Control CCCD write: 0x%04x", cccd_val);
            /* Re-send current flow control value when client subscribes */
            if(cccd_val != 0 && serial->event_callback) {
                uint32_t flow_be = __builtin_bswap32((uint32_t)serial->buff_size);
                ESP_LOGI(TAG, "  -> re-sending Flow Control notification (buff=%d)", serial->buff_size);
                esp_ble_gatts_send_indicate(serial->gatts_if, serial->conn_id,
                    serial->handle_table[IDX_FLOW_VAL], sizeof(flow_be), (uint8_t*)&flow_be, false);
            }
        } else if(param->write.handle == serial->handle_table[IDX_TX_CCCD]) {
            uint16_t cccd_val = 0;
            if(param->write.len == 2) memcpy(&cccd_val, param->write.value, 2);
            ESP_LOGI(TAG, "  -> TX CCCD write: 0x%04x", cccd_val);
        } else {
            ESP_LOGI(TAG, "  -> unknown handle %d", param->write.handle);
            if(param->write.need_rsp) {
                esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                    param->write.trans_id, ESP_GATT_OK, NULL);
            }
        }
        break;

    case ESP_GATTS_READ_EVT:
        if(!serial) break;
        ESP_LOGI(TAG, "gatts: READ handle=%d", param->read.handle);
        if(param->read.handle == serial->handle_table[IDX_TX_VAL]) {
            ESP_LOGI(TAG, "  -> TX val read");
            esp_gatt_rsp_t rsp = {0};
            rsp.attr_value.len = 0;
            esp_ble_gatts_send_response(gatts_if, param->read.conn_id,
                param->read.trans_id, ESP_GATT_OK, &rsp);
        } else if(param->read.handle == serial->handle_table[IDX_RX_VAL]) {
            ESP_LOGI(TAG, "  -> RX val read");
            esp_gatt_rsp_t rsp = {0};
            rsp.attr_value.len = 0;
            esp_ble_gatts_send_response(gatts_if, param->read.conn_id,
                param->read.trans_id, ESP_GATT_OK, &rsp);
        } else {
            ESP_LOGI(TAG, "  -> auto-rsp handle %d", param->read.handle);
        }
        break;

    case ESP_GATTS_CONF_EVT:
        /* Only fire DataSent for TX characteristic indications,
         * not for flow control or RPC status notifications */
        if(!serial) break;
        if(param->conf.handle == serial->handle_table[IDX_TX_VAL]) {
            SerialServiceEventCallback cb = NULL;
            void* ctx = NULL;
            serial_lock(serial);
            cb = serial->event_callback;
            ctx = serial->event_context;
            serial_unlock(serial);
            if(cb) {
                SerialServiceEvent ev = {
                    .event = SerialServiceEventTypeDataSent,
                };
                cb(ev, ctx);
            }
        }
        break;

    case ESP_GATTS_MTU_EVT:
        ESP_LOGI(TAG, "gatts: MTU=%d", param->mtu.mtu);
        /* Bridge MTU update to GAP event system */
        if(serial && serial->connected) {
            GapEvent gap_ev = {
                .type = GapEventTypeUpdateMTU,
                .data.max_packet_size = param->mtu.mtu - 3,
            };
            furi_hal_bt_emit_gap_event(gap_ev);
        }
        break;

    case ESP_GATTS_RESPONSE_EVT:
        if(param->rsp.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "gatts: response failed, handle=%d status=%d",
                param->rsp.handle, param->rsp.status);
        }
        break;

    default:
        ESP_LOGD(TAG, "gatts: unhandled event %d", event);
        break;
    }
}

/* ---- BLE stack init (shared with ble_hid, idempotent) ---- */

static esp_err_t serial_stack_init_once(void) {
    esp_err_t err = ESP_OK;

    if(!serial_state.mutex) {
        serial_state.mutex = xSemaphoreCreateMutex();
        if(!serial_state.mutex) return ESP_ERR_NO_MEM;
    }

    serial_lock_global();
    if(serial_state.initialized) {
        serial_unlock_global();
        return ESP_OK;
    }
    serial_unlock_global();

    err = nvs_flash_init();
    if(err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if(err != ESP_OK) return err;

    err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    err = esp_bt_controller_init(&bt_cfg);
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    err = esp_bluedroid_init_with_cfg(&bluedroid_cfg);
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    err = esp_bluedroid_enable();
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    serial_lock_global();
    serial_state.initialized = true;
    serial_unlock_global();

    return ESP_OK;
}

static esp_err_t serial_apply_security_config(const BleSerialConfig* config) {
    esp_ble_auth_req_t auth_req = serial_auth_req(config->bonding);
    esp_ble_io_cap_t io_cap = serial_io_cap(config->pairing);
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t key_size = 16;

    /* Enable verbose BLE logging to debug GATT operations */
    esp_log_level_set("BT_GATT", ESP_LOG_VERBOSE);
    esp_log_level_set("BT_ATT", ESP_LOG_VERBOSE);

    esp_err_t err;
    err = esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, 1);
    if(err != ESP_OK) return err;
    err = esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &io_cap, 1);
    if(err != ESP_OK) return err;
    err = esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, 1);
    if(err != ESP_OK) return err;
    err = esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, 1);
    if(err != ESP_OK) return err;
    err = esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, 1);
    /* No static passkey — BLE stack generates random passkey each time (like STM32) */
    return err;
}

static bool serial_append_adv_field(
    uint8_t* raw, size_t* offset, uint8_t type, const void* payload, size_t len) {
    if(*offset + len + 2 > ESP_BLE_ADV_DATA_LEN_MAX) return false;
    raw[*offset] = len + 1;
    raw[*offset + 1] = type;
    if(len && payload) memcpy(&raw[*offset + 2], payload, len);
    *offset += len + 2;
    return true;
}

static esp_err_t serial_configure_advertising(const BleSerialConfig* config) {
    const uint8_t adv_flags = ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT;
    size_t name_len = strlen(config->device_name);
    uint8_t raw_adv[ESP_BLE_ADV_DATA_LEN_MAX] = {0};
    size_t raw_len = 0;

    if(!serial_append_adv_field(raw_adv, &raw_len, BLE_SERIAL_ADV_TYPE_FLAGS, &adv_flags, 1))
        return ESP_ERR_INVALID_SIZE;

    /* Advertise 16-bit UUID 0x3080 (same as STM32 — this is what qFlipper scans for) */
    const uint8_t svc_uuid16[] = {0x80, 0x30};  /* 0x3080 little-endian */
    if(!serial_append_adv_field(raw_adv, &raw_len, 0x03 /* Complete 16-bit UUID */, svc_uuid16, 2))
        return ESP_ERR_INVALID_SIZE;

    /* Try to fit device name */
    size_t adv_name_len = name_len;
    while(adv_name_len > 0 &&
          !serial_append_adv_field(raw_adv, &raw_len,
              (adv_name_len == name_len) ? BLE_SERIAL_ADV_TYPE_NAME_FULL : BLE_SERIAL_ADV_TYPE_NAME_SHORT,
              config->device_name, adv_name_len)) {
        adv_name_len--;
    }

    ESP_LOGI(TAG, "adv_config name_len=%u adv_name_len=%u raw_len=%u",
        (unsigned)name_len, (unsigned)adv_name_len, (unsigned)raw_len);

    esp_err_t err = esp_ble_gap_set_device_name(config->device_name);
    if(err != ESP_OK) return err;

    serial_lock_global();
    serial_state.adv_data_pending = true;
    serial_unlock_global();

    err = esp_ble_gap_config_adv_data_raw(raw_adv, raw_len);
    if(err != ESP_OK) {
        serial_lock_global();
        serial_state.adv_data_pending = false;
        serial_unlock_global();
    }
    return err;
}

static bool serial_has_custom_mac(const BleSerialConfig* config) {
    static const uint8_t zero[6] = {0};
    return memcmp(config->mac, zero, 6) != 0;
}

/* ---- Public API ---- */

void ble_serial_get_default_mac(uint8_t mac[6]) {
    esp_read_mac(mac, ESP_MAC_BT);
}

BleSerial* ble_serial_alloc(const BleSerialConfig* config) {
    if(!config) return NULL;

    esp_err_t err = serial_stack_init_once();
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "BLE stack init failed: %s", esp_err_to_name(err));
        return NULL;
    }

    BleSerial* serial = calloc(1, sizeof(BleSerial));
    if(!serial) return NULL;

    serial->mutex = xSemaphoreCreateMutex();
    if(!serial->mutex) {
        free(serial);
        return NULL;
    }

    memcpy(&serial->config, config, sizeof(serial->config));
    ESP_LOGI(TAG, "alloc name=%s bonding=%d pairing=%d",
        serial->config.device_name, serial->config.bonding, serial->config.pairing);

    serial_lock_global();
    if(serial_state.active) {
        serial_unlock_global();
        vSemaphoreDelete(serial->mutex);
        free(serial);
        return NULL;
    }
    serial_state.active = serial;
    serial_state.advertising = false;
    serial_state.advertising_requested = false;
    serial_state.adv_data_pending = false;
    serial_state.app_registered = false;
    serial_state.service_started = false;
    dis_service_started = false;
    bas_service_started = false;
    serial_state.rand_addr_pending = false;
    serial_state.rand_addr_enabled = false;
    serial_unlock_global();

    /* Register GAP callback */
    ESP_LOGI(TAG, "alloc: registering GAP callback");
    err = esp_ble_gap_register_callback(serial_gap_event_handler);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "GAP callback register failed: %s", esp_err_to_name(err));
        goto error;
    }

    /* Register GATTS callback and app */
    ESP_LOGI(TAG, "alloc: registering GATTS callback");
    err = esp_ble_gatts_register_callback(serial_gatts_event_handler);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "GATTS callback register failed: %s", esp_err_to_name(err));
        goto error;
    }

    ESP_LOGI(TAG, "alloc: applying security config");
    err = serial_apply_security_config(&serial->config);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "Security config failed: %s", esp_err_to_name(err));
        goto error;
    }

    if(serial_has_custom_mac(&serial->config)) {
        uint8_t mac[6];
        memcpy(mac, serial->config.mac, 6);
        serial_make_random_static_addr(mac);
        serial_lock_global();
        serial_state.rand_addr_pending = true;
        serial_state.rand_addr_enabled = true;
        serial_unlock_global();
        serial_adv_params.own_addr_type = BLE_ADDR_TYPE_RANDOM;
        esp_ble_gap_set_rand_addr(mac);
    } else {
        serial_adv_params.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
    }

    ESP_LOGI(TAG, "alloc: configuring advertising, has_custom_mac=%d", serial_has_custom_mac(&serial->config));
    err = serial_configure_advertising(&serial->config);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "Advertising config failed: %s", esp_err_to_name(err));
        goto error;
    }
    ESP_LOGI(TAG, "alloc: adv configured, adv_data_pending=%d", serial_state.adv_data_pending);

    /* Create semaphore to wait for GATT service to be fully started */
    serial_state.ready_sem = xSemaphoreCreateBinary();

    ESP_LOGI(TAG, "alloc: registering GATTS app (async)");
    /* Register GATTS app — triggers async chain:
     *   ESP_GATTS_REG_EVT → create attr table
     *   ESP_GATTS_CREAT_ATTR_TAB_EVT → start service
     *   ESP_GATTS_START_EVT → ready_sem signaled */
    err = esp_ble_gatts_app_register(BLE_SERIAL_APP_ID);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "GATTS app register failed: %s", esp_err_to_name(err));
        vSemaphoreDelete(serial_state.ready_sem);
        serial_state.ready_sem = NULL;
        goto error;
    }

    /* Set preferred MTU */
    esp_ble_gatt_set_local_mtu(BLE_SVC_SERIAL_DATA_LEN_MAX + 3);

    /* Wait for GATT service to be fully started (up to 5 seconds) */
    ESP_LOGI(TAG, "alloc: waiting for GATT service start...");
    if(xSemaphoreTake(serial_state.ready_sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "Timeout waiting for GATT service start! app_reg=%d svc_started=%d",
            serial_state.app_registered, serial_state.service_started);
        vSemaphoreDelete(serial_state.ready_sem);
        serial_state.ready_sem = NULL;
        goto error;
    }

    vSemaphoreDelete(serial_state.ready_sem);
    serial_state.ready_sem = NULL;

    ESP_LOGI(TAG, "Serial service ready, adv_data_pending=%d rand_addr_pending=%d",
        serial_state.adv_data_pending, serial_state.rand_addr_pending);
    return serial;

error:
    serial_lock_global();
    if(serial_state.active == serial) serial_state.active = NULL;
    serial_unlock_global();
    if(serial->mutex) vSemaphoreDelete(serial->mutex);
    free(serial);
    return NULL;
}

void ble_serial_free(BleSerial* serial) {
    if(!serial) return;

    serial_lock_global();
    if(serial_state.advertising) {
        esp_ble_gap_stop_advertising();
        serial_state.advertising = false;
    }
    serial_state.advertising_requested = false;
    if(serial_state.active == serial) serial_state.active = NULL;
    serial_unlock_global();

    if(serial_state.app_registered) {
        esp_ble_gatts_app_unregister(serial->gatts_if);
        serial_state.app_registered = false;
    }

    if(serial->mutex) vSemaphoreDelete(serial->mutex);
    free(serial);
}

void ble_serial_reset_initialized(void) {
    serial_lock_global();
    serial_state.initialized = false;
    serial_unlock_global();
}

void ble_serial_set_state_callback(BleSerial* serial, BleSerialStateCallback callback, void* context) {
    if(!serial) return;
    serial_lock(serial);
    serial->state_callback = callback;
    serial->state_context = context;
    bool connected = serial->connected;
    serial_unlock(serial);
    if(callback) callback(connected, context);
}

bool ble_serial_is_connected(BleSerial* serial) {
    if(!serial) return false;
    serial_lock(serial);
    bool c = serial->connected;
    serial_unlock(serial);
    return c;
}

void ble_serial_set_event_callback(
    BleSerial* serial,
    uint16_t buff_size,
    SerialServiceEventCallback callback,
    void* context) {
    if(!serial) return;

    serial_lock(serial);
    serial->event_callback = callback;
    serial->event_context = context;
    serial->buff_size = buff_size;
    serial->bytes_ready = buff_size;
    serial_unlock(serial);

    /* Update flow control characteristic (big-endian, like STM32) */
    if(serial->handle_table[IDX_FLOW_VAL] && serial->connected) {
        uint32_t flow_be = __builtin_bswap32((uint32_t)buff_size);
        esp_ble_gatts_set_attr_value(serial->handle_table[IDX_FLOW_VAL], sizeof(flow_be), (uint8_t*)&flow_be);
        esp_ble_gatts_send_indicate(serial->gatts_if, serial->conn_id,
            serial->handle_table[IDX_FLOW_VAL], sizeof(flow_be), (uint8_t*)&flow_be, false);
    }
}

bool ble_serial_tx(BleSerial* serial, uint8_t* data, uint16_t size) {
    if(!serial || !data || size == 0) return false;

    serial_lock(serial);
    bool connected = serial->connected;
    uint16_t conn_id = serial->conn_id;
    esp_gatt_if_t gatt_if = serial->gatts_if;
    uint16_t tx_handle = serial->handle_table[IDX_TX_VAL];
    serial_unlock(serial);

    if(!connected || !tx_handle) return false;

    /* Send as indication */
    esp_err_t err = esp_ble_gatts_send_indicate(gatt_if, conn_id, tx_handle, size, data, true);
    return err == ESP_OK;
}

void ble_serial_set_rpc_active(BleSerial* serial, FuriHalBtSerialRpcStatus status) {
    if(!serial) return;

    /* RPC Status is sent as native LE uint32_t (no byte-swap), matching STM32 behavior.
     * Flow Control uses big-endian (REVERSE_BYTES_U32 on STM32), but RPC Status does not. */
    uint32_t val = (uint32_t)status;
    if(serial->handle_table[IDX_RPC_VAL]) {
        esp_ble_gatts_set_attr_value(serial->handle_table[IDX_RPC_VAL], sizeof(val), (uint8_t*)&val);
        if(serial->connected) {
            esp_ble_gatts_send_indicate(serial->gatts_if, serial->conn_id,
                serial->handle_table[IDX_RPC_VAL], sizeof(val), (uint8_t*)&val, false);
        }
    }
}

void ble_serial_notify_buffer_is_empty(BleSerial* serial) {
    if(!serial) return;

    /* Match STM32: only send flow control update when buffer was fully consumed */
    serial_lock(serial);
    bool was_empty = (serial->bytes_ready == 0);
    if(was_empty) {
        serial->bytes_ready = serial->buff_size;
    }
    bool connected = serial->connected;
    uint16_t buff_size = serial->buff_size;
    serial_unlock(serial);

    if(was_empty && connected && serial->handle_table[IDX_FLOW_VAL]) {
        uint32_t flow_be = __builtin_bswap32((uint32_t)buff_size);
        esp_ble_gatts_set_attr_value(serial->handle_table[IDX_FLOW_VAL], sizeof(flow_be), (uint8_t*)&flow_be);
        esp_ble_gatts_send_indicate(serial->gatts_if, serial->conn_id,
            serial->handle_table[IDX_FLOW_VAL], sizeof(flow_be), (uint8_t*)&flow_be, false);
    }
}

bool ble_serial_start_advertising(void) {
    ESP_LOGI(TAG, "ble_serial_start_advertising called, active=%p", (void*)serial_state.active);
    bool accepted = false;
    serial_lock_global();
    if(serial_state.active) {
        serial_state.advertising_requested = true;
        serial_try_start_advertising_locked();
        accepted = true;
    } else {
        ESP_LOGW(TAG, "ble_serial_start_advertising: no active instance!");
    }
    serial_unlock_global();
    ESP_LOGI(TAG, "ble_serial_start_advertising returning %d", accepted);
    return accepted;
}

void ble_serial_stop_advertising(void) {
    serial_lock_global();
    serial_state.advertising_requested = false;
    if(serial_state.advertising) {
        esp_ble_gap_stop_advertising();
        serial_state.advertising = false;
    }
    serial_unlock_global();
}

bool ble_serial_is_advertising(void) {
    if(!serial_state.initialized || !serial_state.mutex) return false;
    bool adv = false;
    serial_lock_global();
    adv = serial_state.advertising_requested && serial_state.active &&
          !serial_state.active->connected;
    serial_unlock_global();
    return adv;
}

bool ble_serial_is_active(void) {
    if(!serial_state.initialized || !serial_state.mutex) return false;
    bool active = false;
    serial_lock_global();
    active = serial_state.active != NULL;
    serial_unlock_global();
    return active;
}

bool ble_serial_remove_pairing(void) {
    if(!serial_state.initialized) return false;
    int count = esp_ble_get_bond_device_num();
    if(count <= 0) return true;
    esp_ble_bond_dev_t* devs = calloc(count, sizeof(esp_ble_bond_dev_t));
    if(!devs) return false;
    int req = count;
    esp_err_t err = esp_ble_get_bond_device_list(&req, devs);
    if(err != ESP_OK) { free(devs); return false; }
    bool ok = true;
    for(int i = 0; i < req; i++) {
        if(esp_ble_remove_bond_device(devs[i].bd_addr) != ESP_OK) ok = false;
    }
    free(devs);
    return ok;
}
