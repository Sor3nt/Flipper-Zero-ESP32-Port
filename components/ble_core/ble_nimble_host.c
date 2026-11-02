/**
 * Shared NimBLE host lifecycle for this firmware.
 *
 * IDF 5.4.1's nimble_port_init() initializes the ESP controller and NimBLE
 * host. GATT services and host callbacks are configured after prepare() and
 * before start(), following Espressif's NimBLE peripheral and HID examples.
 */
#include "ble_nimble_host.h"

#if CONFIG_BT_NIMBLE_ENABLED

#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <esp_log.h>
#include <nvs_flash.h>
#include <host/ble_hs_adv.h>
#include <host/ble_hs.h>
#include <host/ble_hs_id.h>
#include <host/ble_sm.h>
#include <host/ble_store.h>
#if CONFIG_BT_NIMBLE_NVS_PERSIST
#include <store/config/ble_store_config.h>
extern void ble_store_config_init(void);
#endif
#include <host/util/util.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <os/os_mbuf.h>

#define TAG "ble_nimble_host"
#define BLE_NIMBLE_SYNC_TIMEOUT_MS 5000U
#define BLE_NIMBLE_ADV_INSTANCE_COUNT CONFIG_BT_NIMBLE_MAX_EXT_ADV_INSTANCES

typedef enum {
    BleNimbleHostStateDown,
    BleNimbleHostStatePrepared,
    BleNimbleHostStateStarting,
    BleNimbleHostStateRunning,
    BleNimbleHostStateStopping,
} BleNimbleHostState;

static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_lifecycle_mutex;
static SemaphoreHandle_t s_sync_sem;
static BleNimbleHostState s_state = BleNimbleHostStateDown;
static bool s_nvs_initialized;
static bool s_random_address_pending;
static uint8_t s_random_address[6];
static esp_err_t s_sync_status = ESP_ERR_INVALID_STATE;

static void ble_nimble_host_task(void* context);
static void ble_nimble_host_sync_cb(void);
static void ble_nimble_host_reset_cb(int reason);

static bool ble_nimble_host_state_is(BleNimbleHostState state) {
    bool matches;
    portENTER_CRITICAL(&s_state_lock);
    matches = s_state == state;
    portEXIT_CRITICAL(&s_state_lock);
    return matches;
}

static void ble_nimble_host_set_state(BleNimbleHostState state) {
    portENTER_CRITICAL(&s_state_lock);
    s_state = state;
    portEXIT_CRITICAL(&s_state_lock);
}

static BleNimbleHostState ble_nimble_host_get_state(void) {
    BleNimbleHostState state;
    portENTER_CRITICAL(&s_state_lock);
    state = s_state;
    portEXIT_CRITICAL(&s_state_lock);
    return state;
}

static esp_err_t ble_nimble_host_create_mutex(void) {
    if(s_lifecycle_mutex) return ESP_OK;

    SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    if(!mutex) return ESP_ERR_NO_MEM;

    portENTER_CRITICAL(&s_state_lock);
    if(!s_lifecycle_mutex) {
        s_lifecycle_mutex = mutex;
        mutex = NULL;
    }
    portEXIT_CRITICAL(&s_state_lock);

    if(mutex) vSemaphoreDelete(mutex);
    return ESP_OK;
}

static void ble_nimble_host_task(void* context) {
    (void)context;
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    ESP_LOGI(TAG, "NimBLE host task stopped");
    nimble_port_freertos_deinit();
}

static void ble_nimble_host_sync_cb(void) {
    uint8_t own_addr_type = BLE_OWN_ADDR_PUBLIC;
    int rc = 0;
    if(s_random_address_pending) {
        rc = ble_hs_id_set_rnd(s_random_address);
        if(rc == 0) s_random_address_pending = false;
    }
    if(rc == 0) {
        rc = ble_hs_util_ensure_addr(0);
        if(rc != 0) rc = ble_hs_util_ensure_addr(1);
    }
    if(rc == 0) rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if(rc != 0) {
        ESP_LOGE(TAG, "failed to establish NimBLE identity address: %d", rc);
        portENTER_CRITICAL(&s_state_lock);
        s_sync_status = ESP_FAIL;
        portEXIT_CRITICAL(&s_state_lock);
        if(s_sync_sem) xSemaphoreGive(s_sync_sem);
        return;
    }

    portENTER_CRITICAL(&s_state_lock);
    s_sync_status = ESP_OK;
    portEXIT_CRITICAL(&s_state_lock);
    ble_nimble_host_set_state(BleNimbleHostStateRunning);
    if(s_sync_sem) xSemaphoreGive(s_sync_sem);
}

static void ble_nimble_host_reset_cb(int reason) {
    ESP_LOGE(TAG, "NimBLE host reset: reason=%d", reason);
    portENTER_CRITICAL(&s_state_lock);
    if(s_state == BleNimbleHostStateRunning) s_state = BleNimbleHostStateStarting;
    portEXIT_CRITICAL(&s_state_lock);
}

esp_err_t ble_nimble_host_prepare(void) {
    esp_err_t err = ble_nimble_host_create_mutex();
    if(err != ESP_OK) return err;
    if(xSemaphoreTake(s_lifecycle_mutex, portMAX_DELAY) != pdTRUE) return ESP_FAIL;

    if(ble_nimble_host_get_state() != BleNimbleHostStateDown) {
        xSemaphoreGive(s_lifecycle_mutex);
        return ESP_OK;
    }

    if(!s_sync_sem) {
        s_sync_sem = xSemaphoreCreateBinary();
        if(!s_sync_sem) {
            xSemaphoreGive(s_lifecycle_mutex);
            return ESP_ERR_NO_MEM;
        }
    }

    if(!s_nvs_initialized) {
        err = nvs_flash_init();
        if(err == ESP_ERR_INVALID_STATE) err = ESP_OK;
        if(err != ESP_OK) {
            ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
            xSemaphoreGive(s_lifecycle_mutex);
            return err;
        }
        s_nvs_initialized = true;
    }

    err = nimble_port_init();
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        xSemaphoreGive(s_lifecycle_mutex);
        return err;
    }

#if CONFIG_BT_NIMBLE_NVS_PERSIST
    ble_store_config_init();
#endif

    ble_hs_cfg.gatts_register_cb = NULL;
    ble_hs_cfg.gatts_register_arg = NULL;
    ble_hs_cfg.reset_cb = ble_nimble_host_reset_cb;
    ble_hs_cfg.sync_cb = ble_nimble_host_sync_cb;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    portENTER_CRITICAL(&s_state_lock);
    s_sync_status = ESP_ERR_INVALID_STATE;
    portEXIT_CRITICAL(&s_state_lock);

    while(xSemaphoreTake(s_sync_sem, 0) == pdTRUE) {
        /* Drain an old sync notification before reinitializing the host. */
    }

    ble_nimble_host_set_state(BleNimbleHostStatePrepared);
    xSemaphoreGive(s_lifecycle_mutex);
    return ESP_OK;
}

esp_err_t ble_nimble_host_set_security(const BleNimbleHostSecurityConfig* config) {
    if(!config) return ESP_ERR_INVALID_ARG;
    if(!ble_nimble_host_state_is(BleNimbleHostStatePrepared)) return ESP_ERR_INVALID_STATE;

    ble_hs_cfg.sm_io_cap = config->io_capability;
    ble_hs_cfg.sm_bonding = config->bonding;
    ble_hs_cfg.sm_mitm = config->mitm;
    ble_hs_cfg.sm_sc = config->secure_connections;
    ble_hs_cfg.sm_sc_only = config->secure_connections_only;
    ble_hs_cfg.sm_our_key_dist = config->our_key_distribution;
    ble_hs_cfg.sm_their_key_dist = config->peer_key_distribution;
    return ESP_OK;
}

esp_err_t ble_nimble_host_start(void) {
    esp_err_t err = ble_nimble_host_prepare();
    if(err != ESP_OK) return err;
    if(xSemaphoreTake(s_lifecycle_mutex, portMAX_DELAY) != pdTRUE) return ESP_FAIL;

    BleNimbleHostState state = ble_nimble_host_get_state();
    if(state == BleNimbleHostStateRunning) {
        xSemaphoreGive(s_lifecycle_mutex);
        return ESP_OK;
    }
    if(state != BleNimbleHostStatePrepared) {
        xSemaphoreGive(s_lifecycle_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    ble_nimble_host_set_state(BleNimbleHostStateStarting);
    nimble_port_freertos_init(ble_nimble_host_task);
    xSemaphoreGive(s_lifecycle_mutex);

    if(xSemaphoreTake(s_sync_sem, pdMS_TO_TICKS(BLE_NIMBLE_SYNC_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "NimBLE host did not synchronize within %u ms", BLE_NIMBLE_SYNC_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }
    portENTER_CRITICAL(&s_state_lock);
    err = s_sync_status;
    portEXIT_CRITICAL(&s_state_lock);
    if(err != ESP_OK) return err;
    return ble_nimble_host_state_is(BleNimbleHostStateRunning) ? ESP_OK : ESP_FAIL;
}

esp_err_t ble_nimble_host_down(void) {
    esp_err_t err = ble_nimble_host_create_mutex();
    if(err != ESP_OK) return err;
    if(xSemaphoreTake(s_lifecycle_mutex, portMAX_DELAY) != pdTRUE) return ESP_FAIL;

    BleNimbleHostState state = ble_nimble_host_get_state();
    if(state == BleNimbleHostStateDown) {
        xSemaphoreGive(s_lifecycle_mutex);
        return ESP_OK;
    }

    bool started = state == BleNimbleHostStateRunning || state == BleNimbleHostStateStarting;
    ble_nimble_host_set_state(BleNimbleHostStateStopping);
    xSemaphoreGive(s_lifecycle_mutex);

    if(started) {
        int rc = nimble_port_stop();
        if(rc != 0) {
            ESP_LOGE(TAG, "nimble_port_stop failed: %d", rc);
            ble_nimble_host_set_state(BleNimbleHostStateRunning);
            return ESP_FAIL;
        }
    }

    err = nimble_port_deinit();
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_deinit failed: %s", esp_err_to_name(err));
        return err;
    }

    ble_nimble_host_set_state(BleNimbleHostStateDown);
    s_random_address_pending = false;
    return ESP_OK;
}

bool ble_nimble_host_is_prepared(void) {
    return ble_nimble_host_state_is(BleNimbleHostStatePrepared) ||
           ble_nimble_host_state_is(BleNimbleHostStateStarting) ||
           ble_nimble_host_state_is(BleNimbleHostStateRunning);
}

bool ble_nimble_host_is_running(void) {
    return ble_nimble_host_state_is(BleNimbleHostStateRunning) ||
           ble_nimble_host_state_is(BleNimbleHostStateStarting);
}

bool ble_nimble_host_is_synced(void) {
    return ble_nimble_host_state_is(BleNimbleHostStateRunning);
}

int ble_nimble_host_add_gap_listener(
    struct ble_gap_event_listener* listener,
    ble_gap_event_fn* callback,
    void* context) {
    if(!listener || !callback || !ble_nimble_host_is_prepared()) return BLE_HS_EINVAL;
    return ble_gap_event_listener_register(listener, callback, context);
}

int ble_nimble_host_remove_gap_listener(struct ble_gap_event_listener* listener) {
    if(!listener || !ble_nimble_host_is_prepared()) return BLE_HS_EINVAL;
    return ble_gap_event_listener_unregister(listener);
}

int ble_nimble_host_set_random_address(const uint8_t address[6]) {
    if(!address || !ble_nimble_host_is_prepared()) return BLE_HS_EINVAL;
    /* Public firmware addresses use display order; NimBLE stores addresses
     * least-significant octet first. Convert and set the static-random bits on
     * NimBLE's most-significant octet. */
    for(size_t i = 0; i < sizeof(s_random_address); ++i) {
        s_random_address[i] = address[sizeof(s_random_address) - 1U - i];
    }
    s_random_address[5] = (s_random_address[5] & 0x3FU) | 0xC0U;
    if(!ble_nimble_host_is_synced()) {
        s_random_address_pending = true;
        return 0;
    }
    int rc = ble_hs_id_set_rnd(s_random_address);
    if(rc == 0) s_random_address_pending = false;
    return rc;
}

int ble_nimble_host_start_legacy_advertising(
    uint8_t instance,
    uint8_t own_addr_type,
    uint16_t interval_min,
    uint16_t interval_max,
    const struct ble_hs_adv_fields* fields,
    ble_gap_event_fn* callback,
    void* context) {
#if CONFIG_BT_NIMBLE_EXT_ADV
    if(!fields || !callback || interval_min < 0x20U || interval_max < interval_min ||
       interval_max > 0x4000U || instance >= BLE_NIMBLE_ADV_INSTANCE_COUNT) {
        return BLE_HS_EINVAL;
    }

    uint8_t adv_data[BLE_HS_ADV_MAX_SZ];
    uint8_t adv_data_len = 0;
    int rc = ble_hs_adv_set_fields(fields, adv_data, &adv_data_len, sizeof(adv_data));
    if(rc != 0) return rc;

    const struct ble_gap_ext_adv_params params = {
        .connectable = 1,
        .scannable = 1,
        .legacy_pdu = 1,
        .own_addr_type = own_addr_type,
        .primary_phy = BLE_HCI_LE_PHY_1M,
        .secondary_phy = BLE_HCI_LE_PHY_1M,
        .sid = 0,
        .itvl_min = interval_min,
        .itvl_max = interval_max,
        .tx_power = 127,
    };
    rc = ble_gap_ext_adv_configure(instance, &params, NULL, callback, context);
    if(rc != 0) return rc;

    struct os_mbuf* data = os_msys_get_pkthdr(adv_data_len, 0);
    if(!data) return BLE_HS_ENOMEM;
    if(os_mbuf_append(data, adv_data, adv_data_len) != 0) {
        os_mbuf_free_chain(data);
        return BLE_HS_ENOMEM;
    }
    rc = ble_gap_ext_adv_set_data(instance, data);
    if(rc != 0) return rc;
    return ble_gap_ext_adv_start(instance, 0, 0);
#else
    (void)instance;
    (void)own_addr_type;
    (void)interval_min;
    (void)interval_max;
    (void)fields;
    (void)callback;
    (void)context;
    return BLE_HS_ENOTSUP;
#endif
}

int ble_nimble_host_stop_advertising(uint8_t instance) {
#if CONFIG_BT_NIMBLE_EXT_ADV
    if(instance >= BLE_NIMBLE_ADV_INSTANCE_COUNT) return BLE_HS_EINVAL;
    if(!ble_gap_ext_adv_active(instance)) return 0;
    int rc = ble_gap_ext_adv_stop(instance);
    return rc == BLE_HS_EALREADY ? 0 : rc;
#else
    (void)instance;
    return BLE_HS_ENOTSUP;
#endif
}

bool ble_nimble_host_remove_all_bonds(void) {
    if(!ble_nimble_host_is_prepared()) return false;

#if CONFIG_BT_NIMBLE_MAX_BONDS > 0
    ble_addr_t peer_addresses[CONFIG_BT_NIMBLE_MAX_BONDS];
    int peer_count = 0;
    int rc = ble_store_util_bonded_peers(
        peer_addresses, &peer_count, CONFIG_BT_NIMBLE_MAX_BONDS);
    if(rc != 0) {
        ESP_LOGE(TAG, "failed to enumerate NimBLE bonds: %d", rc);
        return false;
    }

    for(int i = 0; i < peer_count; ++i) {
        rc = ble_store_util_delete_peer(&peer_addresses[i]);
        if(rc != 0) {
            ESP_LOGE(TAG, "failed to remove NimBLE bond %d: %d", i, rc);
            return false;
        }
    }
#endif
    return true;
}

#endif /* CONFIG_BT_NIMBLE_ENABLED */
