/**
 * NimBLE implementation of the shared BLE lifecycle and link-security policy.
 * The Bluedroid implementation remains selected by CMake when NimBLE is off.
 */
#include "ble_core.h"
#include "ble_nimble_host.h"

#if CONFIG_BT_NIMBLE_ENABLED

#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <host/ble_hs.h>
#include <host/ble_gap.h>
#include <host/ble_sm.h>
#include <host/ble_store.h>
#include <host/util/util.h>

#define TAG "ble_core"

#define BLE_CORE_CONN_INT_MIN 0x0C
#define BLE_CORE_CONN_INT_MAX 0x18
#define BLE_CORE_CONN_LATENCY 0
#define BLE_CORE_CONN_TIMEOUT 400
#define BLE_CORE_COOLDOWN_S 30
#define BLE_CORE_COOLDOWN_US (BLE_CORE_COOLDOWN_S * 1000000LL)
#define BLE_CORE_KEY_SIZE 16
#define BLE_CORE_PAIRING_STALL_MS 10000

static struct {
    BleCoreConfig config;
    BleCoreEventCallback event_cb;
    void* event_context;
    BleCoreSecureCallback secure_cb;
    void* secure_context;
    BleCoreGapEventCallback profile_gap_cb;
    void* profile_gap_context;
    bool security_listener_registered;
    bool profile_listener_registered;
    bool connected;
    bool secure;
    bool pairing_started;
    bool repair_requested;
    bool numeric_comparison_pending;
    uint16_t conn_handle;
    ble_addr_t peer_addr;
    ble_addr_t numeric_comparison_peer;
    uint8_t fail_count;
    int64_t cooldown_until_us;
    esp_timer_handle_t auth_timer;
    esp_timer_handle_t stall_timer;
} s_core = {
    .config = {
        .pairing = BleCorePairingVerifyYesNo,
    },
    .conn_handle = BLE_HS_CONN_HANDLE_NONE,
};

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static struct ble_gap_event_listener s_security_listener;
static struct ble_gap_event_listener s_profile_listener;
static void ble_core_process_encryption_state(uint16_t conn_handle, int status);

static bool ble_core_emit(BleCoreEventType type, uint32_t pin_code, bool ok) {
    BleCoreEventCallback callback;
    void* context;

    portENTER_CRITICAL(&s_lock);
    callback = s_core.event_cb;
    context = s_core.event_context;
    portEXIT_CRITICAL(&s_lock);

    if(!callback) return false;
    const BleCoreEvent event = {.type = type, .pin_code = pin_code, .ok = ok};
    callback(&event, context);
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
    bool start_cooldown = false;
    portENTER_CRITICAL(&s_lock);
    if(s_core.config.fail_limit && ++s_core.fail_count >= s_core.config.fail_limit) {
        s_core.fail_count = 0;
        s_core.cooldown_until_us = esp_timer_get_time() + BLE_CORE_COOLDOWN_US;
        start_cooldown = true;
    }
    portEXIT_CRITICAL(&s_lock);

    if(start_cooldown) {
        ESP_LOGW(TAG, "too many failed pairings; cooldown for %u s", BLE_CORE_COOLDOWN_S);
    }
}

static void ble_core_drop(uint16_t conn_handle) {
    if(conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        int rc = ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        if(rc != 0 && rc != BLE_HS_ENOTCONN) {
            ESP_LOGW(TAG, "disconnect handle %u failed: %d", conn_handle, rc);
        }
    }
}

static void ble_core_auth_timeout_cb(void* context) {
    (void)context;
    uint16_t conn_handle;
    bool drop;
    portENTER_CRITICAL(&s_lock);
    drop = s_core.connected && !s_core.secure;
    conn_handle = s_core.conn_handle;
    portEXIT_CRITICAL(&s_lock);
    if(drop) {
        ESP_LOGW(TAG, "link did not become secure in time; disconnecting");
        ble_core_drop(conn_handle);
    }
}

static void ble_core_stall_timeout_cb(void* context) {
    (void)context;
    bool stalled;
    uint16_t conn_handle;
    portENTER_CRITICAL(&s_lock);
    stalled = s_core.connected && !s_core.secure && !s_core.pairing_started;
    conn_handle = s_core.conn_handle;
    portEXIT_CRITICAL(&s_lock);
    if(stalled) {
        struct ble_gap_conn_desc desc;
        if(ble_gap_conn_find(conn_handle, &desc) == 0 && desc.sec_state.encrypted) {
            /* A bonded central can encrypt the link itself while our
             * ble_gap_security_initiate() reports a timeout race. Reconcile
             * the live security state before claiming pairing never began. */
            ble_core_process_encryption_state(conn_handle, 0);
            return;
        }
        ESP_LOGW(TAG, "link stayed unencrypted without starting pairing (peer may hold a stale bond)");
        ble_core_emit(BleCoreEventPairingStalled, 0, false);
    }
}

static void ble_core_timer_start(
    esp_timer_handle_t* timer,
    esp_timer_cb_t callback,
    const char* name,
    uint32_t timeout_ms) {
    if(!*timer) {
        const esp_timer_create_args_t args = {.callback = callback, .name = name};
        if(esp_timer_create(&args, timer) != ESP_OK) return;
    }
    esp_timer_stop(*timer);
    esp_timer_start_once(*timer, (uint64_t)timeout_ms * 1000ULL);
}

static void ble_core_timer_stop(esp_timer_handle_t timer) {
    if(timer) esp_timer_stop(timer);
}

static void ble_core_tune_link(uint16_t conn_handle, const struct ble_gap_conn_desc* desc) {
    if(!desc || (desc->conn_itvl <= BLE_CORE_CONN_INT_MAX && desc->conn_latency == 0)) return;

    const struct ble_gap_upd_params update = {
        .itvl_min = BLE_CORE_CONN_INT_MIN,
        .itvl_max = BLE_CORE_CONN_INT_MAX,
        .latency = BLE_CORE_CONN_LATENCY,
        .supervision_timeout = BLE_CORE_CONN_TIMEOUT,
        .min_ce_len = 0,
        .max_ce_len = 0,
    };
    int rc = ble_gap_update_params(conn_handle, &update);
    if(rc != 0) {
        ESP_LOGW(TAG, "connection parameter update request failed: %d", rc);
    }
}

static void ble_core_process_encryption_state(uint16_t conn_handle, int status) {
    struct ble_gap_conn_desc desc;
    const int find_rc = ble_gap_conn_find(conn_handle, &desc);
    if(find_rc != 0 || !desc.sec_state.encrypted) {
        if(status == 0) return;
        ESP_LOGW(TAG, "encryption failed on handle %u: status=%d conn_find=%d encrypted=0", conn_handle, status, find_rc);
        ble_core_note_failure();
        ble_core_emit(BleCoreEventPairingDone, 0, false);
        ble_core_drop(conn_handle);
        return;
    }

    if(status != 0) {
        /* NimBLE can report BLE_HS_ETIMEOUT (13) when the peripheral's
         * security request races the central's automatic encryption on a
         * bonded reconnect. The live connection descriptor is authoritative. */
        ESP_LOGW(TAG, "encryption event status=%d but handle %u is already encrypted; checking link authentication", status, conn_handle);
    }

    BleCoreConfig config;
    bool repair_requested;
    portENTER_CRITICAL(&s_lock);
    config = s_core.config;
    repair_requested = s_core.repair_requested;
    s_core.pairing_started = false;
    s_core.numeric_comparison_pending = false;
    portEXIT_CRITICAL(&s_lock);

    if(config.require_authenticated && !desc.sec_state.authenticated) {
        if(!repair_requested) {
            /* A previously stored Just-Works key cannot satisfy a profile that
             * now requires MITM. Remove it and make the next connection pair
             * from scratch; do not treat the weak encrypted link as secure. */
            (void)ble_store_util_delete_peer(&desc.peer_id_addr);
            portENTER_CRITICAL(&s_lock);
            s_core.repair_requested = true;
            portEXIT_CRITICAL(&s_lock);
            ble_core_note_failure();
            ble_core_emit(BleCoreEventPairingDone, 0, false);
            ble_core_drop(conn_handle);
            return;
        }

        ble_core_note_failure();
        ble_core_emit(BleCoreEventPairingDone, 0, false);
        ble_core_drop(conn_handle);
        return;
    }

    portENTER_CRITICAL(&s_lock);
    s_core.fail_count = 0;
    s_core.repair_requested = false;
    portEXIT_CRITICAL(&s_lock);
    ble_core_emit(BleCoreEventPairingDone, 0, true);
    ble_core_mark_secure();
}

static void ble_core_on_encryption_change(const struct ble_gap_event* event) {
    ble_core_process_encryption_state(event->enc_change.conn_handle, event->enc_change.status);
}

int ble_core_handle_gap_event(struct ble_gap_event* event) {
    if(!event) return 0;

    switch(event->type) {
    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        struct ble_sm_io io = {0};
        const uint8_t action = event->passkey.params.action;
        portENTER_CRITICAL(&s_lock);
        s_core.pairing_started = true;
        if(action == BLE_SM_IOACT_NUMCMP) {
            s_core.numeric_comparison_pending = true;
            s_core.conn_handle = event->passkey.conn_handle;
        }
        portEXIT_CRITICAL(&s_lock);

        if(action == BLE_SM_IOACT_NUMCMP) {
            BleCoreConfig config;
            portENTER_CRITICAL(&s_lock);
            config = s_core.config;
            portEXIT_CRITICAL(&s_lock);
            io.action = BLE_SM_IOACT_NUMCMP;
            if(config.auto_confirm_numeric_comparison) {
                io.numcmp_accept = 1;
                (void)ble_core_emit(BleCoreEventPinShow, event->passkey.params.numcmp, false);
                (void)ble_sm_inject_io(event->passkey.conn_handle, &io);
                return 1;
            }
            if(!ble_core_emit(BleCoreEventPinVerify, event->passkey.params.numcmp, false)) {
                io.numcmp_accept = 0;
                (void)ble_sm_inject_io(event->passkey.conn_handle, &io);
                return 1;
            }
            return 1;
        }

        if(action == BLE_SM_IOACT_DISP) {
            io.action = BLE_SM_IOACT_DISP;
            io.passkey = esp_random() % 1000000U;
            ESP_LOGI(TAG, "passkey display action received; dispatching pairing-code UI event");
            (void)ble_core_emit(BleCoreEventPinShow, io.passkey, false);
            int rc = ble_sm_inject_io(event->passkey.conn_handle, &io);
            if(rc != 0) ESP_LOGW(TAG, "passkey display response failed: %d", rc);
            return 1;
        }

        /* The handheld has no keyboard or OOB channel. Cancel an input-only
         * pairing instead of supplying a predictable passkey. */
        ble_core_drop(event->passkey.conn_handle);
        return 1;
    }

    case BLE_GAP_EVENT_ENC_CHANGE:
        ble_core_on_encryption_change(event);
        return 1;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc desc;
        if(ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            (void)ble_store_util_delete_peer(&desc.peer_id_addr);
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    }

    default:
        return 0;
    }
}

static int ble_core_security_listener(struct ble_gap_event* event, void* context) {
    (void)context;
    if(!event) return 0;

    switch(event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if(event->connect.status == 0) {
            struct ble_gap_conn_desc desc;
            if(ble_gap_conn_find(event->connect.conn_handle, &desc) == 0) {
                (void)ble_core_link_connected(event->connect.conn_handle, &desc);
            }
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ble_core_link_disconnected(event->disconnect.conn.conn_handle);
        break;
    case BLE_GAP_EVENT_PASSKEY_ACTION:
    case BLE_GAP_EVENT_ENC_CHANGE:
        (void)ble_core_handle_gap_event(event);
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        return ble_core_handle_gap_event(event);
    default:
        break;
    }
    return 0;
}

static int ble_core_profile_listener(struct ble_gap_event* event, void* context) {
    (void)context;
    BleCoreGapEventCallback callback;
    void* callback_context;
    portENTER_CRITICAL(&s_lock);
    callback = s_core.profile_gap_cb;
    callback_context = s_core.profile_gap_context;
    portEXIT_CRITICAL(&s_lock);
    return callback ? callback(event, callback_context) : 0;
}

esp_err_t ble_core_stack_prepare(void) {
    const bool host_running = ble_nimble_host_is_running();
    esp_err_t err = ble_nimble_host_prepare();
    if(err != ESP_OK) return err;

    portENTER_CRITICAL(&s_lock);
    bool registered = s_core.security_listener_registered;
    portEXIT_CRITICAL(&s_lock);
    if(!registered) {
        int rc = ble_nimble_host_add_gap_listener(
            &s_security_listener,
            ble_core_security_listener,
            NULL);
        if(rc != 0 && rc != BLE_HS_EALREADY) return ESP_FAIL;
        portENTER_CRITICAL(&s_lock);
        s_core.security_listener_registered = true;
        portEXIT_CRITICAL(&s_lock);
    }

    if(host_running) return ESP_OK;
    return ble_core_apply_config(&s_core.config);
}

esp_err_t ble_core_stack_start(void) {
    esp_err_t err = ble_core_stack_prepare();
    if(err != ESP_OK) return err;
    return ble_nimble_host_start();
}

esp_err_t ble_core_stack_up(void) {
    esp_err_t err = ble_core_stack_prepare();
    if(err != ESP_OK || ble_nimble_host_is_running()) return err;

    /* Generic scanning/advertising callers must not inherit the preceding
     * peripheral profile's pairing or bonding policy. */
    const BleCoreConfig open_link_config = {
        .pairing = BleCorePairingVerifyYesNo,
        .bonding = false,
        .just_works = false,
        .require_authenticated = false,
        .auto_confirm_numeric_comparison = false,
        .link_auth_timeout_ms = 0,
        .fail_limit = 0,
        .fast_link = false,
    };
    err = ble_core_apply_config(&open_link_config);
    if(err != ESP_OK) return err;
    return ble_nimble_host_start();
}

esp_err_t ble_core_stack_down(void) {
    portENTER_CRITICAL(&s_lock);
    bool security_listener_registered = s_core.security_listener_registered;
    bool profile_listener_registered = s_core.profile_listener_registered;
    s_core.security_listener_registered = false;
    s_core.profile_listener_registered = false;
    s_core.profile_gap_cb = NULL;
    s_core.profile_gap_context = NULL;
    portEXIT_CRITICAL(&s_lock);

    if(profile_listener_registered) {
        (void)ble_nimble_host_remove_gap_listener(&s_profile_listener);
    }
    if(security_listener_registered) {
        (void)ble_nimble_host_remove_gap_listener(&s_security_listener);
    }

    ble_core_link_disconnected(BLE_HS_CONN_HANDLE_NONE);
    return ble_nimble_host_down();
}

bool ble_core_stack_is_up(void) {
    return ble_nimble_host_is_running();
}

void ble_core_set_event_callback(BleCoreEventCallback callback, void* context) {
    portENTER_CRITICAL(&s_lock);
    s_core.event_cb = callback;
    s_core.event_context = context;
    portEXIT_CRITICAL(&s_lock);
}

void ble_core_set_secure_callback(BleCoreSecureCallback callback, void* context) {
    portENTER_CRITICAL(&s_lock);
    s_core.secure_cb = callback;
    s_core.secure_context = context;
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t ble_core_apply_config(const BleCoreConfig* config) {
    if(!config) return ESP_ERR_INVALID_ARG;

    portENTER_CRITICAL(&s_lock);
    s_core.config = *config;
    s_core.fail_count = 0;
    portEXIT_CRITICAL(&s_lock);

    BleNimbleHostSecurityConfig nimble_config = {
        .io_capability = config->just_works ? BLE_HS_IO_NO_INPUT_OUTPUT :
                         config->pairing == BleCorePairingDisplayOnly ? BLE_HS_IO_DISPLAY_ONLY :
                         config->pairing == BleCorePairingInputOnly ? BLE_HS_IO_KEYBOARD_ONLY :
                                                                      BLE_HS_IO_DISPLAY_YESNO,
        .bonding = config->bonding,
        .mitm = !config->just_works,
        .secure_connections = config->secure_connections,
        .secure_connections_only = false,
        .our_key_distribution = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID,
        .peer_key_distribution = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID,
    };
    ESP_LOGI(
        TAG,
        "security config: io=%u bonding=%u mitm=%u sc=%u sc_only=%u",
        (unsigned)nimble_config.io_capability,
        (unsigned)nimble_config.bonding,
        (unsigned)nimble_config.mitm,
        (unsigned)nimble_config.secure_connections,
        (unsigned)nimble_config.secure_connections_only);
    return ble_nimble_host_set_security(&nimble_config);
}

esp_err_t ble_core_register_gap_callback(BleCoreGapEventCallback callback, void* context) {
    if(!callback) return ESP_ERR_INVALID_ARG;
    esp_err_t err = ble_core_stack_prepare();
    if(err != ESP_OK) return err;

    if(s_core.profile_listener_registered) {
        (void)ble_nimble_host_remove_gap_listener(&s_profile_listener);
        s_core.profile_listener_registered = false;
    }
    portENTER_CRITICAL(&s_lock);
    s_core.profile_gap_cb = callback;
    s_core.profile_gap_context = context;
    portEXIT_CRITICAL(&s_lock);

    int rc = ble_nimble_host_add_gap_listener(
        &s_profile_listener,
        ble_core_profile_listener,
        NULL);
    if(rc != 0) return ESP_FAIL;
    s_core.profile_listener_registered = true;
    return ESP_OK;
}

void ble_core_restore_gap_callback(void) {
    /* NimBLE listeners coexist; there is no global GAP callback to restore. */
}

void ble_core_forward_gap_event(struct ble_gap_event* event) {
    if(event) (void)ble_core_profile_listener(event, NULL);
}

void ble_core_pairing_confirm(bool accept) {
    uint16_t conn_handle;
    bool pending;
    portENTER_CRITICAL(&s_lock);
    pending = s_core.numeric_comparison_pending;
    conn_handle = s_core.conn_handle;
    s_core.numeric_comparison_pending = false;
    portEXIT_CRITICAL(&s_lock);
    if(!pending) return;

    struct ble_sm_io io = {
        .action = BLE_SM_IOACT_NUMCMP,
        .numcmp_accept = accept ? 1 : 0,
    };
    ESP_LOGI(TAG, "numeric comparison %s by user", accept ? "accepted" : "rejected");
    int rc = ble_sm_inject_io(conn_handle, &io);
    if(rc != 0) ESP_LOGW(TAG, "numeric comparison response failed: %d", rc);
}

bool ble_core_link_connected(uint16_t conn_handle, const struct ble_gap_conn_desc* desc) {
    BleCoreConfig config;
    bool duplicate;
    bool immediately_secure;
    portENTER_CRITICAL(&s_lock);
    config = s_core.config;
    duplicate = s_core.connected && s_core.conn_handle == conn_handle;
    if(!duplicate) {
        s_core.conn_handle = conn_handle;
        s_core.connected = true;
        immediately_secure = desc && desc->sec_state.encrypted &&
                             (!config.require_authenticated || desc->sec_state.authenticated);
        s_core.secure = false;
        s_core.pairing_started = false;
        s_core.repair_requested = false;
        s_core.numeric_comparison_pending = false;
        if(desc) s_core.peer_addr = desc->peer_id_addr;
    }
    portEXIT_CRITICAL(&s_lock);
    if(duplicate) return true;

    if(config.fail_limit && ble_core_in_cooldown()) {
        ESP_LOGW(TAG, "pairing cooldown active; disconnecting handle %u", conn_handle);
        ble_core_drop(conn_handle);
        return false;
    }
    if(config.link_auth_timeout_ms) {
        ble_core_timer_start(
            &s_core.auth_timer,
            ble_core_auth_timeout_cb,
            "ble_auth",
            config.link_auth_timeout_ms);
    }
    if(config.require_authenticated) {
        ble_core_timer_start(
            &s_core.stall_timer,
            ble_core_stall_timeout_cb,
            "ble_stall",
            BLE_CORE_PAIRING_STALL_MS);
    }
    if(config.fast_link) ble_core_tune_link(conn_handle, desc);
    if(config.request_security || config.bonding || config.require_authenticated) {
        int rc = ble_gap_security_initiate(conn_handle);
        ESP_LOGI(
            TAG,
            "security initiation: handle=%u bonding=%u mitm=%u sc=%u rc=%d",
            conn_handle,
            (unsigned)config.bonding,
            (unsigned)!config.just_works,
            (unsigned)config.secure_connections,
            rc);
        if(rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGW(TAG, "security initiation on handle %u failed: %d", conn_handle, rc);
        }
    }
    if(immediately_secure) ble_core_mark_secure();
    return true;
}

void ble_core_link_disconnected(uint16_t conn_handle) {
    bool matches;
    portENTER_CRITICAL(&s_lock);
    matches = s_core.connected &&
              (conn_handle == BLE_HS_CONN_HANDLE_NONE || s_core.conn_handle == conn_handle);
    if(matches) {
        s_core.connected = false;
        s_core.secure = false;
        s_core.pairing_started = false;
        s_core.repair_requested = false;
        s_core.numeric_comparison_pending = false;
        s_core.conn_handle = BLE_HS_CONN_HANDLE_NONE;
    }
    portEXIT_CRITICAL(&s_lock);
    if(matches) {
        ble_core_timer_stop(s_core.auth_timer);
        ble_core_timer_stop(s_core.stall_timer);
    }
}

bool ble_core_mark_secure(void) {
    bool changed = false;
    BleCoreSecureCallback callback = NULL;
    void* context = NULL;
    portENTER_CRITICAL(&s_lock);
    if(s_core.connected && !s_core.secure) {
        s_core.secure = true;
        changed = true;
        callback = s_core.secure_cb;
        context = s_core.secure_context;
    }
    portEXIT_CRITICAL(&s_lock);
    if(changed) {
        ble_core_timer_stop(s_core.auth_timer);
        ble_core_timer_stop(s_core.stall_timer);
        if(callback) callback(context);
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
    uint16_t conn_handle;
    portENTER_CRITICAL(&s_lock);
    conn_handle = s_core.connected ? s_core.conn_handle : BLE_HS_CONN_HANDLE_NONE;
    portEXIT_CRITICAL(&s_lock);
    ble_core_drop(conn_handle);
}

bool ble_core_remove_all_bonds(void) {
    return ble_nimble_host_remove_all_bonds();
}

#endif /* CONFIG_BT_NIMBLE_ENABLED */
