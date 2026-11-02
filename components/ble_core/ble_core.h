/**
 * @file ble_core.h
 * Shared BLE stack bring-up, pairing and link-security policy.
 *
 * Both GATT profiles (ble_serial = mobile app/RPC, ble_hid = BadUSB/remote)
 * share the firmware's selected BLE host. This module owns the common host
 * lifecycle, pairing policy, and link-security handling used by those profiles:
 *
 *  - controller + host bring-up, including NimBLE on the ESP32 port, so it
 *    also works after the stack was torn down for WiFi / light sleep
 *  - the SMP parameters (LE Secure Connections + MITM, 16 byte keys, no
 *    leftover static passkey)
 *  - the pairing UX contract: numeric comparison needs an explicit user
 *    confirmation, passkey entry shows a random passkey
 *  - the link policy: reject Just-Works keys when the profile requires an
 *    authenticated link, drop links that never encrypt, throttle repeated
 *    failed pairings, and nudge the central to a short connection interval
 *
 * It has no dependency on furi: UI is reached through an event callback.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <esp_err.h>
#include <sdkconfig.h>

#if CONFIG_BT_NIMBLE_ENABLED
#include <host/ble_gap.h>
#else
#include <esp_gap_ble_api.h>
#include <esp_gatts_api.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    /** DisplayYesNo: numeric comparison, the user confirms the code on the Flipper. */
    BleCorePairingVerifyYesNo = 0,
    /** DisplayOnly: passkey entry, the Flipper shows a random passkey typed on the peer. */
    BleCorePairingDisplayOnly,
    /** InputOnly: the peer displays a passkey; this device must enter it. */
    BleCorePairingInputOnly,
} BleCorePairing;

typedef struct {
    BleCorePairing pairing;
    bool bonding;
    /** Ask the peer to encrypt this profile's link even without a persistent bond. */
    bool request_security;
    /** Explicitly request Just Works pairing with no passkey/MITM prompt. */
    bool just_works;
    /** Request LE Secure Connections when the profile's original policy permits it. */
    bool secure_connections;
    /** Drop links whose key is not MITM-authenticated (Just Works). */
    bool require_authenticated;
    /** Display Numeric Comparison but accept it automatically for serial-profile compatibility. */
    bool auto_confirm_numeric_comparison;
    /** Disconnect a link that is still not encrypted after this many ms (0 = never). */
    uint32_t link_auth_timeout_ms;
    /** After this many consecutive failed pairings refuse new links for a while (0 = never). */
    uint8_t fail_limit;
    /** Ask the central for a short connection interval after connect. */
    bool fast_link;
} BleCoreConfig;

typedef enum {
    BleCoreEventPinShow, /**< pin_code: passkey to type on the peer */
    BleCoreEventPinVerify, /**< pin_code: compare with the peer, answer via ble_core_pairing_confirm() */
    BleCoreEventPairingDone, /**< ok: pairing / encryption finished (hide any pairing UI) */
    /** The link is still unencrypted and the peer never started pairing, typically
     * because it still holds a bond this device no longer has (the stack answers
     * its LTK request negatively and reports nothing). The user must forget the
     * device on the peer. */
    BleCoreEventPairingStalled,
} BleCoreEventType;

typedef struct {
    BleCoreEventType type;
    uint32_t pin_code;
    bool ok;
} BleCoreEvent;

#if CONFIG_BT_NIMBLE_ENABLED

/** Called from the NimBLE host task; callbacks must not block. */
typedef void (*BleCoreEventCallback)(const BleCoreEvent* event, void* context);
typedef void (*BleCoreSecureCallback)(void* context);
typedef int (*BleCoreGapEventCallback)(struct ble_gap_event* event, void* context);

/** Prepare controller/host; services and callbacks may then be registered. */
esp_err_t ble_core_stack_prepare(void);
/** Start the host task and wait until controller/host synchronization. */
esp_err_t ble_core_stack_start(void);
/** Bring up both host and task for scan/advertising-only callers. */
esp_err_t ble_core_stack_up(void);
/** Stop and deinitialize host/controller after active profiles have stopped. */
esp_err_t ble_core_stack_down(void);
bool ble_core_stack_is_up(void);

void ble_core_set_event_callback(BleCoreEventCallback callback, void* context);
void ble_core_set_secure_callback(BleCoreSecureCallback callback, void* context);
esp_err_t ble_core_apply_config(const BleCoreConfig* config);
esp_err_t ble_core_register_gap_callback(BleCoreGapEventCallback callback, void* context);
void ble_core_restore_gap_callback(void);
void ble_core_forward_gap_event(struct ble_gap_event* event);
void ble_core_pairing_confirm(bool accept);
int ble_core_handle_gap_event(struct ble_gap_event* event);
bool ble_core_link_connected(uint16_t conn_handle, const struct ble_gap_conn_desc* desc);
void ble_core_link_disconnected(uint16_t conn_handle);
bool ble_core_mark_secure(void);
bool ble_core_link_is_secure(void);
bool ble_core_link_is_connected(void);
void ble_core_disconnect(void);
bool ble_core_remove_all_bonds(void);

#else

/** Called from the selected BLE host event context: must not block. */
typedef void (*BleCoreEventCallback)(const BleCoreEvent* event, void* context);

/** Called (BTC task) once per connection when the link becomes secure. */
typedef void (*BleCoreSecureCallback)(void* context);

/* ---- stack ---- */

/** Bring up controller + selected BLE host if they are not running. Idempotent. */
esp_err_t ble_core_stack_up(void);
bool ble_core_stack_is_up(void);

/* ---- pairing ---- */

void ble_core_set_event_callback(BleCoreEventCallback callback, void* context);
void ble_core_set_secure_callback(BleCoreSecureCallback callback, void* context);

/** Apply SMP parameters for the profile that is about to start. */
esp_err_t ble_core_apply_config(const BleCoreConfig* config);

/**
 * Install the active profile's GAP handler. NimBLE listeners coexist; the
 * Bluedroid fallback has one GAP callback, so temporary users must forward
 * events they do not own and restore the profile callback when done.
 */
esp_err_t ble_core_register_gap_callback(esp_gap_ble_cb_t callback);
/** Reinstall the handler from ble_core_register_gap_callback() (no-op if none). */
void ble_core_restore_gap_callback(void);
/** Hand a GAP event to the profile handler (no-op if none). */
void ble_core_forward_gap_event(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param);

/** Answer a BleCoreEventPinVerify request. Safe from any thread; no-op if nothing is pending. */
void ble_core_pairing_confirm(bool accept);

/**
 * Route a GAP event through the shared security handling. Call first from every
 * profile GAP handler. Returns true if the event was fully handled here.
 * ESP_GAP_BLE_AUTH_CMPL_EVT is processed but returns false so the profile can
 * still react (e.g. re-publish characteristics).
 */
bool ble_core_handle_gap_event(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param);

/* ---- link lifecycle (call from the GATTS connect/disconnect handlers) ---- */

/** @return false if the link was refused (pairing cooldown); a disconnect is already requested. */
bool ble_core_link_connected(const uint8_t bda[6], const esp_gatt_conn_params_t* params);
void ble_core_link_disconnected(void);

/**
 * Mark the current link as secure. Returns true on the false->true transition.
 * Only call when the stack itself enforced an authenticated permission, or from
 * the AUTH_CMPL policy in ble_core.
 */
bool ble_core_mark_secure(void);
bool ble_core_link_is_secure(void);
bool ble_core_link_is_connected(void);

/** Request disconnect of the current link (no-op if none). */
void ble_core_disconnect(void);

/* ---- bonds ---- */

/** Remove every bonded device. Needs the stack to be up. */
bool ble_core_remove_all_bonds(void);

#endif

#ifdef __cplusplus
}
#endif
