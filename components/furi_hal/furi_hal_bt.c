#include "furi_hal_bt.h"
#include "boards/board.h"

#include <furi.h>
#include <furi_hal_power.h>
#include <ble_core.h>
#include <ble_hid.h>
#include <ble_serial.h>
#include <ble_profile/extra_profiles/hid_profile.h>
#include <ble_profile/extra_profiles/serial_profile.h>
#include <esp_log.h>
#include <stddef.h>
#include <string.h>
#include <sdkconfig.h>

#define TAG "FuriHalBt"

#if CONFIG_BT_NIMBLE_ENABLED
static void furi_hal_bt_reset_beacon_state_after_stack_down(void);

static bool furi_hal_bt_stop_extra_beacon_for_stack_change(void) {
    if(!furi_hal_bt_extra_beacon_is_active()) return true;
    return furi_hal_bt_extra_beacon_stop();
}
#endif

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
static volatile bool s_ble_sleep_inhibited = false;
static volatile bool s_beacon_sleep_inhibited = false;
static bool s_ble_sleep_inhibit_applied = false;
static bool s_beacon_sleep_inhibit_applied = false;
static bool s_ble_inhibit_transition_active = false;

void furi_hal_bt_init(void) {
    /* The inhibit guard is static state; no blocking primitive is needed. */
}

/* Update the requested state under the short critical section, then serialize
 * power API calls through one drain loop. The power API takes a mutex, so never
 * call it while holding a critical section or another lock. Concurrent callers
 * only update desired state; the active drainer applies transitions in order,
 * preventing an exit from overtaking its matching enter. */
static void furi_hal_bt_inhibit_apply(volatile bool* flag, bool desired) {
    bool drain = false;
    {
        FURI_CRITICAL_ENTER();
        *flag = desired;
        if(!s_ble_inhibit_transition_active &&
           (s_ble_sleep_inhibited != s_ble_sleep_inhibit_applied ||
            s_beacon_sleep_inhibited != s_beacon_sleep_inhibit_applied)) {
            s_ble_inhibit_transition_active = true;
            drain = true;
        }
        FURI_CRITICAL_EXIT();
    }

    if(!drain) return;

    while(true) {
        volatile bool* target = NULL;
        bool target_state = false;

        {
            FURI_CRITICAL_ENTER();
            if(s_ble_sleep_inhibited != s_ble_sleep_inhibit_applied) {
                target = &s_ble_sleep_inhibited;
                target_state = s_ble_sleep_inhibited;
            } else if(s_beacon_sleep_inhibited != s_beacon_sleep_inhibit_applied) {
                target = &s_beacon_sleep_inhibited;
                target_state = s_beacon_sleep_inhibited;
            } else {
                s_ble_inhibit_transition_active = false;
            }
            FURI_CRITICAL_EXIT();
        }

        if(!target) break;

        if(target_state) {
            furi_hal_power_light_sleep_inhibit_enter();
        } else {
            furi_hal_power_light_sleep_inhibit_exit();
        }

        {
            FURI_CRITICAL_ENTER();
            if(target == &s_ble_sleep_inhibited) {
                s_ble_sleep_inhibit_applied = target_state;
            } else {
                s_beacon_sleep_inhibit_applied = target_state;
            }
            FURI_CRITICAL_EXIT();
        }
    }
}

static void furi_hal_bt_set_ble_sleep_inhibited(bool inhibited) {
    furi_hal_bt_inhibit_apply(&s_ble_sleep_inhibited, inhibited);
}

/** Emit a GapEvent to the stored callback. Called from profile state callbacks. */
void furi_hal_bt_emit_gap_event(GapEvent event) {
    if(gap_event_cb) {
        if(event.type == GapEventTypePinCodeShow || event.type == GapEventTypePairingStalled) {
            ESP_LOGI(TAG, "dispatching BLE pairing UI event type=%u", (unsigned)event.type);
        }
        gap_event_cb(event, gap_event_ctx);
    } else if(event.type == GapEventTypePinCodeShow || event.type == GapEventTypePairingStalled) {
        ESP_LOGW(TAG, "dropping BLE pairing UI event type=%u: no GAP callback", (unsigned)event.type);
    }
}

/** Translate pairing events from ble_core (BTC task) into GapEvents for the BT service. */
static void bt_core_event_bridge(const BleCoreEvent* event, void* context) {
    (void)context;
    GapEvent gap_event = {0};

    switch(event->type) {
    case BleCoreEventPinShow:
        gap_event.type = GapEventTypePinCodeShow;
        gap_event.data.pin_code = event->pin_code;
        break;
    case BleCoreEventPinVerify:
        gap_event.type = GapEventTypePinCodeVerify;
        gap_event.data.pin_code = event->pin_code;
        break;
    case BleCoreEventPairingDone:
        gap_event.type = GapEventTypePairingDone;
        gap_event.data.pairing_ok = event->ok;
        break;
    case BleCoreEventPairingStalled:
        gap_event.type = GapEventTypePairingStalled;
        break;
    default:
        return;
    }
    furi_hal_bt_emit_gap_event(gap_event);
}

void furi_hal_bt_pairing_confirm(bool accept) {
    ble_core_pairing_confirm(accept);
}

static void bt_gap_event_bridge_hid(bool connected, void* context) {
    (void)context;
    furi_hal_bt_set_ble_sleep_inhibited(connected);
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
    furi_hal_bt_set_ble_sleep_inhibited(connected);
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

bool furi_hal_bt_is_available(void) {
#if defined(BOARD_HAS_BLE) && (BOARD_HAS_BLE == 0)
    return false;
#else
    return true;
#endif
}

bool furi_hal_bt_is_gatt_gap_supported(void) {
    return true;
}

bool furi_hal_bt_is_active(void) {
    return ble_hid_is_active() || ble_serial_is_active();
}

/* ---- Profile management ---- */

static FuriHalBleProfileBase* current_profile = NULL;
static bool profile_teardown_failed = false;

/* Same race as furi_hal_bt_set_ble_sleep_inhibited: the beacon flag is touched
 * from the BT service thread (start/stop) and from furi_hal_bt_reinit. Both go
 * through the shared serialised path so their enter/exit cannot reorder. */
static void furi_hal_bt_sleep_inhibit_enter(volatile bool* inhibited) {
    furi_hal_bt_inhibit_apply(inhibited, true);
}

static void furi_hal_bt_sleep_inhibit_exit(volatile bool* inhibited) {
    furi_hal_bt_inhibit_apply(inhibited, false);
}

void furi_hal_bt_reinit(void) {
    current_profile = NULL;
    /* Called after the BT service has disabled/deinitialized the controller. */
    profile_teardown_failed = false;
    ble_hid_reset_gatts_state();
#if CONFIG_BT_NIMBLE_ENABLED
    furi_hal_bt_reset_beacon_state_after_stack_down();
#endif
    gap_event_cb = NULL;
    gap_event_ctx = NULL;
#if CONFIG_BT_NIMBLE_ENABLED
    ble_core_link_disconnected(BLE_HS_CONN_HANDLE_NONE);
#else
    ble_core_link_disconnected();
#endif
    furi_hal_bt_set_ble_sleep_inhibited(false);
    furi_hal_bt_sleep_inhibit_exit(&s_beacon_sleep_inhibited);
}

bool furi_hal_bt_profile_recovery_required(void) {
    return profile_teardown_failed;
}

bool furi_hal_bt_stop_current_profile(void) {
#if CONFIG_BT_NIMBLE_ENABLED
    const bool beacon_stopped = furi_hal_bt_stop_extra_beacon_for_stack_change();
#endif
    furi_hal_bt_stop_advertising();
    if(!current_profile) {
#if CONFIG_BT_NIMBLE_ENABLED
        return beacon_stopped;
#else
        return true;
#endif
    }

    FuriHalBleProfileBase* profile = current_profile;
    current_profile = NULL;
    bool stopped = profile->config->stop(profile);
#if CONFIG_BT_NIMBLE_ENABLED
    stopped = stopped && beacon_stopped;
#endif
    furi_hal_bt_set_ble_sleep_inhibited(false);
    if(!stopped) {
        profile_teardown_failed = true;
        ESP_LOGE(TAG, "Active BLE profile teardown incomplete during stack shutdown");
    }
    return stopped;
}

FuriHalBleProfileBase* furi_hal_bt_change_app(
    const FuriHalBleProfileTemplate* profile_template,
    FuriHalBleProfileParams params,
    const GapRootSecurityKeys* root_keys,
    GapEventCallback event_cb,
    void* context) {
    (void)root_keys;

    if(profile_teardown_failed) {
        ESP_LOGE(TAG, "BLE profile start rejected: previous GATTS teardown failed; restart BT stack");
        return NULL;
    }

#if CONFIG_BT_NIMBLE_ENABLED
    if(!furi_hal_bt_stop_extra_beacon_for_stack_change()) {
        profile_teardown_failed = true;
        ESP_LOGE(TAG, "NimBLE extra beacon could not stop before profile replacement");
        return NULL;
    }
#endif

    /* Stop the old profile if any */
    if(current_profile) {
        furi_hal_bt_stop_advertising();
        bool stopped = current_profile->config->stop(current_profile);
        current_profile = NULL;
        furi_hal_bt_set_ble_sleep_inhibited(false);
        if(!stopped) {
            profile_teardown_failed = true;
            ESP_LOGE(TAG, "BLE profile teardown incomplete; refusing profile replacement until BT stack restart");
            return NULL;
        }
#if CONFIG_BT_NIMBLE_ENABLED
        esp_err_t stack_err = ble_core_stack_down();
        if(stack_err != ESP_OK) {
            profile_teardown_failed = true;
            ESP_LOGE(TAG, "NimBLE host teardown failed during profile replacement: %s",
                     esp_err_to_name(stack_err));
            return NULL;
        }
        ble_hid_reset_gatts_state();
#endif
    }

    /* Store the GAP event callback */
    gap_event_cb = event_cb;
    gap_event_ctx = context;
    ble_core_set_event_callback(bt_core_event_bridge, NULL);

    /* Start the new profile */
    FuriHalBleProfileBase* profile = profile_template->start(params);
    if(!profile) {
        ESP_LOGE(TAG, "Failed to start BLE profile");
        if(ble_hid_is_teardown_pending() || ble_serial_is_teardown_pending()) {
            profile_teardown_failed = true;
            ESP_LOGE(TAG, "BLE profile startup left GATTS teardown pending; refusing profile changes until BT stack restart");
        }
        gap_event_cb = NULL;
        gap_event_ctx = NULL;
        furi_hal_bt_set_ble_sleep_inhibited(false);
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
    /* With a phone connected the profile only records the request and advertises
     * again after the disconnect (which reports StartAdvertising itself). Reporting
     * it now flipped the BT service to "advertising" mid-connection, and the status
     * update that follows hides the pairing code screen. */
    if(started && gap_event_cb && !ble_core_link_is_connected()) {
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

    /* Like the STM32 HAL, stopping also terminates a live connection. Without
     * this bt_disconnect() only tore down the RPC session and left the phone
     * connected (and able to write) to a profile that was no longer serviced. */
    ble_core_disconnect();

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

/* ---- Key storage stubs (ESP32 NimBLE uses its NVS bond store) ---- */

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
    /* Bonds live in NimBLE's NVS store and are shared by every profile. */
    return ble_core_remove_all_bonds();
}

void furi_hal_bt_set_key_storage_change_callback(
    BleGlueKeyStorageChangedCallback callback,
    void* context) {
    (void)callback;
    (void)context;
}

/* ---- Extra Beacon (non-connectable advertising) ---- */

#include <extra_beacon.h>
#if CONFIG_BT_NIMBLE_ENABLED
#include <furi_hal_random.h>
#include <host/ble_gap.h>
#include <host/ble_hs.h>
#include <host/ble_hs_id.h>
#include <os/os_mbuf.h>

#define EXTRA_BEACON_ADV_INSTANCE 1

static GapExtraBeaconState s_beacon_state = GapExtraBeaconStateStopped;
static GapExtraBeaconConfig s_beacon_config;
static uint8_t s_beacon_data[EXTRA_BEACON_MAX_DATA_SIZE];
static uint8_t s_beacon_data_len;
static bool s_beacon_config_set;

static void furi_hal_bt_reset_beacon_state_after_stack_down(void) {
    s_beacon_state = GapExtraBeaconStateStopped;
}

static int beacon_gap_cb(struct ble_gap_event* event, void* context) {
    (void)context;
    if(event && event->type == BLE_GAP_EVENT_ADV_COMPLETE) {
        s_beacon_state = GapExtraBeaconStateStopped;
    }
    return 0;
}

void gap_extra_beacon_init(void) {
    s_beacon_state = GapExtraBeaconStateStopped;
    memset(&s_beacon_config, 0, sizeof(s_beacon_config));
    memset(s_beacon_data, 0, sizeof(s_beacon_data));
    s_beacon_data_len = 0;
    s_beacon_config_set = false;
}

GapExtraBeaconState gap_extra_beacon_get_state(void) {
    return s_beacon_state;
}

bool gap_extra_beacon_set_config(const GapExtraBeaconConfig* config) {
    if(!config || s_beacon_state == GapExtraBeaconStateStarted) return false;
    memcpy(&s_beacon_config, config, sizeof(s_beacon_config));
    s_beacon_config_set = true;
    return true;
}

const GapExtraBeaconConfig* gap_extra_beacon_get_config(void) {
    return &s_beacon_config;
}

bool gap_extra_beacon_set_data(const uint8_t* data, uint8_t length) {
    if(length > sizeof(s_beacon_data) || (length > 0 && !data)) return false;
    if(length) memcpy(s_beacon_data, data, length);
    s_beacon_data_len = length;
    return true;
}

uint8_t gap_extra_beacon_get_data(uint8_t* data) {
    if(data && s_beacon_data_len) memcpy(data, s_beacon_data, s_beacon_data_len);
    return s_beacon_data_len;
}

bool gap_extra_beacon_start(void) {
    if(s_beacon_state == GapExtraBeaconStateStarted) return true;
    if(!ble_core_stack_is_up()) return false;

    struct ble_gap_ext_adv_params params = {
        .connectable = 0,
        .scannable = 0,
        .legacy_pdu = 1,
        .own_addr_type = BLE_OWN_ADDR_RANDOM,
        .primary_phy = BLE_HCI_LE_PHY_1M,
        .secondary_phy = BLE_HCI_LE_PHY_1M,
        .channel_map = 0x07,
        .sid = 1,
        .tx_power = 127,
    };
    if(s_beacon_config_set) {
        uint32_t min_ms = s_beacon_config.min_adv_interval_ms;
        uint32_t max_ms = s_beacon_config.max_adv_interval_ms;
        if(min_ms < 20U) min_ms = 20U;
        if(max_ms < min_ms) max_ms = min_ms;
        if(max_ms > 10240U) max_ms = 10240U;
        params.itvl_min = (min_ms * 1000U) / 625U;
        params.itvl_max = (max_ms * 1000U) / 625U;
        params.channel_map = (uint8_t)s_beacon_config.adv_channel_map & 0x07U;
        if(params.channel_map == 0) params.channel_map = 0x07U;
        params.own_addr_type = s_beacon_config.address_type == GapAddressTypePublic ?
                                   BLE_OWN_ADDR_PUBLIC : BLE_OWN_ADDR_RANDOM;
    } else {
        params.itvl_min = 0x00A0;
        params.itvl_max = 0x00A0;
    }

    furi_hal_bt_sleep_inhibit_enter(&s_beacon_sleep_inhibited);
    int rc = ble_gap_ext_adv_configure(
        EXTRA_BEACON_ADV_INSTANCE, &params, NULL, beacon_gap_cb, NULL);
    if(rc != 0) goto fail;

    if(params.own_addr_type == BLE_OWN_ADDR_RANDOM) {
        ble_addr_t random_addr = {.type = BLE_ADDR_RANDOM};
        if(s_beacon_config_set) {
            uint8_t public_order[6];
            memcpy(public_order, s_beacon_config.address, sizeof(public_order));
            public_order[0] = (public_order[0] & 0x3FU) | 0xC0U;
            for(size_t i = 0; i < sizeof(random_addr.val); ++i) {
                random_addr.val[i] = public_order[5U - i];
            }
        } else {
            rc = ble_hs_id_gen_rnd(0, &random_addr);
            if(rc != 0) goto fail;
        }
        rc = ble_gap_ext_adv_set_addr(EXTRA_BEACON_ADV_INSTANCE, &random_addr);
        if(rc != 0) goto fail;
    }

    struct os_mbuf* data = os_msys_get_pkthdr(s_beacon_data_len, 0);
    if(!data) goto fail;
    if(s_beacon_data_len && os_mbuf_append(data, s_beacon_data, s_beacon_data_len) != 0) {
        os_mbuf_free_chain(data);
        goto fail;
    }
    rc = ble_gap_ext_adv_set_data(EXTRA_BEACON_ADV_INSTANCE, data);
    if(rc != 0) goto fail; /* NimBLE consumes the mbuf on both outcomes. */
    rc = ble_gap_ext_adv_start(EXTRA_BEACON_ADV_INSTANCE, 0, 0);
    if(rc != 0) goto fail;

    s_beacon_state = GapExtraBeaconStateStarted;
    return true;

fail:
    ESP_LOGW(TAG, "NimBLE extra beacon start failed: %d", rc);
    furi_hal_bt_sleep_inhibit_exit(&s_beacon_sleep_inhibited);
    return false;
}

bool gap_extra_beacon_stop(void) {
    if(!ble_core_stack_is_up()) {
        s_beacon_state = GapExtraBeaconStateStopped;
        furi_hal_bt_sleep_inhibit_exit(&s_beacon_sleep_inhibited);
        return true;
    }
    if(s_beacon_state != GapExtraBeaconStateStarted) {
        furi_hal_bt_sleep_inhibit_exit(&s_beacon_sleep_inhibited);
        return true;
    }
    int rc = ble_gap_ext_adv_stop(EXTRA_BEACON_ADV_INSTANCE);
    if(rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "NimBLE extra beacon stop failed: %d", rc);
        return false;
    }
    s_beacon_state = GapExtraBeaconStateStopped;
    furi_hal_bt_sleep_inhibit_exit(&s_beacon_sleep_inhibited);
    return true;
}

#else
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
    case ESP_GAP_BLE_SET_STATIC_RAND_ADDR_EVT:
    case ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT:
        /* Results of the beacon's own address/advertising setup: handing them to
         * the profile would make it restart its advertising over the beacon. */
        break;
    default:
        /* Bluedroid has one GAP callback and the beacon only borrows it for its
         * advertising: pairing requests, auth results and connection updates still
         * belong to the profile. Swallowing them left the phone connected with no
         * pairing code on screen. */
        ble_core_forward_gap_event(event, param);
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

    furi_hal_bt_sleep_inhibit_enter(&s_beacon_sleep_inhibited);
    esp_ble_gap_register_callback(beacon_gap_cb);

    s_beacon_adv_data_set = false;
    esp_err_t err = esp_ble_gap_config_adv_data_raw(s_beacon_data, s_beacon_data_len);
    if(err != ESP_OK) {
        furi_hal_bt_sleep_inhibit_exit(&s_beacon_sleep_inhibited);
        return false;
    }

    // Wait for advertising data to be set and advertising to start
    for(int i = 0; i < 50 && s_beacon_state != GapExtraBeaconStateStarted; i++) {
        furi_delay_ms(10);
    }
    const bool started = s_beacon_state == GapExtraBeaconStateStarted;
    if(!started) furi_hal_bt_sleep_inhibit_exit(&s_beacon_sleep_inhibited);
    return started;
}

bool gap_extra_beacon_stop(void) {
    if(s_beacon_state != GapExtraBeaconStateStarted) {
        furi_hal_bt_sleep_inhibit_exit(&s_beacon_sleep_inhibited);
        return true;
    }

    esp_err_t err = esp_ble_gap_stop_advertising();
    if(err != ESP_OK) return false;

    for(int i = 0; i < 50 && s_beacon_state != GapExtraBeaconStateStopped; i++) {
        furi_delay_ms(10);
    }
    const bool stopped = s_beacon_state == GapExtraBeaconStateStopped;
    if(stopped) {
        furi_hal_bt_sleep_inhibit_exit(&s_beacon_sleep_inhibited);
        /* Give the GAP callback back to the profile that owned it before the beacon. */
        if(current_profile) ble_core_restore_gap_callback();
    }
    return stopped;
}
#endif /* CONFIG_BT_NIMBLE_ENABLED */

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
    const bool was_active = furi_hal_bt_extra_beacon_is_active();
    const bool started = gap_extra_beacon_start();
    if(started && !was_active) {
        const GapEvent event = {.type = GapEventTypeBeaconStart};
        furi_hal_bt_emit_gap_event(event);
    }
    return started;
}

bool furi_hal_bt_extra_beacon_stop(void) {
    const bool was_active = furi_hal_bt_extra_beacon_is_active();
    const bool stopped = gap_extra_beacon_stop();
    if(stopped && was_active) {
        const GapEvent event = {.type = GapEventTypeBeaconStop};
        furi_hal_bt_emit_gap_event(event);
    }
    return stopped;
}
