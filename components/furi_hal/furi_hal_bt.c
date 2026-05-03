#include "furi_hal_bt.h"

#include <furi.h>
#include <ble_hid.h>
#include <ble_serial.h>
#include <ble_profile/extra_profiles/hid_profile.h>
#include <ble_profile/extra_profiles/serial_profile.h>
#include <esp_log.h>
#include <stddef.h>
#include <string.h>

#define TAG "FuriHalBt"

/* ---- BLE Glue stubs ---- */

static const BleGlueC2Info ble_glue_c2_info = {
    .mode = BleGlueC2ModeStack,
    .StackType = 0,
    .VersionMajor = 0,
    .VersionMinor = 0,
    .VersionSub = 0,
    .VersionBranch = 0,
    .VersionReleaseType = 0,
    .FusVersionMajor = 0,
    .FusVersionMinor = 0,
    .FusVersionSub = 0,
    .MemorySizeSram2B = 0,
    .MemorySizeSram2A = 0,
    .FusMemorySizeSram2B = 0,
    .FusMemorySizeSram2A = 0,
    .FusMemorySizeFlash = 0,
    .StackTypeString = "ESP-BLE",
};

const BleGlueHardfaultInfo* ble_glue_get_hardfault_info(void) {
    return NULL;
}

const BleGlueC2Info* ble_glue_get_c2_info(void) {
    return &ble_glue_c2_info;
}

/* ---- GAP event bridge ---- */

static GapEventCallback gap_event_cb = NULL;
static void* gap_event_ctx = NULL;

/** Emit a GapEvent to the stored callback. Called from profile state callbacks. */
void furi_hal_bt_emit_gap_event(GapEvent event) {
    if(gap_event_cb) {
        gap_event_cb(event, gap_event_ctx);
    }
}

static void bt_gap_event_bridge_hid(bool connected, void* context) {
    (void)context;
    GapEvent event = {0};
    event.type = connected ? GapEventTypeConnected : GapEventTypeDisconnected;
    furi_hal_bt_emit_gap_event(event);

    if(!connected && ble_hid_is_advertising()) {
        GapEvent adv_event = {.type = GapEventTypeStartAdvertising};
        furi_hal_bt_emit_gap_event(adv_event);
    }
}

static void bt_gap_event_bridge_serial(bool connected, void* context) {
    (void)context;
    GapEvent event = {0};
    event.type = connected ? GapEventTypeConnected : GapEventTypeDisconnected;
    furi_hal_bt_emit_gap_event(event);

    if(!connected && ble_serial_is_advertising()) {
        GapEvent adv_event = {.type = GapEventTypeStartAdvertising};
        furi_hal_bt_emit_gap_event(adv_event);
    }
}

/* ---- Radio stack ---- */

bool furi_hal_bt_start_radio_stack(void) {
    return true;
}

bool furi_hal_bt_is_gatt_gap_supported(void) {
    return true;
}

bool furi_hal_bt_is_active(void) {
    return ble_hid_is_active() || ble_serial_is_active();
}

/* ---- Profile management ---- */

static FuriHalBleProfileBase* current_profile = NULL;

void furi_hal_bt_reinit(void) {
    current_profile = NULL;
    gap_event_cb = NULL;
    gap_event_ctx = NULL;
}

FuriHalBleProfileBase* furi_hal_bt_change_app(
    const FuriHalBleProfileTemplate* profile_template,
    FuriHalBleProfileParams params,
    const GapRootSecurityKeys* root_keys,
    GapEventCallback event_cb,
    void* context) {
    (void)root_keys;

    /* Stop the old profile if any */
    if(current_profile) {
        furi_hal_bt_stop_advertising();
        current_profile->config->stop(current_profile);
        current_profile = NULL;
    }

    /* Store the GAP event callback */
    gap_event_cb = event_cb;
    gap_event_ctx = context;

    /* Start the new profile */
    FuriHalBleProfileBase* profile = profile_template->start(params);
    if(!profile) {
        ESP_LOGE(TAG, "Failed to start BLE profile");
        gap_event_cb = NULL;
        gap_event_ctx = NULL;
        return NULL;
    }

    current_profile = profile;

    /* Install the right state callback based on profile type */
    if(furi_hal_bt_check_profile_type(profile, ble_profile_hid)) {
        ble_profile_hid_set_state_callback(profile, bt_gap_event_bridge_hid, NULL);
    } else if(furi_hal_bt_check_profile_type(profile, ble_profile_serial)) {
        ble_profile_serial_set_state_callback(profile, bt_gap_event_bridge_serial, NULL);
    }

    ESP_LOGI(TAG, "BLE profile started");
    return profile;
}

bool furi_hal_bt_check_profile_type(
    FuriHalBleProfileBase* profile,
    const FuriHalBleProfileTemplate* profile_template) {
    return profile && profile->config == profile_template;
}

/* ---- Advertising ---- */

void furi_hal_bt_start_advertising(void) {
    bool started = false;

    ESP_LOGI(TAG, "furi_hal_bt_start_advertising: profile=%p", (void*)current_profile);
    if(current_profile) {
        bool is_hid = furi_hal_bt_check_profile_type(current_profile, ble_profile_hid);
        bool is_serial = furi_hal_bt_check_profile_type(current_profile, ble_profile_serial);
        ESP_LOGI(TAG, "  is_hid=%d is_serial=%d", is_hid, is_serial);
        if(is_hid) {
            started = ble_hid_start_advertising();
        } else if(is_serial) {
            started = ble_serial_start_advertising();
        } else {
            ESP_LOGW(TAG, "  unknown profile type!");
        }
    } else {
        ESP_LOGW(TAG, "  no current profile!");
    }

    ESP_LOGI(TAG, "  started=%d", started);
    if(started && gap_event_cb) {
        GapEvent event = {.type = GapEventTypeStartAdvertising};
        gap_event_cb(event, gap_event_ctx);
    }
}

void furi_hal_bt_stop_advertising(void) {
    if(current_profile) {
        if(furi_hal_bt_check_profile_type(current_profile, ble_profile_hid)) {
            ble_hid_stop_advertising();
        } else if(furi_hal_bt_check_profile_type(current_profile, ble_profile_serial)) {
            ble_serial_stop_advertising();
        }
    }

    if(gap_event_cb) {
        GapEvent event = {.type = GapEventTypeStopAdvertising};
        gap_event_cb(event, gap_event_ctx);
    }
}

/* ---- Battery / power stubs ---- */

void furi_hal_bt_update_battery_level(uint8_t battery_level) {
    (void)battery_level;
}

void furi_hal_bt_update_power_state(bool charging) {
    (void)charging;
}

/* ---- Key storage stubs (ESP32 Bluedroid uses NVS) ---- */

static uint8_t key_storage_dummy[16];

void furi_hal_bt_get_key_storage_buff(uint8_t** key_buff_addr, uint16_t* key_buff_size) {
    if(key_buff_addr) *key_buff_addr = key_storage_dummy;
    if(key_buff_size) *key_buff_size = sizeof(key_storage_dummy);
}

void furi_hal_bt_nvm_sram_sem_acquire(void) {
}

void furi_hal_bt_nvm_sram_sem_release(void) {
}

bool furi_hal_bt_clear_white_list(void) {
    return ble_hid_remove_pairing();
}

void furi_hal_bt_set_key_storage_change_callback(
    BleGlueKeyStorageChangedCallback callback,
    void* context) {
    (void)callback;
    (void)context;
}

/* ---- Extra Beacon (non-connectable advertising) ---- */

#include <extra_beacon.h>
#include <esp_gap_ble_api.h>
#include <esp_bt_main.h>
#include <furi_hal_random.h>

static GapExtraBeaconState s_beacon_state = GapExtraBeaconStateStopped;
static GapExtraBeaconConfig s_beacon_config;
static uint8_t s_beacon_data[EXTRA_BEACON_MAX_DATA_SIZE];
static uint8_t s_beacon_data_len;
static volatile bool s_beacon_adv_data_set = false;

static esp_ble_adv_params_t s_beacon_adv_params = {
    .adv_int_min = 0x00A0, // 100ms default
    .adv_int_max = 0x00A0,
    .adv_type = ADV_TYPE_NONCONN_IND,
    .own_addr_type = BLE_ADDR_TYPE_RANDOM,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static void beacon_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
    switch(event) {
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
        s_beacon_adv_data_set = true;
        if(param->adv_data_raw_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            esp_ble_gap_start_advertising(&s_beacon_adv_params);
        }
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if(param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            s_beacon_state = GapExtraBeaconStateStarted;
        }
        break;
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        s_beacon_state = GapExtraBeaconStateStopped;
        break;
    default:
        break;
    }
}

void gap_extra_beacon_init(void) {
    s_beacon_state = GapExtraBeaconStateStopped;
    memset(&s_beacon_config, 0, sizeof(s_beacon_config));
    s_beacon_data_len = 0;
}

GapExtraBeaconState gap_extra_beacon_get_state(void) {
    return s_beacon_state;
}

bool gap_extra_beacon_set_config(const GapExtraBeaconConfig* config) {
    if(!config) return false;
    memcpy(&s_beacon_config, config, sizeof(GapExtraBeaconConfig));

    // Convert intervals from ms to BLE units (0.625ms)
    s_beacon_adv_params.adv_int_min = (config->min_adv_interval_ms * 1000) / 625;
    s_beacon_adv_params.adv_int_max = (config->max_adv_interval_ms * 1000) / 625;
    if(s_beacon_adv_params.adv_int_min < 0x20) s_beacon_adv_params.adv_int_min = 0x20;
    if(s_beacon_adv_params.adv_int_max < s_beacon_adv_params.adv_int_min)
        s_beacon_adv_params.adv_int_max = s_beacon_adv_params.adv_int_min;

    // Channel map
    s_beacon_adv_params.channel_map = (esp_ble_adv_channel_t)config->adv_channel_map;

    // Address type
    s_beacon_adv_params.own_addr_type =
        (config->address_type == GapAddressTypePublic) ? BLE_ADDR_TYPE_PUBLIC :
                                                         BLE_ADDR_TYPE_RANDOM;

    // Set random address if configured
    if(config->address_type == GapAddressTypeRandom) {
        esp_bd_addr_t addr;
        memcpy(addr, config->address, 6);
        addr[0] = (addr[0] & 0x3F) | 0xC0; // Ensure valid random static format
        esp_ble_gap_set_rand_addr(addr);
    }

    return true;
}

const GapExtraBeaconConfig* gap_extra_beacon_get_config(void) {
    return &s_beacon_config;
}

bool gap_extra_beacon_set_data(const uint8_t* data, uint8_t length) {
    if(length > EXTRA_BEACON_MAX_DATA_SIZE) return false;
    memcpy(s_beacon_data, data, length);
    s_beacon_data_len = length;
    return true;
}

uint8_t gap_extra_beacon_get_data(uint8_t* data) {
    if(data) {
        memcpy(data, s_beacon_data, s_beacon_data_len);
    }
    return s_beacon_data_len;
}

bool gap_extra_beacon_start(void) {
    if(s_beacon_state == GapExtraBeaconStateStarted) return true;
    if(esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) return false;

    esp_ble_gap_register_callback(beacon_gap_cb);

    s_beacon_adv_data_set = false;
    esp_err_t err = esp_ble_gap_config_adv_data_raw(s_beacon_data, s_beacon_data_len);
    if(err != ESP_OK) return false;

    // Wait for advertising data to be set and advertising to start
    for(int i = 0; i < 50 && s_beacon_state != GapExtraBeaconStateStarted; i++) {
        furi_delay_ms(10);
    }
    return s_beacon_state == GapExtraBeaconStateStarted;
}

bool gap_extra_beacon_stop(void) {
    if(s_beacon_state != GapExtraBeaconStateStarted) return true;

    esp_err_t err = esp_ble_gap_stop_advertising();
    if(err != ESP_OK) return false;

    for(int i = 0; i < 50 && s_beacon_state != GapExtraBeaconStateStopped; i++) {
        furi_delay_ms(10);
    }
    return s_beacon_state == GapExtraBeaconStateStopped;
}

bool furi_hal_bt_extra_beacon_is_active(void) {
    return gap_extra_beacon_get_state() == GapExtraBeaconStateStarted;
}

bool furi_hal_bt_extra_beacon_set_config(const GapExtraBeaconConfig* config) {
    return gap_extra_beacon_set_config(config);
}

const GapExtraBeaconConfig* furi_hal_bt_extra_beacon_get_config(void) {
    return gap_extra_beacon_get_config();
}

bool furi_hal_bt_extra_beacon_set_data(const uint8_t* data, uint8_t len) {
    return gap_extra_beacon_set_data(data, len);
}

uint8_t furi_hal_bt_extra_beacon_get_data(uint8_t* data) {
    return gap_extra_beacon_get_data(data);
}

bool furi_hal_bt_extra_beacon_start(void) {
    return gap_extra_beacon_start();
}

bool furi_hal_bt_extra_beacon_stop(void) {
    return gap_extra_beacon_stop();
}
