/**
 * NimBLE host lifecycle and shared configuration for ESP-IDF 5.4.1.
 *
 * Profiles prepare the host, register their GATT services and callbacks, then
 * start the host task. This ordering is required by the IDF NimBLE HID device
 * backend, which installs host callbacks before the host task is started.
 */
#pragma once

#include <sdkconfig.h>

#if CONFIG_BT_NIMBLE_ENABLED

#include <stdbool.h>
#include <stdint.h>

#include <esp_err.h>
#include <host/ble_gap.h>
#include <host/ble_hs_adv.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t io_capability;
    bool bonding;
    bool mitm;
    bool secure_connections;
    bool secure_connections_only;
    uint8_t our_key_distribution;
    uint8_t peer_key_distribution;
} BleNimbleHostSecurityConfig;

/** Initialize NVS, controller, NimBLE transport and host, but do not run task. */
esp_err_t ble_nimble_host_prepare(void);

/** Apply host-wide pairing parameters. Call after prepare and before start. */
esp_err_t ble_nimble_host_set_security(const BleNimbleHostSecurityConfig* config);

/** Start the NimBLE host task and wait for the host/controller sync callback. */
esp_err_t ble_nimble_host_start(void);

/** Stop the host task and deinitialize NimBLE and the ESP controller. */
esp_err_t ble_nimble_host_down(void);

bool ble_nimble_host_is_prepared(void);
bool ble_nimble_host_is_running(void);
bool ble_nimble_host_is_synced(void);

/** Add or remove an independent NimBLE GAP listener. */
int ble_nimble_host_add_gap_listener(
    struct ble_gap_event_listener* listener,
    ble_gap_event_fn* callback,
    void* context);
int ble_nimble_host_remove_gap_listener(struct ble_gap_event_listener* listener);

/** Set a static random identity address while the radio is idle. */
int ble_nimble_host_set_random_address(const uint8_t address[6]);

/** Start connectable legacy-PDU advertising through NimBLE's extended API. */
int ble_nimble_host_start_legacy_advertising(
    uint8_t instance,
    uint8_t own_addr_type,
    uint16_t interval_min,
    uint16_t interval_max,
    const struct ble_hs_adv_fields* fields,
    ble_gap_event_fn* callback,
    void* context);

/** Stop an active advertising instance; succeeds if it is already stopped. */
int ble_nimble_host_stop_advertising(uint8_t instance);

/** Remove peer security records, CCCDs and peer records from NimBLE's store. */
bool ble_nimble_host_remove_all_bonds(void);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_BT_NIMBLE_ENABLED */
