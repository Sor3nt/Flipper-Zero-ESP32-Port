#include "ble_spam_hal.h"

#include <sdkconfig.h>

#if CONFIG_BT_NIMBLE_ENABLED

#include <btshim.h>
#include <ble_core.h>
#include <esp_log.h>
#include <furi.h>
#include <furi_hal_random.h>
#include <host/ble_gap.h>
#include <host/ble_hs.h>
#include <host/ble_hs_adv.h>
#include <host/ble_hs_id.h>
#include <nimble/nimble_port.h>
#include <os/os_mbuf.h>
#include <string.h>

#define TAG "BleSpamHal"
#define BLE_SPAM_ADV_INSTANCE 0

static struct ble_gap_ext_adv_params s_adv_params = {
    .connectable = 0,
    .scannable = 0,
    .legacy_pdu = 1,
    .own_addr_type = BLE_OWN_ADDR_RANDOM,
    .primary_phy = BLE_HCI_LE_PHY_1M,
    .secondary_phy = BLE_HCI_LE_PHY_1M,
    .sid = 0,
    .itvl_min = 0x20,
    .itvl_max = 0x40,
    .tx_power = 127,
};

static bool s_hal_started;
static bool s_adv_configured;
static bool s_adv_data_configured;
static bool s_advertising;
static bool s_btshim_was_stopped;
static uint8_t s_adv_payload[BLE_HS_ADV_MAX_SZ];
static uint8_t s_adv_payload_len;
static uint8_t s_current_address[6];
static bool s_has_current_address;

static void ble_spam_make_static_addr(const uint8_t address[6], ble_addr_t* random_address) {
    random_address->type = BLE_ADDR_RANDOM;
    for(size_t i = 0; i < sizeof(random_address->val); ++i) {
        random_address->val[i] = address[sizeof(random_address->val) - 1U - i];
    }
    random_address->val[5] = (random_address->val[5] & 0x3FU) | 0xC0U;
}

static bool ble_spam_adv_set_payload(const uint8_t* data, uint8_t len) {
    if(!data || len == 0 || len > sizeof(s_adv_payload)) return false;
    struct os_mbuf* data_mbuf = os_msys_get_pkthdr(len, 0);
    if(!data_mbuf) {
        ESP_LOGE(TAG, "raw advertising mbuf allocation failed");
        return false;
    }
    int rc = os_mbuf_append(data_mbuf, data, len);
    if(rc != 0) {
        os_mbuf_free_chain(data_mbuf);
        ESP_LOGE(TAG, "raw advertising mbuf append failed: %d", rc);
        return false;
    }

    rc = ble_gap_ext_adv_set_data(BLE_SPAM_ADV_INSTANCE, data_mbuf);
    if(rc != 0) {
        ESP_LOGE(TAG, "raw advertising data update failed: %d", rc);
        return false; /* NimBLE consumes the mbuf on both success and failure. */
    }
    memcpy(s_adv_payload, data, len);
    s_adv_payload_len = len;
    s_adv_data_configured = true;
    return true;
}

static int ble_spam_ext_adv_event(struct ble_gap_event* event, void* context) {
    (void)context;
    if(event->type == BLE_GAP_EVENT_ADV_COMPLETE ||
       (event->type == BLE_GAP_EVENT_CONNECT && event->connect.status == 0)) {
        s_advertising = false;
    }
    return 0;
}

static bool ble_spam_adv_configure(void) {
    int rc = ble_gap_ext_adv_configure(
        BLE_SPAM_ADV_INSTANCE,
        &s_adv_params,
        NULL,
        ble_spam_ext_adv_event,
        NULL);
    if(rc != 0) {
        ESP_LOGE(TAG, "extended advertising configure failed: %d", rc);
        s_adv_configured = false;
        return false;
    }
    s_adv_configured = true;
    if(s_has_current_address) {
        ble_addr_t random_address;
        ble_spam_make_static_addr(s_current_address, &random_address);
        rc = ble_gap_ext_adv_set_addr(BLE_SPAM_ADV_INSTANCE, &random_address);
        if(rc != 0) {
            ESP_LOGE(TAG, "extended advertising address restore failed: %d", rc);
            s_adv_configured = false;
            return false;
        }
    }
    if(s_adv_payload_len && !ble_spam_adv_set_payload(s_adv_payload, s_adv_payload_len)) {
        s_adv_configured = false;
        return false;
    }
    return true;
}

static bool ble_spam_set_static_addr(const uint8_t address[6]) {
    if(!address || !s_hal_started || s_advertising || !s_adv_configured) return false;
    ble_addr_t random_address;
    ble_spam_make_static_addr(address, &random_address);
    int rc = ble_gap_ext_adv_set_addr(BLE_SPAM_ADV_INSTANCE, &random_address);
    if(rc != 0) ESP_LOGW(TAG, "extended advertising address update failed: %d", rc);
    if(rc == 0) {
        memcpy(s_current_address, address, sizeof(s_current_address));
        s_has_current_address = true;
    }
    return rc == 0;
}

bool ble_spam_hal_start(void) {
    if(s_hal_started) {
        ESP_LOGW(TAG, "BLE Spam HAL is already initialized");
        return false;
    }

    Bt* bt = furi_record_open(RECORD_BT);
    bt_stop_stack(bt);
    furi_record_close(RECORD_BT);

    if(ble_core_stack_is_up()) {
        ESP_LOGE(TAG, "btshim did not release the NimBLE host before BLE Spam startup");
        return false;
    }
    s_btshim_was_stopped = true;

    esp_err_t err = ble_core_stack_up();
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE host startup failed: %s", esp_err_to_name(err));
        ble_spam_hal_stop();
        return false;
    }

    s_adv_configured = false;
    s_adv_data_configured = false;
    s_advertising = false;
    s_adv_payload_len = 0;
    memset(s_adv_payload, 0, sizeof(s_adv_payload));
    s_has_current_address = false;
    memset(s_current_address, 0, sizeof(s_current_address));
    if(!ble_spam_adv_configure()) {
        ble_spam_hal_stop();
        return false;
    }
    s_hal_started = true;

    uint8_t address[6];
    furi_hal_random_fill_buf(address, sizeof(address));
    address[0] = (address[0] & 0x3F) | 0xC0;
    if(!ble_spam_set_static_addr(address)) {
        ESP_LOGE(TAG, "initial random static address setup failed");
        ble_spam_hal_stop();
        return false;
    }

    ESP_LOGI(TAG, "NimBLE BLE Spam HAL ready (legacy PDU raw advertising)");
    return true;
}

bool ble_spam_hal_set_adv_data(const uint8_t* data, uint8_t len) {
    if(!s_hal_started || !s_adv_configured || !data || len == 0 || len > BLE_HS_ADV_MAX_SZ ||
       s_advertising || ble_gap_ext_adv_active(BLE_SPAM_ADV_INSTANCE)) {
        ESP_LOGW(TAG, "raw advertising data rejected by HAL state/length check");
        return false;
    }

    return ble_spam_adv_set_payload(data, len);
}

bool ble_spam_hal_set_adv_interval_ms(uint32_t delay_ms) {
    if(!s_hal_started || delay_ms < 20 || delay_ms > 500 || s_advertising ||
       ble_gap_ext_adv_active(BLE_SPAM_ADV_INSTANCE)) {
        ESP_LOGW(TAG, "advertising interval rejected by range/state check");
        return false;
    }

    const uint32_t max_interval_ms = (delay_ms * 3U + 1U) / 2U;
    uint32_t min_units = (delay_ms * 1000U) / 625U;
    uint32_t max_units = (max_interval_ms * 1000U) / 625U;
    if(min_units < 0x20U) min_units = 0x20U;
    if(max_units < min_units) max_units = min_units;
    if(max_units > 0x4000U) max_units = 0x4000U;

    s_adv_params.itvl_min = (uint16_t)min_units;
    s_adv_params.itvl_max = (uint16_t)max_units;
    return ble_spam_adv_configure();
}

bool ble_spam_hal_start_adv(void) {
    if(!s_hal_started || !s_adv_configured || !s_adv_data_configured || s_advertising ||
       ble_gap_ext_adv_active(BLE_SPAM_ADV_INSTANCE)) {
        ESP_LOGW(TAG, "start advertising rejected by HAL state check");
        return false;
    }

    int rc = ble_gap_ext_adv_start(BLE_SPAM_ADV_INSTANCE, 0, 0);
    if(rc != 0) {
        ESP_LOGE(TAG, "start advertising failed: %d", rc);
        return false;
    }
    s_advertising = true;
    return true;
}

bool ble_spam_hal_stop_adv(void) {
    if(!s_hal_started) return true;
    if(!s_advertising && !ble_gap_ext_adv_active(BLE_SPAM_ADV_INSTANCE)) return true;
    int rc = ble_gap_ext_adv_stop(BLE_SPAM_ADV_INSTANCE);
    if(rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "stop advertising failed: %d", rc);
        return false;
    }
    s_advertising = false;
    return true;
}

bool ble_spam_hal_set_random_addr(void) {
    uint8_t address[6];
    furi_hal_random_fill_buf(address, sizeof(address));
    address[0] = (address[0] & 0x3F) | 0xC0;
    return ble_spam_set_static_addr(address);
}

void ble_spam_hal_set_addr(const uint8_t address[6]) {
    if(!address) return;
    uint8_t static_address[6];
    memcpy(static_address, address, sizeof(static_address));
    static_address[0] = (static_address[0] & 0x3F) | 0xC0;
    if(!ble_spam_set_static_addr(static_address)) {
        ESP_LOGW(TAG, "set address did not complete successfully");
    }
}

void ble_spam_hal_stop(void) {
    if(!s_btshim_was_stopped && !s_hal_started) return;
    if(!ble_spam_hal_stop_adv()) {
        ESP_LOGE(TAG, "NimBLE advertiser did not stop; retaining host state");
        return;
    }

    esp_err_t err = ble_core_stack_down();
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE host teardown failed: %s", esp_err_to_name(err));
        return;
    }

    s_adv_configured = false;
    s_adv_data_configured = false;
    s_advertising = false;
    s_adv_payload_len = 0;
    memset(s_adv_payload, 0, sizeof(s_adv_payload));
    s_has_current_address = false;
    memset(s_current_address, 0, sizeof(s_current_address));
    s_hal_started = false;
    if(s_btshim_was_stopped) {
        Bt* bt = furi_record_open(RECORD_BT);
        bt_start_stack(bt);
        furi_record_close(RECORD_BT);
        s_btshim_was_stopped = false;
    }
    ESP_LOGI(TAG, "BLE Spam HAL stopped; btshim restored");
}

#else

#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_gap_ble_api.h>
#include <esp_log.h>
#include <furi.h>
#include <furi_hal_random.h>
#include <btshim.h>
#include <string.h>

#define TAG "BleSpamHal"
#define BLE_SPAM_GAP_TIMEOUT_MS 1500U

enum {
    BleSpamGapEventAdvData = 1U << 0,
    BleSpamGapEventAdvStart = 1U << 1,
    BleSpamGapEventAdvStop = 1U << 2,
    BleSpamGapEventRandAddr = 1U << 3,
};

static FuriEventFlag* s_gap_events = NULL;
static volatile bool s_adv_configured = false;
static volatile bool s_advertising = false;
static volatile bool s_adv_data_pending = false;
static volatile bool s_adv_start_pending = false;
static volatile bool s_adv_stop_pending = false;
static volatile bool s_rand_addr_pending = false;
static volatile esp_bt_status_t s_adv_data_status = ESP_BT_STATUS_FAIL;
static volatile esp_bt_status_t s_adv_start_status = ESP_BT_STATUS_FAIL;
static volatile esp_bt_status_t s_adv_stop_status = ESP_BT_STATUS_FAIL;
static volatile esp_bt_status_t s_rand_addr_status = ESP_BT_STATUS_FAIL;

static esp_ble_adv_params_t s_adv_params = {
    .adv_int_min = 0x20,               // 20ms
    .adv_int_max = 0x40,               // 40ms
    .adv_type = ADV_TYPE_IND,          // Connectable (allows pairing)
    .own_addr_type = BLE_ADDR_TYPE_RANDOM,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static void signal_gap_event(uint32_t event_flag) {
    FuriEventFlag* events = s_gap_events;
    if(events) {
        furi_event_flag_set(events, event_flag);
    }
}

static bool wait_gap_event(uint32_t event_flag, const char* operation) {
    if(!s_gap_events) return false;

    uint32_t received = furi_event_flag_wait(
        s_gap_events, event_flag, FuriFlagWaitAny, BLE_SPAM_GAP_TIMEOUT_MS);
    if((received & FuriFlagError) != 0U) {
        if(received == FuriFlagErrorTimeout) {
            ESP_LOGE(TAG, "%s completion timed out", operation);
        } else {
            ESP_LOGE(TAG, "%s event wait failed: 0x%08lx", operation, (unsigned long)received);
        }
        return false;
    }

    if((received & event_flag) == 0) {
        ESP_LOGE(TAG, "%s completed without the expected event flag", operation);
        return false;
    }

    return true;
}

static void spam_gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
    switch(event) {
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
        s_adv_data_status = param->adv_data_raw_cmpl.status;
        s_adv_data_pending = false;
        s_adv_configured = (s_adv_data_status == ESP_BT_STATUS_SUCCESS);
        signal_gap_event(BleSpamGapEventAdvData);
        break;

    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        s_adv_start_status = param->adv_start_cmpl.status;
        s_adv_start_pending = false;
        s_advertising = (s_adv_start_status == ESP_BT_STATUS_SUCCESS);
        signal_gap_event(BleSpamGapEventAdvStart);
        if(!s_advertising) {
            ESP_LOGW(TAG, "adv start failed: %d", s_adv_start_status);
        }
        break;

    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        s_adv_stop_status = param->adv_stop_cmpl.status;
        s_adv_stop_pending = false;
        if(s_adv_stop_status == ESP_BT_STATUS_SUCCESS) {
            s_advertising = false;
        }
        signal_gap_event(BleSpamGapEventAdvStop);
        if(s_adv_stop_status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGW(TAG, "adv stop failed: %d", s_adv_stop_status);
        }
        break;

    case ESP_GAP_BLE_SET_STATIC_RAND_ADDR_EVT:
        s_rand_addr_status = param->set_rand_addr_cmpl.status;
        s_rand_addr_pending = false;
        signal_gap_event(BleSpamGapEventRandAddr);
        if(s_rand_addr_status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGW(TAG, "set random address failed: %d", s_rand_addr_status);
        }
        break;

    default:
        break;
    }
}

static bool ble_spam_hal_set_static_addr(const esp_bd_addr_t addr) {
    if(!s_gap_events || s_advertising || s_adv_data_pending || s_adv_start_pending ||
       s_adv_stop_pending || s_rand_addr_pending) {
        ESP_LOGW(TAG, "set random address rejected while GAP operation is active");
        return false;
    }

    furi_event_flag_clear(s_gap_events, BleSpamGapEventRandAddr);
    s_rand_addr_status = ESP_BT_STATUS_FAIL;
    s_rand_addr_pending = true;

    esp_err_t err = esp_ble_gap_set_rand_addr((uint8_t*)addr);
    if(err != ESP_OK) {
        s_rand_addr_pending = false;
        ESP_LOGW(TAG, "set_rand_addr request failed: %s", esp_err_to_name(err));
        return false;
    }

    if(!wait_gap_event(BleSpamGapEventRandAddr, "set_rand_addr")) return false;
    return s_rand_addr_status == ESP_BT_STATUS_SUCCESS;
}

bool ble_spam_hal_start(void) {
    ESP_LOGI(TAG, "Starting BLE spam HAL...");

    if(s_gap_events) {
        ESP_LOGE(TAG, "BLE spam HAL is already initialized");
        return false;
    }

    // Stop btshim BLE stack before taking ownership of the ESP-IDF GAP callback.
    Bt* bt = furi_record_open(RECORD_BT);
    bt_stop_stack(bt);
    furi_record_close(RECORD_BT);
    furi_delay_ms(100);

    // bt_stop_stack() has a void public API, so verify that ownership was
    // actually released before replacing the process-wide GAP callback.
    if(esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_UNINITIALIZED ||
       esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_IDLE) {
        ESP_LOGE(TAG, "btshim did not release the BLE stack before HAL startup");
        return false;
    }

    s_gap_events = furi_event_flag_alloc();
    s_adv_configured = false;
    s_advertising = false;
    s_adv_data_pending = false;
    s_adv_start_pending = false;
    s_adv_stop_pending = false;
    s_rand_addr_pending = false;

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_bt_controller_init(&bt_cfg);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "controller init: %s", esp_err_to_name(err));
        return false;
    }
    if(esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_INITED) {
        ESP_LOGE(TAG, "controller init returned success in an unexpected state");
        return false;
    }

    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "controller enable: %s", esp_err_to_name(err));
        return false;
    }
    if(esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_ENABLED) {
        ESP_LOGE(TAG, "controller enable returned success in an unexpected state");
        return false;
    }

    esp_bluedroid_config_t bd_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    err = esp_bluedroid_init_with_cfg(&bd_cfg);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "bluedroid init: %s", esp_err_to_name(err));
        return false;
    }
    if(esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_INITIALIZED) {
        ESP_LOGE(TAG, "bluedroid init returned success in an unexpected state");
        return false;
    }

    err = esp_bluedroid_enable();
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "bluedroid enable: %s", esp_err_to_name(err));
        return false;
    }
    if(esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) {
        ESP_LOGE(TAG, "bluedroid enable returned success in an unexpected state");
        return false;
    }

    err = esp_ble_gap_register_callback(spam_gap_event_handler);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "gap register: %s", esp_err_to_name(err));
        return false;
    }

#ifdef ESP_PWR_LVL_P21
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P21);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P21);
#else
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P20);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P20);
#endif

    esp_bd_addr_t initial_addr;
    furi_hal_random_fill_buf(initial_addr, sizeof(initial_addr));
    initial_addr[0] = (initial_addr[0] & 0x3F) | 0xC0;
    if(!ble_spam_hal_set_static_addr(initial_addr)) {
        ESP_LOGE(TAG, "initial random address setup failed");
        return false;
    }

    ESP_LOGI(TAG, "BLE spam HAL ready");
    return true;
}

static void ble_spam_hal_wait_pending_events(void) {
    if(s_adv_start_pending &&
       !wait_gap_event(BleSpamGapEventAdvStart, "pending advertising start")) {
        ESP_LOGW(TAG, "advertising start callback is still pending");
    }
    if(s_adv_stop_pending &&
       !wait_gap_event(BleSpamGapEventAdvStop, "pending advertising stop")) {
        ESP_LOGW(TAG, "advertising stop callback is still pending");
    }
    if(s_adv_data_pending && !wait_gap_event(BleSpamGapEventAdvData, "pending adv data")) {
        ESP_LOGW(TAG, "advertising-data callback is still pending");
    }
    if(s_rand_addr_pending && !wait_gap_event(BleSpamGapEventRandAddr, "pending random address")) {
        ESP_LOGW(TAG, "random-address callback is still pending");
    }
}

static bool ble_spam_hal_deinit_stack(void) {
    esp_bluedroid_status_t bluedroid_status = esp_bluedroid_get_status();
    if(bluedroid_status == ESP_BLUEDROID_STATUS_ENABLED) {
        esp_err_t err = esp_bluedroid_disable();
        furi_delay_ms(50);
        bluedroid_status = esp_bluedroid_get_status();
        if(err != ESP_OK && bluedroid_status == ESP_BLUEDROID_STATUS_ENABLED) {
            ESP_LOGE(TAG, "bluedroid disable: %s", esp_err_to_name(err));
            return false;
        }
    }

    if(bluedroid_status == ESP_BLUEDROID_STATUS_INITIALIZED) {
        esp_err_t err = esp_bluedroid_deinit();
        furi_delay_ms(50);
        bluedroid_status = esp_bluedroid_get_status();
        if(err != ESP_OK && bluedroid_status != ESP_BLUEDROID_STATUS_UNINITIALIZED) {
            ESP_LOGE(TAG, "bluedroid deinit: %s", esp_err_to_name(err));
            return false;
        }
    }

    if(bluedroid_status != ESP_BLUEDROID_STATUS_UNINITIALIZED) {
        ESP_LOGE(TAG, "bluedroid did not reach UNINITIALIZED state");
        return false;
    }

    esp_bt_controller_status_t controller_status = esp_bt_controller_get_status();
    if(controller_status == ESP_BT_CONTROLLER_STATUS_ENABLED) {
        esp_err_t err = esp_bt_controller_disable();
        furi_delay_ms(50);
        controller_status = esp_bt_controller_get_status();
        if(err != ESP_OK && controller_status == ESP_BT_CONTROLLER_STATUS_ENABLED) {
            ESP_LOGE(TAG, "controller disable: %s", esp_err_to_name(err));
            return false;
        }
    }

    if(controller_status == ESP_BT_CONTROLLER_STATUS_INITED) {
        esp_err_t err = esp_bt_controller_deinit();
        furi_delay_ms(50);
        controller_status = esp_bt_controller_get_status();
        if(err != ESP_OK && controller_status != ESP_BT_CONTROLLER_STATUS_IDLE) {
            ESP_LOGE(TAG, "controller deinit: %s", esp_err_to_name(err));
            return false;
        }
    }

    if(controller_status != ESP_BT_CONTROLLER_STATUS_IDLE) {
        ESP_LOGE(TAG, "controller did not reach IDLE state");
        return false;
    }

    return true;
}

void ble_spam_hal_stop(void) {
    ESP_LOGI(TAG, "Stopping BLE spam HAL...");

    if(!ble_spam_hal_stop_adv()) {
        ESP_LOGW(TAG, "advertising did not report a clean stop before teardown");
    }

    // A callback can complete after the first stop attempt. Drain all known
    // operation types once more, then stop again if a late start became active.
    ble_spam_hal_wait_pending_events();
    if(s_advertising && !ble_spam_hal_stop_adv()) {
        ESP_LOGW(TAG, "advertising remained active during teardown");
    }
    ble_spam_hal_wait_pending_events();

    // Do not free the callback's event object or restart btshim unless the host
    // and controller have both confirmed shutdown.
    if(!ble_spam_hal_deinit_stack()) {
        ESP_LOGE(TAG, "BLE teardown incomplete; retaining GAP event flag and not restarting btshim");
        return;
    }

    if(s_gap_events) {
        FuriEventFlag* events = s_gap_events;
        s_gap_events = NULL;
        furi_event_flag_free(events);
    }
    s_adv_configured = false;
    s_advertising = false;
    s_adv_data_pending = false;
    s_adv_start_pending = false;
    s_adv_stop_pending = false;
    s_rand_addr_pending = false;

    Bt* bt = furi_record_open(RECORD_BT);
    bt_start_stack(bt);
    furi_record_close(RECORD_BT);

    ESP_LOGI(TAG, "BLE spam HAL stopped, btshim restored");
}

bool ble_spam_hal_set_adv_data(const uint8_t* data, uint8_t len) {
    if(!s_gap_events || !data || len == 0 || len > 31 || s_advertising ||
       s_adv_data_pending || s_adv_start_pending || s_adv_stop_pending || s_rand_addr_pending) {
        ESP_LOGW(TAG, "raw adv data request rejected by HAL state/length check");
        return false;
    }

    furi_event_flag_clear(s_gap_events, BleSpamGapEventAdvData);
    s_adv_configured = false;
    s_adv_data_status = ESP_BT_STATUS_FAIL;
    s_adv_data_pending = true;

    esp_err_t err = esp_ble_gap_config_adv_data_raw((uint8_t*)data, len);
    if(err != ESP_OK) {
        s_adv_data_pending = false;
        ESP_LOGE(TAG, "config_adv_data_raw failed: %s", esp_err_to_name(err));
        return false;
    }

    if(!wait_gap_event(BleSpamGapEventAdvData, "config_adv_data_raw")) return false;
    return s_adv_configured && s_adv_data_status == ESP_BT_STATUS_SUCCESS;
}

bool ble_spam_hal_set_adv_interval_ms(uint32_t delay_ms) {
    if(!s_gap_events || delay_ms < 20 || delay_ms > 500 || s_advertising || s_adv_data_pending ||
       s_adv_start_pending || s_adv_stop_pending || s_rand_addr_pending) {
        ESP_LOGW(TAG, "advertising interval rejected by range/state check");
        return false;
    }

    // Legacy BLE advertising interval units are 0.625ms. Match the selected
    // payload dwell to a min interval and use a 1.5x max interval, as upstream.
    uint32_t max_interval_ms = (delay_ms * 3U + 1U) / 2U;
    uint32_t min_units = (delay_ms * 1000U) / 625U;
    uint32_t max_units = (max_interval_ms * 1000U) / 625U;
    if(min_units < 0x20U) min_units = 0x20U;
    if(max_units < min_units) max_units = min_units;
    if(max_units > 0x4000U) max_units = 0x4000U;

    s_adv_params.adv_int_min = (uint16_t)min_units;
    s_adv_params.adv_int_max = (uint16_t)max_units;
    return true;
}

bool ble_spam_hal_start_adv(void) {
    if(!s_gap_events || !s_adv_configured || s_advertising || s_adv_data_pending ||
       s_adv_start_pending || s_adv_stop_pending || s_rand_addr_pending) {
        ESP_LOGW(TAG, "start advertising rejected by HAL state check");
        return false;
    }

    furi_event_flag_clear(s_gap_events, BleSpamGapEventAdvStart);
    s_adv_start_status = ESP_BT_STATUS_FAIL;
    s_adv_start_pending = true;

    esp_err_t err = esp_ble_gap_start_advertising(&s_adv_params);
    if(err != ESP_OK) {
        s_adv_start_pending = false;
        ESP_LOGE(TAG, "start advertising request failed: %s", esp_err_to_name(err));
        return false;
    }

    if(!wait_gap_event(BleSpamGapEventAdvStart, "start advertising")) return false;
    return s_advertising && s_adv_start_status == ESP_BT_STATUS_SUCCESS;
}

bool ble_spam_hal_stop_adv(void) {
    if(!s_gap_events) return true;

    // A start operation can finish after its caller timed out. Resolve that state
    // before deciding whether a stop command is required.
    if(s_adv_start_pending) {
        if(!wait_gap_event(BleSpamGapEventAdvStart, "pending start advertising")) return false;
        if(s_adv_start_status != ESP_BT_STATUS_SUCCESS) return !s_advertising;
    }

    if(s_adv_stop_pending) {
        if(!wait_gap_event(BleSpamGapEventAdvStop, "pending stop advertising")) return false;
        return s_adv_stop_status == ESP_BT_STATUS_SUCCESS && !s_advertising;
    }

    if(s_adv_data_pending) {
        if(!wait_gap_event(BleSpamGapEventAdvData, "pending adv data")) return false;
        if(s_adv_data_status != ESP_BT_STATUS_SUCCESS) return false;
    }

    if(s_rand_addr_pending) {
        if(!wait_gap_event(BleSpamGapEventRandAddr, "pending random address")) return false;
        if(s_rand_addr_status != ESP_BT_STATUS_SUCCESS) return false;
    }

    if(!s_advertising) return true;

    furi_event_flag_clear(s_gap_events, BleSpamGapEventAdvStop);
    s_adv_stop_status = ESP_BT_STATUS_FAIL;
    s_adv_stop_pending = true;

    esp_err_t err = esp_ble_gap_stop_advertising();
    if(err != ESP_OK) {
        s_adv_stop_pending = false;
        ESP_LOGE(TAG, "stop advertising request failed: %s", esp_err_to_name(err));
        return false;
    }

    if(!wait_gap_event(BleSpamGapEventAdvStop, "stop advertising")) return false;
    return s_adv_stop_status == ESP_BT_STATUS_SUCCESS && !s_advertising;
}

bool ble_spam_hal_set_random_addr(void) {
    esp_bd_addr_t addr;
    furi_hal_random_fill_buf(addr, sizeof(addr));
    // Random static address: top two bits of the most significant byte are 11.
    addr[0] = (addr[0] & 0x3F) | 0xC0;
    return ble_spam_hal_set_static_addr(addr);
}

void ble_spam_hal_set_addr(const uint8_t addr[6]) {
    if(!addr) return;

    esp_bd_addr_t static_addr;
    memcpy(static_addr, addr, sizeof(static_addr));
    static_addr[0] = (static_addr[0] & 0x3F) | 0xC0;
    if(!ble_spam_hal_set_static_addr(static_addr)) {
        ESP_LOGW(TAG, "set_addr did not complete successfully");
    }
}

#endif /* CONFIG_BT_NIMBLE_ENABLED */
