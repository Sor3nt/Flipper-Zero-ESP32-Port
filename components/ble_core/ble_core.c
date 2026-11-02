/**
 * @file ble_core.c
 * Shared BLE stack bring-up, pairing and link-security policy. See ble_core.h.
 */

#include "ble_core.h"

#include <stdlib.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <nvs_flash.h>

#define TAG "ble_core"

/* Preferred connection parameters, 1.25 ms units. 15..30 ms satisfies the iOS
 * rules (min >= 15 ms, max >= min + 15 ms) and roughly halves the indication
 * round trip compared with the 30-50 ms centrals tend to pick on their own.
 * Latency stays 0 so writes from the phone are never parked behind skipped
 * connection events. Supervision timeout 4 s. */
#define BLE_CORE_CONN_INT_MIN 0x0C
#define BLE_CORE_CONN_INT_MAX 0x18
#define BLE_CORE_CONN_LATENCY 0
#define BLE_CORE_CONN_TIMEOUT 400

#define BLE_CORE_COOLDOWN_S  30
#define BLE_CORE_COOLDOWN_US (BLE_CORE_COOLDOWN_S * 1000000LL)

#define BLE_CORE_KEY_SIZE 16

/* A peer without a bond starts pairing within a second or two of our security
 * request (the phone shows its pairing dialog at the same time). One that stays
 * unencrypted this long is almost always holding a stale bond. */
#define BLE_CORE_PAIRING_STALL_MS 10000

static struct {
    BleCoreConfig cfg;

    BleCoreEventCallback event_cb;
    void* event_ctx;
    BleCoreSecureCallback secure_cb;
    void* secure_ctx;
    esp_gap_ble_cb_t profile_gap_cb;

    bool connected;
    bool secure;
    esp_bd_addr_t bda;
    esp_timer_handle_t auth_timer;
    esp_timer_handle_t stall_timer;

    bool nc_pending;
    bool pairing_started;
    /* An unauthenticated key was already answered with a re-pairing request on this link. */
    bool repair_requested;
    esp_bd_addr_t nc_bda;

    uint8_t fail_count;
    int64_t cooldown_until_us;
} s_core;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* ---- stack ---- */

bool ble_core_stack_is_up(void) {
    return esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED &&
           esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED;
}

esp_err_t ble_core_stack_up(void) {
    if(ble_core_stack_is_up()) return ESP_OK;

    esp_err_t err = nvs_flash_init();
    if(err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if(err != ESP_OK) return err;
        err = nvs_flash_init();
    }
    if(err != ESP_OK) return err;

    err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    if(esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
        esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        err = esp_bt_controller_init(&bt_cfg);
        if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    }
    if(esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_ENABLED) {
        err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
        if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    }

    if(esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_UNINITIALIZED) {
        esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
        err = esp_bluedroid_init_with_cfg(&bluedroid_cfg);
        if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    }
    if(esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) {
        err = esp_bluedroid_enable();
        if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    }

    return ESP_OK;
}

/* ---- callbacks / helpers ---- */

void ble_core_set_event_callback(BleCoreEventCallback callback, void* context) {
    portENTER_CRITICAL(&s_lock);
    s_core.event_cb = callback;
    s_core.event_ctx = context;
    portEXIT_CRITICAL(&s_lock);
}

void ble_core_set_secure_callback(BleCoreSecureCallback callback, void* context) {
    portENTER_CRITICAL(&s_lock);
    s_core.secure_cb = callback;
    s_core.secure_ctx = context;
    portEXIT_CRITICAL(&s_lock);
}

static bool ble_core_emit(BleCoreEventType type, uint32_t pin_code, bool ok) {
    BleCoreEventCallback cb;
    void* ctx;

    portENTER_CRITICAL(&s_lock);
    cb = s_core.event_cb;
    ctx = s_core.event_ctx;
    portEXIT_CRITICAL(&s_lock);

    if(!cb) return false;
    const BleCoreEvent event = {.type = type, .pin_code = pin_code, .ok = ok};
    cb(&event, ctx);
    return true;
}

static bool ble_core_in_cooldown(void) {
    int64_t until;

    portENTER_CRITICAL(&s_lock);
    until = s_core.cooldown_until_us;
    portEXIT_CRITICAL(&s_lock);

    return esp_timer_get_time() < until;
}

static void ble_core_note_failure(void) {
    bool cooldown = false;

    portENTER_CRITICAL(&s_lock);
    if(s_core.cfg.fail_limit && ++s_core.fail_count >= s_core.cfg.fail_limit) {
        s_core.fail_count = 0;
        s_core.cooldown_until_us = esp_timer_get_time() + BLE_CORE_COOLDOWN_US;
        cooldown = true;
    }
    portEXIT_CRITICAL(&s_lock);

    if(cooldown) {
        ESP_LOGW(TAG, "too many failed pairings, refusing new links for %d s", BLE_CORE_COOLDOWN_S);
    }
}

static void ble_core_drop(const uint8_t bda[6]) {
    esp_bd_addr_t addr;
    memcpy(addr, bda, sizeof(addr));
    esp_ble_gap_disconnect(addr);
}

/* ---- auth timeout: a link that never encrypts must not hold the only slot ---- */

static void ble_core_auth_timeout_cb(void* arg) {
    (void)arg;
    esp_bd_addr_t bda;
    bool drop;

    portENTER_CRITICAL(&s_lock);
    drop = s_core.connected && !s_core.secure;
    memcpy(bda, s_core.bda, sizeof(bda));
    portEXIT_CRITICAL(&s_lock);

    if(drop) {
        ESP_LOGW(TAG, "link not secured in time, disconnecting");
        ble_core_drop(bda);
    }
}

static void ble_core_auth_timer_arm(uint32_t timeout_ms) {
    if(!s_core.auth_timer) {
        const esp_timer_create_args_t args = {
            .callback = ble_core_auth_timeout_cb,
            .name = "ble_auth",
        };
        if(esp_timer_create(&args, &s_core.auth_timer) != ESP_OK) return;
    }
    esp_timer_stop(s_core.auth_timer);
    esp_timer_start_once(s_core.auth_timer, (uint64_t)timeout_ms * 1000ULL);
}

static void ble_core_auth_timer_stop(void) {
    if(s_core.auth_timer) esp_timer_stop(s_core.auth_timer);
}

/* ---- stalled pairing: the peer encrypts with a key we no longer have ---- */

static void ble_core_stall_timeout_cb(void* arg) {
    (void)arg;
    bool stalled;

    portENTER_CRITICAL(&s_lock);
    stalled = s_core.connected && !s_core.secure && !s_core.pairing_started;
    portEXIT_CRITICAL(&s_lock);

    if(stalled) {
        /* Bluedroid answers the peer's LTK request negatively without telling the
         * app, so this silence is the only symptom of a stale bond on the peer. */
        ESP_LOGW(
            TAG,
            "peer never started pairing (stale bond on the phone?) - forget this device on the phone");
        ble_core_emit(BleCoreEventPairingStalled, 0, false);
    }
}

static void ble_core_stall_timer_arm(void) {
    if(!s_core.stall_timer) {
        const esp_timer_create_args_t args = {
            .callback = ble_core_stall_timeout_cb,
            .name = "ble_stall",
        };
        if(esp_timer_create(&args, &s_core.stall_timer) != ESP_OK) return;
    }
    esp_timer_stop(s_core.stall_timer);
    esp_timer_start_once(s_core.stall_timer, (uint64_t)BLE_CORE_PAIRING_STALL_MS * 1000ULL);
}

static void ble_core_stall_timer_stop(void) {
    if(s_core.stall_timer) esp_timer_stop(s_core.stall_timer);
}

/* ---- GAP callback ownership ---- */

esp_err_t ble_core_register_gap_callback(esp_gap_ble_cb_t callback) {
    esp_err_t err = esp_ble_gap_register_callback(callback);
    if(err == ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_core.profile_gap_cb = callback;
        portEXIT_CRITICAL(&s_lock);
    }
    return err;
}

void ble_core_restore_gap_callback(void) {
    esp_gap_ble_cb_t cb;

    portENTER_CRITICAL(&s_lock);
    cb = s_core.profile_gap_cb;
    portEXIT_CRITICAL(&s_lock);

    if(cb && esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED) {
        esp_ble_gap_register_callback(cb);
    }
}

void ble_core_forward_gap_event(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
    esp_gap_ble_cb_t cb;

    portENTER_CRITICAL(&s_lock);
    cb = s_core.profile_gap_cb;
    portEXIT_CRITICAL(&s_lock);

    if(cb) cb(event, param);
}

/* ---- security config ---- */

esp_err_t ble_core_apply_config(const BleCoreConfig* config) {
    if(!config) return ESP_ERR_INVALID_ARG;

    portENTER_CRITICAL(&s_lock);
    s_core.cfg = *config;
    s_core.fail_count = 0;
    portEXIT_CRITICAL(&s_lock);

    /* LE Secure Connections + MITM. Legacy pairing is still accepted as a
     * fallback for old phones; whether the resulting key is good enough is
     * decided per profile (require_authenticated) and by the GATT permissions. */
    esp_ble_auth_req_t auth_req =
        config->bonding ? ESP_LE_AUTH_REQ_SC_MITM_BOND : ESP_LE_AUTH_REQ_SC_MITM;
    /* The Flipper has no keyboard: everything that is not "verify" shows a passkey. */
    esp_ble_io_cap_t io_cap =
        (config->pairing == BleCorePairingDisplayOnly) ? ESP_IO_CAP_OUT : ESP_IO_CAP_IO;
    uint8_t key_dist = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t key_size = BLE_CORE_KEY_SIZE;
    uint8_t accept_specified = ESP_BLE_ONLY_ACCEPT_SPECIFIED_AUTH_DISABLE;
    esp_err_t err;

    err = esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(auth_req));
    if(err != ESP_OK) return err;
    err = esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &io_cap, sizeof(io_cap));
    if(err != ESP_OK) return err;
    err = esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &key_dist, sizeof(key_dist));
    if(err != ESP_OK) return err;
    err = esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &key_dist, sizeof(key_dist));
    if(err != ESP_OK) return err;
    err = esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size));
    if(err != ESP_OK) return err;
    /* Refuse peers that try to negotiate a reduced-entropy key. */
    err = esp_ble_gap_set_security_param(ESP_BLE_SM_MIN_KEY_SIZE, &key_size, sizeof(key_size));
    if(err != ESP_OK) return err;
    err = esp_ble_gap_set_security_param(
        ESP_BLE_SM_ONLY_ACCEPT_SPECIFIED_SEC_AUTH, &accept_specified, sizeof(accept_specified));
    if(err != ESP_OK) return err;
    /* The static passkey lives in the SMP control block and survives profile
     * switches: a profile that ever set one would leave it active for every
     * later pairing until the stack restarts. Always start from random. */
    return esp_ble_gap_set_security_param(ESP_BLE_SM_CLEAR_STATIC_PASSKEY, NULL, 0);
}

/* ---- pairing ---- */

void ble_core_pairing_confirm(bool accept) {
    esp_bd_addr_t bda;
    bool pending;

    portENTER_CRITICAL(&s_lock);
    pending = s_core.nc_pending;
    s_core.nc_pending = false;
    memcpy(bda, s_core.nc_bda, sizeof(bda));
    portEXIT_CRITICAL(&s_lock);

    if(!pending) return;
    ESP_LOGI(TAG, "numeric comparison %s by user", accept ? "accepted" : "rejected");
    esp_ble_confirm_reply(bda, accept);
}

static void ble_core_on_auth_cmpl(const esp_ble_auth_cmpl_t* auth) {
    BleCoreConfig cfg;
    bool started;
    bool repair_done;
    esp_bd_addr_t bda;

    memcpy(bda, auth->bd_addr, sizeof(bda));

    portENTER_CRITICAL(&s_lock);
    cfg = s_core.cfg;
    started = s_core.pairing_started;
    repair_done = s_core.repair_requested;
    s_core.pairing_started = false;
    s_core.nc_pending = false;
    portEXIT_CRITICAL(&s_lock);

    if(!auth->success) {
        ESP_LOGW(TAG, "pairing failed, reason=0x%x", (unsigned)auth->fail_reason);
        /* Only count attempts the user was actually asked about: a stale bond
         * on the phone must not lock the device out. */
        if(started) ble_core_note_failure();
        ble_core_emit(BleCoreEventPairingDone, 0, false);
        ble_core_drop(bda);
        return;
    }

    const bool mitm = (auth->auth_mode & ESP_LE_AUTH_REQ_MITM) != 0;
    ESP_LOGI(
        TAG,
        "link encrypted: auth_mode=0x%x mitm=%d sc=%d bond=%d (pairing dialog shown=%d)",
        (unsigned)auth->auth_mode,
        mitm,
        (auth->auth_mode & ESP_LE_AUTH_REQ_SC_ONLY) != 0,
        (auth->auth_mode & ESP_LE_AUTH_BOND) != 0,
        started);

    if(cfg.require_authenticated && !mitm) {
        /* Just Works, or a bond made by the old unauthenticated firmware: anyone in
         * range could have produced it, so it must not unlock the profile.
         *
         * Do NOT delete the bond and drop the link here. Removing a bond of a
         * connected peer takes the link down and leaves the phone holding a key
         * this device no longer has: every later attempt then fails with "PIN or
         * key missing" and the phone's app reports a failed pairing without the
         * Flipper ever showing a code. Instead ask the phone to pair again with
         * MITM: a peer whose key is too weak for the requested level re-pairs,
         * and the new key replaces the old one. */
        if(!repair_done) {
            portENTER_CRITICAL(&s_lock);
            s_core.repair_requested = true;
            portEXIT_CRITICAL(&s_lock);

            ESP_LOGW(TAG, "key is not authenticated, asking peer to pair again with MITM");
            ble_core_stall_timer_arm();
            esp_ble_set_encryption(bda, ESP_BLE_SEC_ENCRYPT_MITM);
            return;
        }

        /* The peer already had its chance: it cannot do an authenticated pairing
         * (no display/keyboard). Forget the weak bond so such peers cannot fill
         * the bond table, and refuse the link. */
        ESP_LOGW(TAG, "peer cannot pair with MITM, rejecting");
        esp_ble_remove_bond_device(bda);
        ble_core_note_failure();
        ble_core_emit(BleCoreEventPairingDone, 0, false);
        ble_core_drop(bda);
        return;
    }

    portENTER_CRITICAL(&s_lock);
    s_core.fail_count = 0;
    portEXIT_CRITICAL(&s_lock);

    ble_core_emit(BleCoreEventPairingDone, 0, true);
    ble_core_mark_secure();
}

bool ble_core_handle_gap_event(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
    switch(event) {
    case ESP_GAP_BLE_SEC_REQ_EVT:
        /* Peer asks us to start security: fine, unless we are throttling. */
        ESP_LOGI(TAG, "peer security request (cooldown=%d)", ble_core_in_cooldown());
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, !ble_core_in_cooldown());
        return true;

    case ESP_GAP_BLE_NC_REQ_EVT: {
        const uint32_t code = param->ble_security.key_notif.passkey;
        ESP_LOGI(TAG, "numeric comparison requested, code %06lu", (unsigned long)code);

        portENTER_CRITICAL(&s_lock);
        memcpy(s_core.nc_bda, param->ble_security.key_notif.bd_addr, sizeof(s_core.nc_bda));
        s_core.nc_pending = true;
        s_core.pairing_started = true;
        portEXIT_CRITICAL(&s_lock);

        /* The user has to compare and confirm. Without a UI to ask, refuse. */
        if(!ble_core_emit(BleCoreEventPinVerify, code, false)) ble_core_pairing_confirm(false);
        return true;
    }

    case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
        ESP_LOGI(
            TAG,
            "passkey entry requested, code %06lu",
            (unsigned long)param->ble_security.key_notif.passkey);
        portENTER_CRITICAL(&s_lock);
        s_core.pairing_started = true;
        portEXIT_CRITICAL(&s_lock);
        ble_core_emit(BleCoreEventPinShow, param->ble_security.key_notif.passkey, false);
        return true;

    case ESP_GAP_BLE_PASSKEY_REQ_EVT:
        /* We have no keyboard to type a passkey on. */
        esp_ble_passkey_reply(param->ble_security.ble_req.bd_addr, false, 0);
        return true;

    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        ble_core_on_auth_cmpl(&param->ble_security.auth_cmpl);
        return false;

    default:
        return false;
    }
}

/* ---- link lifecycle ---- */

static void ble_core_tune_link(const uint8_t bda[6], const esp_gatt_conn_params_t* params) {
    if(params->interval <= BLE_CORE_CONN_INT_MAX && params->latency == 0) return;

    esp_ble_conn_update_params_t update = {
        .min_int = BLE_CORE_CONN_INT_MIN,
        .max_int = BLE_CORE_CONN_INT_MAX,
        .latency = BLE_CORE_CONN_LATENCY,
        .timeout = BLE_CORE_CONN_TIMEOUT,
    };
    memcpy(update.bda, bda, sizeof(update.bda));
    ESP_LOGI(
        TAG,
        "link interval %u (x1.25ms) latency %u, requesting %u..%u",
        params->interval,
        params->latency,
        BLE_CORE_CONN_INT_MIN,
        BLE_CORE_CONN_INT_MAX);
    esp_ble_gap_update_conn_params(&update);
}

bool ble_core_link_connected(const uint8_t bda[6], const esp_gatt_conn_params_t* params) {
    BleCoreConfig cfg;
    bool duplicate;

    portENTER_CRITICAL(&s_lock);
    cfg = s_core.cfg;
    /* The HID profile registers several GATT apps and each one is told about the
     * same connection: only the first event starts the link. */
    duplicate = s_core.connected && memcmp(s_core.bda, bda, sizeof(s_core.bda)) == 0;
    if(!duplicate) {
        memcpy(s_core.bda, bda, sizeof(s_core.bda));
        s_core.connected = true;
        s_core.secure = false;
        s_core.pairing_started = false;
        s_core.repair_requested = false;
        s_core.nc_pending = false;
    }
    portEXIT_CRITICAL(&s_lock);

    if(duplicate) return true;

    if(cfg.fail_limit && ble_core_in_cooldown()) {
        ESP_LOGW(TAG, "pairing cooldown active, dropping link");
        ble_core_drop(bda);
        return false;
    }

    if(cfg.link_auth_timeout_ms) ble_core_auth_timer_arm(cfg.link_auth_timeout_ms);
    if(cfg.require_authenticated) ble_core_stall_timer_arm();
    if(cfg.fast_link && params) ble_core_tune_link(bda, params);
    return true;
}

void ble_core_link_disconnected(void) {
    portENTER_CRITICAL(&s_lock);
    s_core.connected = false;
    s_core.secure = false;
    s_core.pairing_started = false;
    s_core.repair_requested = false;
    s_core.nc_pending = false;
    portEXIT_CRITICAL(&s_lock);

    ble_core_auth_timer_stop();
    ble_core_stall_timer_stop();
}

bool ble_core_mark_secure(void) {
    bool changed = false;
    BleCoreSecureCallback cb = NULL;
    void* ctx = NULL;

    portENTER_CRITICAL(&s_lock);
    if(s_core.connected && !s_core.secure) {
        s_core.secure = true;
        changed = true;
        cb = s_core.secure_cb;
        ctx = s_core.secure_ctx;
    }
    portEXIT_CRITICAL(&s_lock);

    if(changed) {
        ble_core_auth_timer_stop();
        ble_core_stall_timer_stop();
        if(cb) cb(ctx);
    }
    return changed;
}

bool ble_core_link_is_secure(void) {
    bool secure;

    portENTER_CRITICAL(&s_lock);
    secure = s_core.connected && s_core.secure;
    portEXIT_CRITICAL(&s_lock);

    return secure;
}

bool ble_core_link_is_connected(void) {
    bool connected;

    portENTER_CRITICAL(&s_lock);
    connected = s_core.connected;
    portEXIT_CRITICAL(&s_lock);

    return connected;
}

void ble_core_disconnect(void) {
    esp_bd_addr_t bda;
    bool connected;

    portENTER_CRITICAL(&s_lock);
    connected = s_core.connected;
    memcpy(bda, s_core.bda, sizeof(bda));
    portEXIT_CRITICAL(&s_lock);

    if(connected && esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED) {
        ble_core_drop(bda);
    }
}

/* ---- bonds ---- */

bool ble_core_remove_all_bonds(void) {
    if(esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) return false;

    const int count = esp_ble_get_bond_device_num();
    if(count <= 0) return true;

    esp_ble_bond_dev_t* devices = calloc(count, sizeof(esp_ble_bond_dev_t));
    if(!devices) return false;

    int listed = count;
    if(esp_ble_get_bond_device_list(&listed, devices) != ESP_OK) {
        free(devices);
        return false;
    }

    bool ok = true;
    for(int i = 0; i < listed; i++) {
        if(esp_ble_remove_bond_device(devices[i].bd_addr) != ESP_OK) ok = false;
    }

    free(devices);
    return ok;
}
