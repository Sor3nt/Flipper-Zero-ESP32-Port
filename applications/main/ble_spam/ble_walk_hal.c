#include "ble_walk_hal.h"

#include <sdkconfig.h>

#if CONFIG_BT_NIMBLE_ENABLED
#include "ble_walk_hal_nimble.inc"
#else

#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_gap_ble_api.h>
#include <esp_gattc_api.h>
#include <esp_gatt_common_api.h>
#include <esp_log.h>
#include <furi.h>
#include <btshim.h>
#include <esp_attr.h>
#include <string.h>

#define TAG "BleWalkHal"
#define WALK_GATTC_APP_ID 0

static EXT_RAM_BSS_ATTR BleWalkDevice s_devices[BLE_WALK_MAX_DEVICES];
static uint16_t s_device_count = 0;
static volatile bool s_scanning = false;

static EXT_RAM_BSS_ATTR BleWalkService s_services[BLE_WALK_MAX_SERVICES];
static uint16_t s_service_count = 0;
static volatile bool s_services_ready = false;

static EXT_RAM_BSS_ATTR BleWalkChar s_chars[BLE_WALK_MAX_CHARS];
static uint16_t s_char_count = 0;
static volatile bool s_chars_ready = false;

static EXT_RAM_BSS_ATTR uint8_t s_read_buf[BLE_WALK_MAX_VALUE_LEN];
static uint16_t s_read_len = 0;
static volatile bool s_read_ready = false;
static volatile uint8_t s_read_status = 0;

static esp_gatt_if_t s_gattc_if = ESP_GATT_IF_NONE;
static uint16_t s_conn_id = 0;
static volatile bool s_connected = false;
static bool s_hal_started = false;
static esp_bd_addr_t s_last_connect_addr;
// True from the moment esp_ble_gattc_open() is issued until the stack
// resolves it (OPEN_EVT, success or fail, or a DISCONNECT_EVT for a cancelled
// pending connection). A retry while this is still true gets rejected by
// Bluedroid with "BT_GATT: gatt_connect wrong state" — see ble_walk_hal_connect().
static volatile bool s_gattc_busy = false;

// ---------------------------------------------------------------------------
// GAP callback — scanning
// ---------------------------------------------------------------------------

static void walk_gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
    switch(event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        if(param->scan_param_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            esp_ble_gap_start_scanning(0); // scan indefinitely
        }
        break;

    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        s_scanning = (param->scan_start_cmpl.status == ESP_BT_STATUS_SUCCESS);
        break;

    case ESP_GAP_BLE_SCAN_RESULT_EVT:
        if(param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
            // Parse name from AD data + scan response
            char parsed_name[32] = "";
            // Try adv data first, then scan response
            for(int pass = 0; pass < 2 && parsed_name[0] == '\0'; pass++) {
                uint8_t* adv;
                uint8_t adv_len;
                if(pass == 0) {
                    adv = param->scan_rst.ble_adv;
                    adv_len = param->scan_rst.adv_data_len;
                } else {
                    adv = param->scan_rst.ble_adv + param->scan_rst.adv_data_len;
                    adv_len = param->scan_rst.scan_rsp_len;
                }
                uint8_t pos = 0;
                while(pos < adv_len) {
                    uint8_t len = adv[pos];
                    if(len == 0 || pos + len >= adv_len) break;
                    uint8_t type = adv[pos + 1];
                    if(type == 0x09 || type == 0x08) {
                        uint8_t name_len = len - 1;
                        if(name_len > 31) name_len = 31;
                        memcpy(parsed_name, &adv[pos + 2], name_len);
                        parsed_name[name_len] = '\0';
                        break;
                    }
                    pos += len + 1;
                }
            }

            // Check if device already in list
            int found = -1;
            for(int i = 0; i < s_device_count; i++) {
                if(memcmp(s_devices[i].addr, param->scan_rst.bda, 6) == 0) {
                    found = i;
                    break;
                }
            }

            if(found >= 0) {
                s_devices[found].rssi = param->scan_rst.rssi;
                if(s_devices[found].name[0] == '\0' && parsed_name[0] != '\0') {
                    strncpy(s_devices[found].name, parsed_name, 31);
                    s_devices[found].name[31] = '\0';
                }
                // Update raw AD data
                s_devices[found].adv_data_len = param->scan_rst.adv_data_len;
                if(s_devices[found].adv_data_len > 31) s_devices[found].adv_data_len = 31;
                memcpy(s_devices[found].adv_data, param->scan_rst.ble_adv, s_devices[found].adv_data_len);
            } else if(s_device_count < BLE_WALK_MAX_DEVICES) {
                BleWalkDevice* dev = &s_devices[s_device_count];
                memcpy(dev->addr, param->scan_rst.bda, 6);
                dev->addr_type = param->scan_rst.ble_addr_type;
                dev->rssi = param->scan_rst.rssi;
                strncpy(dev->name, parsed_name, 31);
                dev->name[31] = '\0';

                // Store raw AD data for cloning
                dev->adv_data_len = param->scan_rst.adv_data_len;
                if(dev->adv_data_len > 31) dev->adv_data_len = 31;
                memcpy(dev->adv_data, param->scan_rst.ble_adv, dev->adv_data_len);

                dev->scan_rsp_len = param->scan_rst.scan_rsp_len;
                if(dev->scan_rsp_len > 31) dev->scan_rsp_len = 31;
                if(dev->scan_rsp_len > 0) {
                    memcpy(dev->scan_rsp_data,
                           param->scan_rst.ble_adv + param->scan_rst.adv_data_len,
                           dev->scan_rsp_len);
                }

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
// GATTC callback — connect, service/char discovery, read/write
// ---------------------------------------------------------------------------

static void walk_gattc_event_handler(
    esp_gattc_cb_event_t event,
    esp_gatt_if_t gattc_if,
    esp_ble_gattc_cb_param_t* param) {

    switch(event) {
    case ESP_GATTC_REG_EVT:
        if(param->reg.status == ESP_GATT_OK) {
            s_gattc_if = gattc_if;
            ESP_LOGI(TAG, "GATTC registered, if=%d", gattc_if);
        }
        break;

    case ESP_GATTC_CONNECT_EVT:
        ESP_LOGI(TAG, "GATTC CONNECT_EVT: conn_id=%d, addr=%02X:%02X:%02X:%02X:%02X:%02X",
                 param->connect.conn_id,
                 param->connect.remote_bda[0], param->connect.remote_bda[1],
                 param->connect.remote_bda[2], param->connect.remote_bda[3],
                 param->connect.remote_bda[4], param->connect.remote_bda[5]);
        break;

    case ESP_GATTC_OPEN_EVT:
        s_gattc_busy = false;
        ESP_LOGI(TAG, "GATTC OPEN_EVT: status=%d, conn_id=%d",
                 param->open.status, param->open.conn_id);
        if(param->open.status == ESP_GATT_OK) {
            s_conn_id = param->open.conn_id;
            s_connected = true;
            esp_ble_gattc_send_mtu_req(gattc_if, s_conn_id);
        } else {
            s_connected = false;
            ESP_LOGW(TAG, "OPEN failed: status=%d", param->open.status);
        }
        break;

    case ESP_GATTC_CFG_MTU_EVT:
        ESP_LOGI(TAG, "MTU configured: %d", param->cfg_mtu.mtu);
        break;

    case ESP_GATTC_SEARCH_RES_EVT:
        if(s_service_count < BLE_WALK_MAX_SERVICES) {
            BleWalkService* svc = &s_services[s_service_count];
            memcpy(&svc->uuid, &param->search_res.srvc_id.uuid, sizeof(esp_bt_uuid_t));
            svc->start_handle = param->search_res.start_handle;
            svc->end_handle = param->search_res.end_handle;
            s_service_count++;
        }
        break;

    case ESP_GATTC_SEARCH_CMPL_EVT:
        s_services_ready = true;
        ESP_LOGI(TAG, "Service discovery complete: %d services", s_service_count);
        break;

    case ESP_GATTC_READ_CHAR_EVT:
        s_read_status = param->read.status;
        if(param->read.status == ESP_GATT_OK) {
            s_read_len = param->read.value_len;
            if(s_read_len > BLE_WALK_MAX_VALUE_LEN) s_read_len = BLE_WALK_MAX_VALUE_LEN;
            memcpy(s_read_buf, param->read.value, s_read_len);
        } else {
            s_read_len = 0;
        }
        s_read_ready = true;
        break;

    case ESP_GATTC_WRITE_CHAR_EVT:
        ESP_LOGI(TAG, "Write complete: status=%d", param->write.status);
        break;

    case ESP_GATTC_DISCONNECT_EVT:
        s_connected = false;
        s_gattc_busy = false; // also fires for a cancelled pending connection
        ESP_LOGI(TAG, "GATTC DISCONNECT: reason=%d", param->disconnect.reason);
        break;

    default:
        ESP_LOGD(TAG, "GATTC event: %d", event);
        break;
    }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool ble_walk_hal_start(void) {
    if(s_hal_started) {
        ESP_LOGI(TAG, "BLE Walk HAL already started");
        return true;
    }

    ESP_LOGI(TAG, "Starting BLE Walk HAL...");

    Bt* bt = furi_record_open(RECORD_BT);
    bt_stop_stack(bt);
    furi_record_close(RECORD_BT);
    furi_delay_ms(100);

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

    err = esp_ble_gap_register_callback(walk_gap_event_handler);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "gap register: %s", esp_err_to_name(err));
        return false;
    }

    // Reset state BEFORE async registration
    s_device_count = 0;
    s_scanning = false;
    s_connected = false;
    s_gattc_busy = false;
    s_gattc_if = ESP_GATT_IF_NONE;

    err = esp_ble_gattc_register_callback(walk_gattc_event_handler);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "gattc register: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_ble_gattc_app_register(WALK_GATTC_APP_ID);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "gattc app register: %s", esp_err_to_name(err));
        return false;
    }

    // Wait for REG_EVT callback to set s_gattc_if
    for(int i = 0; i < 40 && s_gattc_if == ESP_GATT_IF_NONE; i++) {
        furi_delay_ms(50);
    }

    esp_ble_gatt_set_local_mtu(200);

    s_hal_started = true;
    ESP_LOGI(TAG, "BLE Walk HAL ready (gattc_if=%d)", s_gattc_if);
    return true;
}

void ble_walk_hal_stop(void) {
    ESP_LOGI(TAG, "Stopping BLE Walk HAL...");

    if(s_connected) {
        esp_ble_gattc_close(s_gattc_if, s_conn_id);
        furi_delay_ms(100);
    }
    if(s_scanning) {
        esp_ble_gap_stop_scanning();
        furi_delay_ms(50);
    }

    esp_ble_gattc_app_unregister(s_gattc_if);
    furi_delay_ms(50);

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

    Bt* bt = furi_record_open(RECORD_BT);
    bt_start_stack(bt);
    furi_record_close(RECORD_BT);

    s_hal_started = false;
    ESP_LOGI(TAG, "BLE Walk HAL stopped");
}

// ---------------------------------------------------------------------------
// Scanning
// ---------------------------------------------------------------------------

bool ble_walk_hal_start_scan(void) {
    // Idempotent: re-issuing esp_ble_gap_set_scan_params() while a scan is
    // already active gets rejected by the controller ("Command Disallowed",
    // HCI op 0x200b) — bta_dm_ble_set_scan_fil_params then logs "fail to set
    // scan params" every time. Auto-Walk's poll loop calls this once per
    // iteration (~1.7s) regardless of scan state, so without this guard it
    // spams that failure indefinitely whenever no new device is found.
    if(s_scanning) return true;

    // Preserve the existing device list — the scan keeps populating it. On
    // first start after ble_walk_hal_start() the list is already empty (HAL
    // init zeroes it), so no explicit clear is needed here. Callers that
    // want a fresh list can call ble_walk_hal_clear_devices() first.
    esp_ble_scan_params_t scan_params = {
        .scan_type = BLE_SCAN_TYPE_ACTIVE,
        .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval = 0x50,  // 50ms
        .scan_window = 0x30,    // 30ms
    };

    esp_err_t err = esp_ble_gap_set_scan_params(&scan_params);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "set_scan_params: %s", esp_err_to_name(err));
        return false;
    }
    // Scan starts automatically in GAP callback after params are set
    return true;
}

void ble_walk_hal_stop_scan(void) {
    if(s_scanning) {
        esp_ble_gap_stop_scanning();
        for(int i = 0; i < 20 && s_scanning; i++) {
            furi_delay_ms(5);
        }
    }
}

bool ble_walk_hal_is_scanning(void) {
    return s_scanning;
}

BleWalkDevice* ble_walk_hal_get_devices(uint16_t* count) {
    *count = s_device_count;
    return s_devices;
}

// ---------------------------------------------------------------------------
// GATT Client
// ---------------------------------------------------------------------------

bool ble_walk_hal_connect(BleWalkDevice* device, volatile bool* abort_flag) {
    if(s_gattc_if == ESP_GATT_IF_NONE) {
        ESP_LOGE(TAG, "connect: gattc_if not registered");
        return false;
    }

    ble_walk_hal_stop_scan();
    furi_delay_ms(100);

    s_connected = false;
    s_service_count = 0;
    s_services_ready = false;

    memcpy(s_last_connect_addr, device->addr, 6);

    ESP_LOGI(TAG, "Connecting to %02X:%02X:%02X:%02X:%02X:%02X (type=%d, if=%d, name='%s')",
             device->addr[0], device->addr[1], device->addr[2],
             device->addr[3], device->addr[4], device->addr[5],
             device->addr_type, s_gattc_if, device->name);

    s_gattc_busy = true;
    esp_err_t err = esp_ble_gattc_open(s_gattc_if, device->addr, device->addr_type, true);
    if(err != ESP_OK) {
        s_gattc_busy = false;
        ESP_LOGE(TAG, "gattc_open failed: %s", esp_err_to_name(err));
        return false;
    }

    // Wait for connection (max 4s) — abortable
    bool aborted = false;
    for(int i = 0; i < 80 && !s_connected; i++) {
        if(abort_flag && *abort_flag) {
            aborted = true;
            ESP_LOGW(TAG, "connect aborted after %d ms", i * 50);
            break;
        }
        furi_delay_ms(50);
    }

    if(!s_connected) {
        ESP_LOGW(TAG, "Connection %s, cancelling", aborted ? "aborted" : "timeout after 4s");
        // Cancel the pending connection attempt. esp_ble_gattc_close() needs
        // an already-OPEN connection; since OPEN never completed there is no
        // valid conn_id to close (conn_id=0 is not "this" connection), and
        // calling it anyway does not cancel anything. Only the GAP-level
        // disconnect actually cancels a pending LE create-connection.
        esp_ble_gap_disconnect(device->addr);
        // Wait for the stack to confirm the cancel (OPEN_EVT or DISCONNECT_EVT
        // clears s_gattc_busy) instead of a fixed short delay. Without this,
        // a quick retry to the same/another device calls esp_ble_gattc_open()
        // while this gattc_if is still marked busy from the aborted attempt,
        // and Bluedroid rejects it immediately with "gatt_connect wrong state".
        // Stay abortable so the GUI thread never blocks longer than needed.
        uint32_t wait_ms = aborted ? 200 : 3000;
        for(uint32_t i = 0; i < wait_ms / 20 && s_gattc_busy; i++) {
            if(abort_flag && *abort_flag) break;
            furi_delay_ms(20);
        }
        // Stack didn't confirm in time — force-clear so we don't wedge every
        // future connect attempt for the rest of the session.
        if(s_gattc_busy) {
            ESP_LOGW(TAG, "cancel not confirmed after %lums, clearing busy flag anyway",
                     (unsigned long)wait_ms);
            s_gattc_busy = false;
        }
    }
    return s_connected;
}

void ble_walk_hal_disconnect(void) {
    if(s_gattc_if == ESP_GATT_IF_NONE) return;

    if(s_connected) {
        ESP_LOGI(TAG, "Disconnecting conn_id=%d...", s_conn_id);
        esp_ble_gattc_close(s_gattc_if, s_conn_id);
        for(int i = 0; i < 40 && s_connected; i++) {
            furi_delay_ms(50);
        }
        if(s_connected) {
            // Force GAP-level disconnect
            esp_ble_gap_disconnect(s_last_connect_addr);
            furi_delay_ms(200);
            s_connected = false;
            ESP_LOGW(TAG, "Disconnect forced");
        }
    }
}

bool ble_walk_hal_is_connected(void) {
    return s_connected;
}

bool ble_walk_hal_discover_services(void) {
    if(!s_connected) return false;

    s_service_count = 0;
    s_services_ready = false;

    esp_err_t err = esp_ble_gattc_search_service(s_gattc_if, s_conn_id, NULL);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "search_service: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool ble_walk_hal_services_ready(void) {
    return s_services_ready;
}

BleWalkService* ble_walk_hal_get_services(uint16_t* count) {
    *count = s_service_count;
    return s_services;
}

bool ble_walk_hal_discover_chars(BleWalkService* service) {
    if(!s_connected) return false;

    s_char_count = 0;
    s_chars_ready = false;

    uint16_t count = BLE_WALK_MAX_CHARS;
    esp_gattc_char_elem_t result[BLE_WALK_MAX_CHARS];

    esp_err_t err = esp_ble_gattc_get_all_char(
        s_gattc_if, s_conn_id, service->start_handle, service->end_handle,
        result, &count, 0);

    if(err != ESP_OK) {
        ESP_LOGE(TAG, "get_all_char: %s", esp_err_to_name(err));
        s_chars_ready = true;
        return false;
    }

    for(int i = 0; i < count && s_char_count < BLE_WALK_MAX_CHARS; i++) {
        BleWalkChar* chr = &s_chars[s_char_count];
        memcpy(&chr->uuid, &result[i].uuid, sizeof(esp_bt_uuid_t));
        chr->handle = result[i].char_handle;
        chr->properties = result[i].properties;
        s_char_count++;
    }

    s_chars_ready = true;
    ESP_LOGI(TAG, "Found %d characteristics", s_char_count);
    return true;
}

bool ble_walk_hal_chars_ready(void) {
    return s_chars_ready;
}

BleWalkChar* ble_walk_hal_get_chars(uint16_t* count) {
    *count = s_char_count;
    return s_chars;
}

bool ble_walk_hal_read_char(uint16_t handle) {
    if(!s_connected) return false;

    s_read_ready = false;
    s_read_len = 0;

    esp_err_t err = esp_ble_gattc_read_char(s_gattc_if, s_conn_id, handle, ESP_GATT_AUTH_REQ_NONE);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "read_char: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool ble_walk_hal_read_ready(void) {
    return s_read_ready;
}

uint8_t* ble_walk_hal_get_read_value(uint16_t* len) {
    *len = s_read_len;
    return s_read_buf;
}

uint8_t ble_walk_hal_get_read_status(void) {
    return s_read_status;
}

bool ble_walk_hal_write_char(uint16_t handle, const uint8_t* data, uint16_t len) {
    if(!s_connected) return false;

    esp_err_t err = esp_ble_gattc_write_char(
        s_gattc_if, s_conn_id, handle, len, (uint8_t*)data,
        ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "write_char: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

#endif /* CONFIG_BT_NIMBLE_ENABLED */
