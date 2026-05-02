#include "ble_spam_hal.h"

#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_gap_ble_api.h>
#include <esp_log.h>
#include <furi.h>
#include <furi_hal_random.h>
#include <btshim.h>
#include <string.h>

#define TAG "BleSpamHal"

static volatile bool s_adv_configured = false;
static volatile bool s_advertising = false;
static volatile bool s_rand_addr_pending = false;

static esp_ble_adv_params_t s_adv_params = {
    .adv_int_min = 0x20,               // 20ms
    .adv_int_max = 0x40,               // 40ms
    .adv_type = ADV_TYPE_NONCONN_IND,  // Non-connectable
    .own_addr_type = BLE_ADDR_TYPE_RANDOM,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static void spam_gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
    switch(event) {
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
        s_adv_configured = true;
        if(param->adv_data_raw_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            esp_ble_gap_start_advertising(&s_adv_params);
        } else {
            ESP_LOGW(TAG, "adv data set failed: %d", param->adv_data_raw_cmpl.status);
        }
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        s_advertising = (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS);
        if(!s_advertising) {
            ESP_LOGW(TAG, "adv start failed: %d", param->adv_start_cmpl.status);
        }
        break;
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        s_advertising = false;
        break;
    case ESP_GAP_BLE_SET_STATIC_RAND_ADDR_EVT:
        s_rand_addr_pending = false;
        break;
    default:
        break;
    }
}

bool ble_spam_hal_start(void) {
    ESP_LOGI(TAG, "Starting BLE spam HAL...");

    // Stop btshim BLE stack
    Bt* bt = furi_record_open(RECORD_BT);
    bt_stop_stack(bt);
    furi_record_close(RECORD_BT);
    furi_delay_ms(100);

    // Init BLE controller
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

    // Init Bluedroid
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

    // Register our GAP callback
    err = esp_ble_gap_register_callback(spam_gap_event_handler);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "gap register: %s", esp_err_to_name(err));
        return false;
    }

    // Set TX power to maximum for all BLE power types
    #if defined(ESP_PWR_LVL_P21)
        esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P21);
        esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P21);
    #elif defined(ESP_PWR_LVL_P20)
        esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P20);
        esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P20);
    #elif defined(ESP_PWR_LVL_P9)
        esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P9);
        esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);
    #else
        esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P7);
        esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P7);
    #endif

    s_adv_configured = false;
    s_advertising = false;
    s_rand_addr_pending = false;

    // Set an initial random static address so that the first start_advertising()
    // call (triggered asynchronously from ADV_DATA_RAW_SET_COMPLETE_EVT) can use
    // BLE_ADDR_TYPE_RANDOM without falling back to an invalid zero address.
    ble_spam_hal_set_random_addr();

    ESP_LOGI(TAG, "BLE spam HAL ready");
    return true;
}

void ble_spam_hal_stop(void) {
    ESP_LOGI(TAG, "Stopping BLE spam HAL...");

    // Stop advertising
    if(s_advertising) {
        esp_ble_gap_stop_advertising();
        furi_delay_ms(50);
    }

    // Teardown Bluedroid
    if(esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_UNINITIALIZED) {
        esp_bluedroid_disable();
        furi_delay_ms(50);
        esp_bluedroid_deinit();
        furi_delay_ms(50);
    }

    // Teardown controller
    if(esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_IDLE) {
        esp_bt_controller_disable();
        furi_delay_ms(50);
    }
    if(esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
        esp_bt_controller_deinit();
        furi_delay_ms(50);
    }

    // Restart btshim
    Bt* bt = furi_record_open(RECORD_BT);
    bt_start_stack(bt);
    furi_record_close(RECORD_BT);

    ESP_LOGI(TAG, "BLE spam HAL stopped, btshim restored");
}

bool ble_spam_hal_set_adv_data(const uint8_t* data, uint8_t len) {
    if(len > 31) return false;

    s_adv_configured = false;
    esp_err_t err = esp_ble_gap_config_adv_data_raw((uint8_t*)data, len);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "config_adv_data_raw failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

void ble_spam_hal_stop_adv(void) {
    if(s_advertising) {
        esp_ble_gap_stop_advertising();
        for(int i = 0; i < 20 && s_advertising; i++) {
            furi_delay_ms(2);
        }
    }
}

void ble_spam_hal_set_random_addr(void) {
    esp_bd_addr_t addr;
    furi_hal_random_fill_buf(addr, 6);
    // Random static address: top two bits of MSB must be 11 (0xC0..0xFF).
    addr[0] = (addr[0] & 0x3F) | 0xC0;
    s_rand_addr_pending = true;
    esp_err_t err = esp_ble_gap_set_rand_addr(addr);
    if(err != ESP_OK) {
        ESP_LOGW(TAG, "set_rand_addr: %s", esp_err_to_name(err));
        s_rand_addr_pending = false;
        return;
    }
    // Wait up to 40ms for SET_STATIC_RAND_ADDR_EVT so the subsequent
    // start_advertising() call has a valid own-address to use.
    for(int i = 0; i < 20 && s_rand_addr_pending; i++) {
        furi_delay_ms(2);
    }
}

void ble_spam_hal_set_addr(const uint8_t addr[6]) {
    esp_bd_addr_t a;
    memcpy(a, addr, 6);
    // Ensure valid random static format
    a[0] = (a[0] & 0x3F) | 0xC0;
    esp_err_t err = esp_ble_gap_set_rand_addr(a);
    if(err != ESP_OK) {
        ESP_LOGW(TAG, "set_addr: %s", esp_err_to_name(err));
    }
    furi_delay_ms(2);
}
