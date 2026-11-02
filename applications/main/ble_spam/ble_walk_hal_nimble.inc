/* NimBLE backend for the BLE Walk scanner and GATT client. */
#include <esp_attr.h>
#include <esp_log.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <furi.h>
#include <btshim.h>
#include <ble_core.h>
#include <host/ble_gap.h>
#include <host/ble_gatt.h>
#include <host/ble_hs.h>
#include <host/ble_hs_id.h>
#include <host/ble_hs_mbuf.h>
#include <host/ble_att.h>
#include <nimble/ble.h>
#include <os/os_mbuf.h>
#include <string.h>

#define TAG "BleWalkHal"
#define WALK_SCAN_INTERVAL 0x50
#define WALK_SCAN_WINDOW WALK_SCAN_INTERVAL
#define WALK_CONNECT_TIMEOUT_MS 15000

static EXT_RAM_BSS_ATTR BleWalkDevice s_devices[BLE_WALK_MAX_DEVICES];
static uint16_t s_device_count;
static volatile bool s_scanning;
static EXT_RAM_BSS_ATTR BleWalkService s_services[BLE_WALK_MAX_SERVICES];
static uint16_t s_service_count;
static volatile bool s_services_ready;
static EXT_RAM_BSS_ATTR BleWalkChar s_chars[BLE_WALK_MAX_CHARS];
static uint16_t s_char_count;
static volatile bool s_chars_ready;
static EXT_RAM_BSS_ATTR uint8_t s_read_buf[BLE_WALK_MAX_VALUE_LEN];
static uint16_t s_read_len;
static volatile bool s_read_ready;
static volatile uint8_t s_read_status;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_connected;
static volatile bool s_gattc_busy;
static volatile bool s_connect_complete;
static bool s_hal_started;

static void address_to_public_order(uint8_t out[6], const uint8_t nimble_addr[6]) {
    for(size_t i = 0; i < 6; ++i) out[i] = nimble_addr[5U - i];
}

static void address_to_nimble_order(uint8_t out[6], const uint8_t public_addr[6]) {
    for(size_t i = 0; i < 6; ++i) out[i] = public_addr[5U - i];
}

static bool uuid_from_nimble(BleUuid* out, const ble_uuid_t* uuid) {
    if(!out || !uuid) return false;
    memset(out, 0, sizeof(*out));
    switch(uuid->type) {
    case BLE_UUID_TYPE_16:
        out->len = ESP_UUID_LEN_16;
        out->uuid.uuid16 = ((const ble_uuid16_t*)uuid)->value;
        return true;
    case BLE_UUID_TYPE_32:
        out->len = ESP_UUID_LEN_32;
        out->uuid.uuid32 = ((const ble_uuid32_t*)uuid)->value;
        return true;
    case BLE_UUID_TYPE_128:
        out->len = ESP_UUID_LEN_128;
        memcpy(out->uuid.uuid128, ((const ble_uuid128_t*)uuid)->value, 16);
        return true;
    default:
        return false;
    }
}

static int device_find(const uint8_t addr[6]) {
    for(uint16_t i = 0; i < s_device_count; ++i) {
        if(memcmp(s_devices[i].addr, addr, 6) == 0) return (int)i;
    }
    return -1;
}

static void parse_name(const uint8_t* data, uint8_t data_len, char name[32]) {
    uint8_t offset = 0;
    while(data && offset < data_len) {
        const uint8_t ad_len = data[offset];
        if(ad_len == 0 || (uint16_t)offset + ad_len + 1U > data_len) break;
        const uint8_t ad_type = data[offset + 1U];
        if((ad_type == 0x08 || ad_type == 0x09) && ad_len > 1) {
            size_t name_len = ad_len - 1U;
            if(name_len > 31U) name_len = 31U;
            memcpy(name, &data[offset + 2U], name_len);
            name[name_len] = '\0';
            return;
        }
        offset = (uint8_t)(offset + ad_len + 1U);
    }
}

static void walk_store_report_data(
    const ble_addr_t* address,
    int8_t rssi,
    const uint8_t* data,
    uint8_t data_len,
    bool scan_response) {
    if(!address || !data || data_len == 0) return;
    uint8_t addr[6];
    address_to_public_order(addr, address->val);
    const int found = device_find(addr);
    int index = found;
    if(index < 0) {
        if(s_device_count >= BLE_WALK_MAX_DEVICES) return;
        index = s_device_count++;
        memset(&s_devices[index], 0, sizeof(s_devices[index]));
        memcpy(s_devices[index].addr, addr, sizeof(addr));
        s_devices[index].addr_type = address->type;
    }

    BleWalkDevice* device = &s_devices[index];
    device->rssi = rssi;
    char name[32] = {0};
    parse_name(data, data_len, name);
    if(name[0] != '\0') {
        strncpy(device->name, name, sizeof(device->name) - 1U);
        device->name[sizeof(device->name) - 1U] = '\0';
    }

    uint8_t* payload = scan_response ? device->scan_rsp_data : device->adv_data;
    uint8_t* payload_len = scan_response ? &device->scan_rsp_len : &device->adv_data_len;
    size_t copy_len = data_len;
    if(copy_len > 31U) copy_len = 31U;
    memcpy(payload, data, copy_len);
    *payload_len = (uint8_t)copy_len;
}

static void walk_store_report(const struct ble_gap_disc_desc* disc) {
    if(!disc) return;
    walk_store_report_data(
        &disc->addr,
        disc->rssi,
        disc->data,
        disc->length_data,
        disc->event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP);
}

#if CONFIG_BT_NIMBLE_EXT_ADV
static void walk_store_ext_report(const struct ble_gap_ext_disc_desc* disc) {
    if(!disc || disc->data_status == BLE_GAP_EXT_ADV_DATA_STATUS_INCOMPLETE) return;
    const bool legacy_scan_response =
        (disc->props & BLE_HCI_ADV_LEGACY_MASK) &&
        disc->legacy_event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP;
    walk_store_report_data(
        &disc->addr,
        disc->rssi,
        disc->data,
        disc->length_data,
        legacy_scan_response);
}
#endif

static int walk_scan_event(struct ble_gap_event* event, void* context) {
    (void)context;
    if(!event) return 0;
    switch(event->type) {
    case BLE_GAP_EVENT_DISC:
        walk_store_report(&event->disc);
        break;
#if CONFIG_BT_NIMBLE_EXT_ADV
    case BLE_GAP_EVENT_EXT_DISC:
        walk_store_ext_report(&event->ext_disc);
        break;
#endif
    case BLE_GAP_EVENT_DISC_COMPLETE:
        s_scanning = false;
        break;
    default:
        break;
    }
    return 0;
}

static int walk_start_scan(void) {
    if(!s_hal_started) return BLE_HS_EINVAL;
    if(s_scanning) return 0;
    if(s_connected || s_gattc_busy) return BLE_HS_EBUSY;
    uint8_t own_addr_type = BLE_OWN_ADDR_PUBLIC;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if(rc != 0) return rc;
    const struct ble_gap_disc_params params = {
        .itvl = WALK_SCAN_INTERVAL,
        .window = WALK_SCAN_WINDOW,
        .filter_policy = BLE_HCI_SCAN_FILT_NO_WL,
        .limited = 0,
        .passive = 0,
        .filter_duplicates = 0,
    };
    rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &params, walk_scan_event, NULL);
    if(rc == 0) s_scanning = true;
    return rc;
}

static int walk_stop_scan(void) {
    if(!s_scanning) return 0;
    int rc = ble_gap_disc_cancel();
    if(rc == 0 || rc == BLE_HS_EALREADY) s_scanning = false;
    return rc;
}

static int walk_connect_event(struct ble_gap_event* event, void* context) {
    (void)context;
    if(!event) return 0;
    if(event->type == BLE_GAP_EVENT_CONNECT) {
        s_connect_complete = true;
        s_gattc_busy = false;
        if(event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_connected = true;
            int rc = ble_gattc_exchange_mtu(s_conn_handle, NULL, NULL);
            if(rc != 0 && rc != BLE_HS_EALREADY) {
                ESP_LOGW(TAG, "MTU exchange request failed: %d", rc);
            }
        } else {
            s_connected = false;
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            ESP_LOGW(TAG, "GATT connection failed: %d", event->connect.status);
        }
    } else if(event->type == BLE_GAP_EVENT_DISCONNECT) {
        s_connected = false;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_gattc_busy = false;
        s_connect_complete = true;
    }
    return 0;
}

static int walk_services_cb(uint16_t conn_handle, const struct ble_gatt_error* error,
                            const struct ble_gatt_svc* service, void* context) {
    (void)conn_handle;
    (void)context;
    if(error && error->status == 0 && service) {
        if(s_service_count < BLE_WALK_MAX_SERVICES) {
            BleWalkService* out = &s_services[s_service_count++];
            if(!uuid_from_nimble(&out->uuid, &service->uuid.u)) return BLE_ATT_ERR_UNLIKELY;
            out->start_handle = service->start_handle;
            out->end_handle = service->end_handle;
        }
    } else if(error && (error->status == BLE_HS_EDONE || error->status != 0)) {
        if(error->status != BLE_HS_EDONE) ESP_LOGW(TAG, "service discovery ended: %u", error->status);
        s_services_ready = true;
    }
    return 0;
}

static int walk_chars_cb(uint16_t conn_handle, const struct ble_gatt_error* error,
                         const struct ble_gatt_chr* chr, void* context) {
    (void)conn_handle;
    (void)context;
    if(error && error->status == 0 && chr) {
        if(s_char_count < BLE_WALK_MAX_CHARS) {
            BleWalkChar* out = &s_chars[s_char_count++];
            if(!uuid_from_nimble(&out->uuid, &chr->uuid.u)) return BLE_ATT_ERR_UNLIKELY;
            out->handle = chr->val_handle;
            out->properties = chr->properties;
        }
    } else if(error && (error->status == BLE_HS_EDONE || error->status != 0)) {
        if(error->status != BLE_HS_EDONE) ESP_LOGW(TAG, "characteristic discovery ended: %u", error->status);
        s_chars_ready = true;
    }
    return 0;
}

static int walk_read_cb(uint16_t conn_handle, const struct ble_gatt_error* error,
                        struct ble_gatt_attr* attr, void* context) {
    (void)conn_handle;
    (void)context;
    s_read_len = 0;
    s_read_status = error ? (uint8_t)error->status : (uint8_t)BLE_HS_EUNKNOWN;
    if(error && error->status == 0 && attr && attr->om) {
        uint16_t len = OS_MBUF_PKTLEN(attr->om);
        if(len > BLE_WALK_MAX_VALUE_LEN) len = BLE_WALK_MAX_VALUE_LEN;
        if(os_mbuf_copydata(attr->om, 0, len, s_read_buf) == 0) {
            s_read_len = len;
            s_read_status = 0;
        } else {
            s_read_status = (uint8_t)BLE_HS_EUNKNOWN;
        }
    }
    s_read_ready = true;
    return 0;
}

static int walk_write_cb(uint16_t conn_handle, const struct ble_gatt_error* error,
                         struct ble_gatt_attr* attr, void* context) {
    (void)conn_handle;
    (void)attr;
    (void)context;
    if(error && error->status != 0) ESP_LOGW(TAG, "GATT write failed: %u", error->status);
    return 0;
}

bool ble_walk_hal_start(void) {
    if(s_hal_started) return true;
    Bt* bt = furi_record_open(RECORD_BT);
    bt_stop_stack(bt);
    furi_record_close(RECORD_BT);
    furi_delay_ms(100);

    if(ble_core_stack_is_up()) {
        ESP_LOGE(TAG, "refusing GATT client takeover while btshim still owns the NimBLE host");
        return false;
    }

    s_device_count = 0;
    s_scanning = false;
    s_services_ready = false;
    s_chars_ready = false;
    s_read_ready = false;
    s_connected = false;
    s_gattc_busy = false;
    s_connect_complete = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;

    esp_err_t err = ble_core_stack_up();
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE stack start failed: %s", esp_err_to_name(err));
        (void)ble_core_stack_down();
        Bt* restore = furi_record_open(RECORD_BT);
        bt_start_stack(restore);
        furi_record_close(RECORD_BT);
        return false;
    }
    (void)ble_att_set_preferred_mtu(200);
    s_hal_started = true;
    return true;
}

void ble_walk_hal_stop(void) {
    if(!s_hal_started) return;
    (void)walk_stop_scan();
    if(s_connected && s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        int rc = ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        if(rc != 0 && rc != BLE_HS_ENOTCONN) ESP_LOGW(TAG, "disconnect failed: %d", rc);
        for(int i = 0; i < 40 && s_connected; ++i) furi_delay_ms(50);
    }
    esp_err_t err = ble_core_stack_down();
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE stack teardown failed: %s", esp_err_to_name(err));
        return;
    }
    s_hal_started = false;
    s_connected = false;
    s_scanning = false;
    s_gattc_busy = false;
    Bt* bt = furi_record_open(RECORD_BT);
    bt_start_stack(bt);
    furi_record_close(RECORD_BT);
}

bool ble_walk_hal_start_scan(void) {
    if(!s_hal_started || s_scanning) return s_scanning;
    const int rc = walk_start_scan();
    if(rc != 0) ESP_LOGE(TAG, "NimBLE scan start failed: %d", rc);
    return rc == 0;
}

void ble_walk_hal_stop_scan(void) {
    const int rc = walk_stop_scan();
    if(rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGW(TAG, "NimBLE scan stop failed: %d", rc);
}

bool ble_walk_hal_is_scanning(void) { return s_scanning; }

BleWalkDevice* ble_walk_hal_get_devices(uint16_t* count) {
    if(count) *count = s_device_count;
    return s_devices;
}

bool ble_walk_hal_connect(BleWalkDevice* device, volatile bool* abort_flag) {
    if(!s_hal_started || !device || s_connected || s_gattc_busy) return false;
    const int stop_rc = walk_stop_scan();
    if(stop_rc != 0 && stop_rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "scan stop before connect failed: %d", stop_rc);
        return false;
    }
    for(int i = 0; i < 40 && s_scanning; ++i) furi_delay_ms(5);
    if(s_scanning) {
        ESP_LOGW(TAG, "scan did not stop before connect");
        return false;
    }
    s_services_ready = false;
    s_service_count = 0;
    s_chars_ready = false;
    s_char_count = 0;
    s_connect_complete = false;
    s_gattc_busy = true;
    uint8_t own_addr_type = BLE_OWN_ADDR_PUBLIC;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if(rc != 0) {
        s_gattc_busy = false;
        return false;
    }
    ble_addr_t peer = {.type = device->addr_type};
    address_to_nimble_order(peer.val, device->addr);
    ESP_LOGI(TAG, "Connecting to %02X:%02X:%02X:%02X:%02X:%02X (type=%u, name='%s')",
             device->addr[0], device->addr[1], device->addr[2], device->addr[3],
             device->addr[4], device->addr[5], device->addr_type, device->name);
    rc = ble_gap_connect(
        own_addr_type, &peer, WALK_CONNECT_TIMEOUT_MS, NULL, walk_connect_event, NULL);
    if(rc != 0) {
        s_gattc_busy = false;
        ESP_LOGW(TAG, "connect request failed: %d", rc);
        if(!abort_flag || !*abort_flag) (void)walk_start_scan();
        return false;
    }

    bool aborted = false;
    for(int i = 0; i < WALK_CONNECT_TIMEOUT_MS / 50 + 20 && !s_connect_complete; ++i) {
        if(abort_flag && *abort_flag) {
            aborted = true;
            (void)ble_gap_conn_cancel();
            break;
        }
        furi_delay_ms(50);
    }
    if(!s_connect_complete) {
        (void)ble_gap_conn_cancel();
        for(int i = 0; i < 60 && !s_connect_complete; ++i) furi_delay_ms(50);
    }
    if(!s_connected && !s_gattc_busy && (!abort_flag || !*abort_flag)) {
        (void)walk_start_scan();
    }
    if(aborted) ESP_LOGI(TAG, "connection attempt cancelled");
    else if(!s_connected) ESP_LOGW(TAG, "connection attempt timed out or failed");
    return s_connected;
}

void ble_walk_hal_disconnect(void) {
    if(s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    int rc = ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    if(rc != 0 && rc != BLE_HS_ENOTCONN) ESP_LOGW(TAG, "disconnect request failed: %d", rc);
    for(int i = 0; i < 40 && s_connected; ++i) furi_delay_ms(50);
}

bool ble_walk_hal_is_connected(void) { return s_connected; }

bool ble_walk_hal_discover_services(void) {
    if(!s_connected) return false;
    s_service_count = 0;
    s_services_ready = false;
    const int rc = ble_gattc_disc_all_svcs(s_conn_handle, walk_services_cb, NULL);
    if(rc != 0) {
        ESP_LOGE(TAG, "service discovery start failed: %d", rc);
        s_services_ready = true;
        return false;
    }
    return true;
}

bool ble_walk_hal_services_ready(void) { return s_services_ready; }

BleWalkService* ble_walk_hal_get_services(uint16_t* count) {
    if(count) *count = s_service_count;
    return s_services;
}

bool ble_walk_hal_discover_chars(BleWalkService* service) {
    if(!s_connected || !service || service->start_handle > service->end_handle) return false;
    s_char_count = 0;
    s_chars_ready = false;
    const int rc = ble_gattc_disc_all_chrs(
        s_conn_handle, service->start_handle, service->end_handle, walk_chars_cb, NULL);
    if(rc != 0) {
        ESP_LOGE(TAG, "characteristic discovery start failed: %d", rc);
        s_chars_ready = true;
        return false;
    }
    return true;
}

bool ble_walk_hal_chars_ready(void) { return s_chars_ready; }

BleWalkChar* ble_walk_hal_get_chars(uint16_t* count) {
    if(count) *count = s_char_count;
    return s_chars;
}

bool ble_walk_hal_read_char(uint16_t handle) {
    if(!s_connected || handle == 0) return false;
    s_read_ready = false;
    s_read_len = 0;
    s_read_status = 0;
    const int rc = ble_gattc_read(s_conn_handle, handle, walk_read_cb, NULL);
    if(rc != 0) {
        ESP_LOGE(TAG, "GATT read start failed: %d", rc);
        return false;
    }
    return true;
}

bool ble_walk_hal_read_ready(void) { return s_read_ready; }

uint8_t* ble_walk_hal_get_read_value(uint16_t* len) {
    if(len) *len = s_read_len;
    return s_read_buf;
}

uint8_t ble_walk_hal_get_read_status(void) { return s_read_status; }

bool ble_walk_hal_write_char(uint16_t handle, const uint8_t* data, uint16_t len) {
    if(!s_connected || handle == 0 || (!data && len > 0)) return false;
    const int rc = ble_gattc_write_flat(s_conn_handle, handle, data, len, walk_write_cb, NULL);
    if(rc != 0) ESP_LOGE(TAG, "GATT write start failed: %d", rc);
    return rc == 0;
}
