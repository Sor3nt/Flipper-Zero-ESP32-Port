/** NimBLE GATT implementation of the Flipper serial service. */
#include "ble_serial.h"
#include <sdkconfig.h>

#if CONFIG_BT_NIMBLE_ENABLED

#include <string.h>
#include <stdlib.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <furi_ble/gap.h>
#include <furi_hal_version.h>
#include <host/ble_att.h>
#include <host/ble_gatt.h>
#include <host/ble_gap.h>
#include <host/ble_hs.h>
#include <host/ble_hs_adv.h>
#include <host/ble_hs_id.h>
#include <host/ble_uuid.h>
#include <os/os_mbuf.h>
#include <services/gap/ble_svc_gap.h>
#include <services/gatt/ble_svc_gatt.h>
#include <protobuf_version.h>

#include <ble_core.h>
#include <ble_nimble_host.h>

#define TAG "ble_serial"
#define SERIAL_ADV_MIN_INTERVAL 0x20
#define SERIAL_ADV_MAX_INTERVAL 0x40
#define SERIAL_ACCESS_BUFFER BLE_SVC_SERIAL_DATA_LEN_MAX
#define SERIAL_AUTHENTICATED_READ \
    (BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_READ_AUTHEN)
#define SERIAL_AUTHENTICATED_WRITE \
    (BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_READ_AUTHEN | \
     BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC | \
     BLE_GATT_CHR_F_WRITE_AUTHEN)

#define SERIAL_STR_HELPER(x) #x
#define SERIAL_STR(x) SERIAL_STR_HELPER(x)

extern void furi_hal_bt_emit_gap_event(GapEvent event);

static const ble_uuid128_t serial_svc_uuid = BLE_UUID128_INIT(
    0x00, 0x00, 0xFE, 0x60, 0xCC, 0x7A, 0x48, 0x2A,
    0x98, 0x4A, 0x7F, 0x2E, 0xD5, 0xB3, 0xE5, 0x8F);
static const ble_uuid128_t serial_tx_uuid = BLE_UUID128_INIT(
    0x00, 0x00, 0xFE, 0x61, 0x8E, 0x22, 0x45, 0x41,
    0x9D, 0x4C, 0x21, 0xED, 0xAE, 0x82, 0xED, 0x19);
static const ble_uuid128_t serial_rx_uuid = BLE_UUID128_INIT(
    0x00, 0x00, 0xFE, 0x62, 0x8E, 0x22, 0x45, 0x41,
    0x9D, 0x4C, 0x21, 0xED, 0xAE, 0x82, 0xED, 0x19);
static const ble_uuid128_t serial_flow_uuid = BLE_UUID128_INIT(
    0x00, 0x00, 0xFE, 0x63, 0x8E, 0x22, 0x45, 0x41,
    0x9D, 0x4C, 0x21, 0xED, 0xAE, 0x82, 0xED, 0x19);
static const ble_uuid128_t serial_rpc_uuid = BLE_UUID128_INIT(
    0x00, 0x00, 0xFE, 0x64, 0x8E, 0x22, 0x45, 0x41,
    0x9D, 0x4C, 0x21, 0xED, 0xAE, 0x82, 0xED, 0x19);
static const ble_uuid128_t dis_api_uuid = BLE_UUID128_INIT(
    0x33, 0xA9, 0xB5, 0x3E, 0x87, 0x5D, 0x1A, 0x8E,
    0xC8, 0x47, 0x5E, 0xAE, 0x6D, 0x66, 0xF6, 0x03);
static const ble_uuid16_t dis_svc_uuid = BLE_UUID16_INIT(0x180A);
static const ble_uuid16_t dis_manufacturer_uuid = BLE_UUID16_INIT(0x2A29);
static const ble_uuid16_t dis_hw_uuid = BLE_UUID16_INIT(0x2A27);
static const ble_uuid16_t dis_sw_uuid = BLE_UUID16_INIT(0x2A28);
static const ble_uuid16_t bas_svc_uuid = BLE_UUID16_INIT(0x180F);
static const ble_uuid16_t bas_level_uuid = BLE_UUID16_INIT(0x2A19);

static const char dis_manufacturer[] = "Flipper Devices Inc.";
static const char dis_hw_revision[] = "ESP32-C6 1.0";
static const char dis_sw_revision[] = "1.4.3";
static const char dis_api_version[] =
    SERIAL_STR(PROTOBUF_MAJOR_VERSION) "." SERIAL_STR(PROTOBUF_MINOR_VERSION);

typedef struct BleSerial {
    SemaphoreHandle_t mutex;
    SemaphoreHandle_t callbacks_drained;
    uint32_t callback_refs;
    BleSerialConfig config;
    bool connected;
    uint16_t conn_handle;
    SerialServiceEventCallback event_callback;
    void* event_context;
    uint16_t buff_size;
    uint16_t bytes_ready;
    BleSerialStateCallback state_callback;
    void* state_context;
} BleSerial;

static struct {
    SemaphoreHandle_t mutex;
    BleSerial* active;
    bool initialized;
    bool advertising;
    bool advertising_start_inflight;
    bool advertising_stop_after_start;
    bool advertising_requested;
    bool custom_address;
    bool tx_subscribed;
    bool flow_subscribed;
    bool rpc_subscribed;
    uint32_t rpc_status;
    uint32_t flow_value_be;
} s_serial = {0};

static uint16_t s_tx_handle;
static uint16_t s_rx_handle;
static uint16_t s_flow_handle;
static uint16_t s_rpc_handle;
static uint16_t s_dis_manufacturer_handle;
static uint16_t s_dis_hw_handle;
static uint16_t s_dis_sw_handle;
static uint16_t s_dis_api_handle;
static uint16_t s_battery_handle;
static uint8_t s_battery_level = 100;

static int serial_access_cb(
    uint16_t conn_handle,
    uint16_t attr_handle,
    struct ble_gatt_access_ctxt* context,
    void* arg);
static int serial_gap_event_cb(struct ble_gap_event* event, void* context);
static int serial_adv_event_cb(struct ble_gap_event* event, void* context);

static const struct ble_gatt_chr_def serial_characteristics[] = {
    {
        .uuid = &serial_tx_uuid.u,
        .access_cb = serial_access_cb,
        .flags = SERIAL_AUTHENTICATED_READ | BLE_GATT_CHR_F_INDICATE,
        .val_handle = &s_tx_handle,
    },
    {
        .uuid = &serial_rx_uuid.u,
        .access_cb = serial_access_cb,
        .flags = SERIAL_AUTHENTICATED_WRITE,
        .val_handle = &s_rx_handle,
    },
    {
        .uuid = &serial_flow_uuid.u,
        .access_cb = serial_access_cb,
        .flags = SERIAL_AUTHENTICATED_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_flow_handle,
    },
    {
        .uuid = &serial_rpc_uuid.u,
        .access_cb = serial_access_cb,
        .flags = SERIAL_AUTHENTICATED_WRITE | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_rpc_handle,
    },
    {0},
};

static const struct ble_gatt_chr_def dis_characteristics[] = {
    {.uuid = &dis_manufacturer_uuid.u,
     .access_cb = serial_access_cb,
     .flags = BLE_GATT_CHR_F_READ,
     .val_handle = &s_dis_manufacturer_handle},
    {.uuid = &dis_hw_uuid.u,
     .access_cb = serial_access_cb,
     .flags = BLE_GATT_CHR_F_READ,
     .val_handle = &s_dis_hw_handle},
    {.uuid = &dis_sw_uuid.u,
     .access_cb = serial_access_cb,
     .flags = BLE_GATT_CHR_F_READ,
     .val_handle = &s_dis_sw_handle},
    {.uuid = &dis_api_uuid.u,
     .access_cb = serial_access_cb,
     .flags = BLE_GATT_CHR_F_READ,
     .val_handle = &s_dis_api_handle},
    {0},
};

static const struct ble_gatt_chr_def bas_characteristics[] = {
    {.uuid = &bas_level_uuid.u,
     .access_cb = serial_access_cb,
     .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
     .val_handle = &s_battery_handle},
    {0},
};

static const struct ble_gatt_svc_def serial_services[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY,
     .uuid = &serial_svc_uuid.u,
     .characteristics = serial_characteristics},
    {.type = BLE_GATT_SVC_TYPE_PRIMARY,
     .uuid = &dis_svc_uuid.u,
     .characteristics = dis_characteristics},
    {.type = BLE_GATT_SVC_TYPE_PRIMARY,
     .uuid = &bas_svc_uuid.u,
     .characteristics = bas_characteristics},
    {0},
};

static void serial_lock_global(void) {
    if(s_serial.mutex) xSemaphoreTake(s_serial.mutex, portMAX_DELAY);
}

static void serial_unlock_global(void) {
    if(s_serial.mutex) xSemaphoreGive(s_serial.mutex);
}

static int serial_gap_adv_stop(void) {
#if CONFIG_BT_NIMBLE_EXT_ADV
    return ble_nimble_host_stop_advertising(0);
#else
    int rc = ble_gap_adv_stop();
    return rc == BLE_HS_EALREADY ? 0 : rc;
#endif
}

static void serial_lock(BleSerial* serial) {
    if(serial && serial->mutex) xSemaphoreTake(serial->mutex, portMAX_DELAY);
}

static void serial_unlock(BleSerial* serial) {
    if(serial && serial->mutex) xSemaphoreGive(serial->mutex);
}

static BleSerial* serial_callback_acquire(void) {
    if(!s_serial.mutex) return NULL;
    serial_lock_global();
    BleSerial* serial = s_serial.active;
    if(serial) {
        serial_lock(serial);
        serial->callback_refs++;
        serial_unlock(serial);
    }
    serial_unlock_global();
    return serial;
}

static void serial_callback_release(BleSerial* serial) {
    if(!serial) return;
    serial_lock(serial);
    if(serial->callback_refs > 0) serial->callback_refs--;
    bool drained = serial->callback_refs == 0;
    serial_unlock(serial);
    if(drained && serial->callbacks_drained) xSemaphoreGive(serial->callbacks_drained);
}

static bool serial_detach_and_wait(BleSerial* serial) {
    serial_lock_global();
    if(s_serial.active == serial) s_serial.active = NULL;
    serial_unlock_global();

    while(true) {
        serial_lock(serial);
        uint32_t refs = serial->callback_refs;
        serial_unlock(serial);
        if(refs == 0) return true;
        (void)xSemaphoreTake(serial->callbacks_drained, portMAX_DELAY);
    }
}

static void serial_update_connection(BleSerial* serial, bool connected) {
    BleSerialStateCallback callback;
    void* context;
    serial_lock(serial);
    if(serial->connected == connected) {
        serial_unlock(serial);
        return;
    }
    serial->connected = connected;
    callback = serial->state_callback;
    context = serial->state_context;
    serial_unlock(serial);
    if(callback) callback(connected, context);
}

static uint16_t serial_read_bytes(
    struct ble_gatt_access_ctxt* context,
    const void* value,
    uint16_t length) {
    int rc = os_mbuf_append(context->om, value, length);
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static uint16_t serial_handle_rx_write(BleSerial* serial, struct os_mbuf* om) {
    const uint16_t length = OS_MBUF_PKTLEN(om);
    if(length > BLE_SVC_SERIAL_DATA_LEN_MAX) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    uint8_t data[SERIAL_ACCESS_BUFFER];
    uint16_t copied = 0;
    int rc = ble_hs_mbuf_to_flat(om, data, sizeof(data), &copied);
    if(rc != 0 || copied != length) return BLE_ATT_ERR_UNLIKELY;

    SerialServiceEventCallback callback;
    void* context;
    serial_lock(serial);
    callback = serial->event_callback;
    context = serial->event_context;
    if(serial->bytes_ready >= length) {
        serial->bytes_ready -= length;
    } else {
        serial->bytes_ready = 0;
    }
    serial_unlock(serial);

    if(callback) {
        SerialServiceEvent event = {
            .event = SerialServiceEventTypeDataReceived,
            .data = {.buffer = data, .size = copied},
        };
        (void)callback(event, context);
    }
    return 0;
}

static uint16_t serial_handle_rpc_write(BleSerial* serial, struct os_mbuf* om) {
    const uint16_t length = OS_MBUF_PKTLEN(om);
    if(length > sizeof(s_serial.rpc_status)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if(length < sizeof(s_serial.rpc_status)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    uint32_t value = 0;
    uint16_t copied = 0;
    int rc = ble_hs_mbuf_to_flat(om, &value, sizeof(value), &copied);
    if(rc != 0 || copied != sizeof(value)) return BLE_ATT_ERR_UNLIKELY;
    serial_lock_global();
    s_serial.rpc_status = value;
    serial_unlock_global();
    if(value == FuriHalBtSerialRpcStatusNotActive) {
        SerialServiceEventCallback callback;
        void* context;
        serial_lock(serial);
        callback = serial->event_callback;
        context = serial->event_context;
        serial_unlock(serial);
        if(callback) {
            SerialServiceEvent event = {.event = SerialServiceEventTypesBleResetRequest};
            (void)callback(event, context);
        }
    }
    return 0;
}

static int serial_access_cb(
    uint16_t conn_handle,
    uint16_t attr_handle,
    struct ble_gatt_access_ctxt* context,
    void* arg) {
    (void)conn_handle;
    (void)arg;
    BleSerial* serial = serial_callback_acquire();
    if(!serial) return BLE_ATT_ERR_UNLIKELY;

    int rc = BLE_ATT_ERR_UNLIKELY;
    const ble_uuid_t* uuid = context->chr ? context->chr->uuid : NULL;

    if(context->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        if(attr_handle == s_tx_handle || attr_handle == s_rx_handle) {
            rc = 0; /* The Flipper serial contract returns an empty value here. */
        } else if(attr_handle == s_flow_handle) {
            serial_lock(serial);
            uint32_t value = __builtin_bswap32((uint32_t)serial->buff_size);
            serial_unlock(serial);
            rc = serial_read_bytes(context, &value, sizeof(value));
        } else if(attr_handle == s_rpc_handle) {
            serial_lock_global();
            uint32_t value = s_serial.rpc_status;
            serial_unlock_global();
            rc = serial_read_bytes(context, &value, sizeof(value));
        } else if(ble_uuid_cmp(uuid, &dis_manufacturer_uuid.u) == 0) {
            rc = serial_read_bytes(context, dis_manufacturer, sizeof(dis_manufacturer) - 1);
        } else if(ble_uuid_cmp(uuid, &dis_hw_uuid.u) == 0) {
            rc = serial_read_bytes(context, dis_hw_revision, sizeof(dis_hw_revision) - 1);
        } else if(ble_uuid_cmp(uuid, &dis_sw_uuid.u) == 0) {
            rc = serial_read_bytes(context, dis_sw_revision, sizeof(dis_sw_revision) - 1);
        } else if(ble_uuid_cmp(uuid, &dis_api_uuid.u) == 0) {
            rc = serial_read_bytes(context, dis_api_version, sizeof(dis_api_version) - 1);
        } else if(attr_handle == s_battery_handle) {
            rc = serial_read_bytes(context, &s_battery_level, sizeof(s_battery_level));
        }
    } else if(context->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        if(attr_handle == s_rx_handle) {
            rc = serial_handle_rx_write(serial, context->om);
        } else if(attr_handle == s_rpc_handle) {
            rc = serial_handle_rpc_write(serial, context->om);
        }
    }

    serial_callback_release(serial);
    return rc;
}

static bool serial_send_custom(
    uint16_t conn_handle,
    uint16_t attr_handle,
    const void* data,
    uint16_t length,
    bool indicate) {
    struct os_mbuf* om = ble_hs_mbuf_from_flat(data, length);
    if(!om) return false;
    int rc = indicate ? ble_gatts_indicate_custom(conn_handle, attr_handle, om) :
                        ble_gatts_notify_custom(conn_handle, attr_handle, om);
    if(rc != 0) {
        ESP_LOGW(TAG, "%s handle %u failed: %d", indicate ? "indicate" : "notify", attr_handle, rc);
        return false;
    }
    return true;
}

static void serial_emit_data_sent(BleSerial* serial) {
    SerialServiceEventCallback callback;
    void* context;
    serial_lock(serial);
    callback = serial->event_callback;
    context = serial->event_context;
    serial_unlock(serial);
    if(callback) {
        SerialServiceEvent event = {.event = SerialServiceEventTypeDataSent};
        (void)callback(event, context);
    }
}

static bool serial_start_advertising(BleSerial* serial) {
    if(!serial || !ble_nimble_host_is_synced()) return false;
    serial_lock_global();
    bool can_start = s_serial.active == serial && s_serial.advertising_requested &&
                     !s_serial.advertising && !s_serial.advertising_start_inflight;
    if(can_start) {
        s_serial.advertising_start_inflight = true;
        s_serial.advertising_stop_after_start = false;
    }
    serial_unlock_global();
    if(!can_start) return false;
    serial_lock(serial);
    bool connected = serial->connected;
    serial_unlock(serial);
    if(connected) {
        serial_lock_global();
        s_serial.advertising_start_inflight = false;
        s_serial.advertising_stop_after_start = false;
        serial_unlock_global();
        return false;
    }

    uint16_t advertised_uuid = 0x3080;
    const FuriHalVersionColor color = furi_hal_version_get_hw_color();
    if(color == FuriHalVersionColorBlack) advertised_uuid |= 0x0001;
    else if(color == FuriHalVersionColorWhite) advertised_uuid |= 0x0002;
    else if(color == FuriHalVersionColorTransparent) advertised_uuid |= 0x0003;

    const ble_uuid16_t service_uuid = BLE_UUID16_INIT(advertised_uuid);
    const size_t name_length = strnlen(serial->config.device_name, sizeof(serial->config.device_name));
    const uint8_t max_name_length = BLE_HS_ADV_MAX_SZ - 3 - 4 - 2;
    const uint8_t advertised_name_length = name_length > max_name_length ? max_name_length : name_length;
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids16 = &service_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;
    fields.name = (const uint8_t*)serial->config.device_name;
    fields.name_len = advertised_name_length;
    fields.name_is_complete = advertised_name_length == name_length;
    const uint8_t own_addr_type = s_serial.custom_address ? BLE_OWN_ADDR_RANDOM : BLE_OWN_ADDR_PUBLIC;
#if CONFIG_BT_NIMBLE_EXT_ADV
    int rc = ble_nimble_host_start_legacy_advertising(
        0,
        own_addr_type,
        SERIAL_ADV_MIN_INTERVAL,
        SERIAL_ADV_MAX_INTERVAL,
        &fields,
        serial_adv_event_cb,
        NULL);
#else
    int rc = ble_gap_adv_set_fields(&fields);
    if(rc == 0) {
        struct ble_gap_adv_params params = {0};
        params.conn_mode = BLE_GAP_CONN_MODE_UND;
        params.disc_mode = BLE_GAP_DISC_MODE_GEN;
        params.itvl_min = SERIAL_ADV_MIN_INTERVAL;
        params.itvl_max = SERIAL_ADV_MAX_INTERVAL;
        rc = ble_gap_adv_start(
            own_addr_type, NULL, BLE_HS_FOREVER, &params, serial_adv_event_cb, NULL);
    }
#endif
    if(rc != 0) {
        ESP_LOGE(TAG, "serial advertising configuration/start failed: %d", rc);
        serial_lock_global();
        s_serial.advertising_start_inflight = false;
        s_serial.advertising_stop_after_start = false;
        serial_unlock_global();
        return false;
    }

    serial_lock_global();
    s_serial.advertising_start_inflight = false;
    s_serial.advertising = true;
    bool stop_after_start = s_serial.advertising_stop_after_start ||
                            !s_serial.advertising_requested;
    s_serial.advertising_stop_after_start = false;
    serial_unlock_global();

    if(stop_after_start) {
        rc = serial_gap_adv_stop();
        serial_lock_global();
        if(rc == 0) s_serial.advertising = false;
        serial_unlock_global();
        return rc == 0;
    }
    return true;
}

static int serial_adv_event_cb(struct ble_gap_event* event, void* context) {
    (void)context;
    /* NimBLE sends PASSKEY_ACTION to the per-connection GAP callback, not to
     * the global event-listener list used by ble_core_security_listener. */
    if(event && event->type == BLE_GAP_EVENT_PASSKEY_ACTION) {
        (void)ble_core_handle_gap_event(event);
        return 0;
    }
    if(event && event->type == BLE_GAP_EVENT_REPEAT_PAIRING) {
        return ble_core_handle_gap_event(event);
    }
    return 0;
}

static int serial_gap_event_cb(struct ble_gap_event* event, void* context) {
    (void)context;
    BleSerial* serial = serial_callback_acquire();
    if(!serial || !event) {
        serial_callback_release(serial);
        return 0;
    }

    switch(event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if(event->connect.status == 0) {
            serial_lock_global();
            s_serial.advertising = false;
            s_serial.advertising_start_inflight = false;
            s_serial.tx_subscribed = false;
            s_serial.flow_subscribed = false;
            s_serial.rpc_subscribed = false;
            serial_unlock_global();
            serial_lock(serial);
            serial->conn_handle = event->connect.conn_handle;
            serial_unlock(serial);
            serial_update_connection(serial, true);
        } else {
            serial_lock_global();
            bool restart_after_failed_connect = s_serial.advertising_requested;
            serial_unlock_global();
            if(restart_after_failed_connect) (void)serial_start_advertising(serial);
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT: {
        serial_update_connection(serial, false);
        serial_lock_global();
        s_serial.advertising = false;
        bool restart_after_disconnect = s_serial.advertising_requested;
        serial_unlock_global();
        if(restart_after_disconnect) (void)serial_start_advertising(serial);
        break;
    }

    case BLE_GAP_EVENT_ADV_COMPLETE: {
        serial_lock_global();
        s_serial.advertising = false;
        bool restart_after_adv_complete = s_serial.advertising_requested;
        serial_unlock_global();
        if(restart_after_adv_complete) (void)serial_start_advertising(serial);
        break;
    }

    case BLE_GAP_EVENT_SUBSCRIBE: {
        bool update_flow = false;
        bool update_rpc = false;
        serial_lock_global();
        if(event->subscribe.attr_handle == s_tx_handle) {
            s_serial.tx_subscribed = event->subscribe.cur_indicate;
        } else if(event->subscribe.attr_handle == s_flow_handle) {
            s_serial.flow_subscribed = event->subscribe.cur_notify;
            update_flow = s_serial.flow_subscribed;
        } else if(event->subscribe.attr_handle == s_rpc_handle) {
            s_serial.rpc_subscribed = event->subscribe.cur_notify;
            update_rpc = s_serial.rpc_subscribed;
        }
        serial_unlock_global();
        if(update_flow) ble_gatts_chr_updated(s_flow_handle);
        if(update_rpc) ble_gatts_chr_updated(s_rpc_handle);
        break;
    }

    case BLE_GAP_EVENT_NOTIFY_TX:
        if(event->notify_tx.attr_handle == s_tx_handle && event->notify_tx.indication &&
           event->notify_tx.status == BLE_HS_EDONE) {
            serial_emit_data_sent(serial);
        }
        break;

    case BLE_GAP_EVENT_MTU: {
        GapEvent gap_event = {
            .type = GapEventTypeUpdateMTU,
            .data.max_packet_size = event->mtu.value > 3 ? event->mtu.value - 3 : 0,
        };
        furi_hal_bt_emit_gap_event(gap_event);
        break;
    }

    case BLE_GAP_EVENT_ENC_CHANGE:
        serial_lock(serial);
        bool connected = serial->connected;
        serial_unlock(serial);
        if(event->enc_change.status == 0 && connected) {
            serial_lock_global();
            bool notify_flow = s_serial.flow_subscribed;
            bool notify_rpc = s_serial.rpc_subscribed;
            serial_unlock_global();
            if(notify_flow) ble_gatts_chr_updated(s_flow_handle);
            if(notify_rpc) ble_gatts_chr_updated(s_rpc_handle);
        }
        break;

    default:
        break;
    }

    serial_callback_release(serial);
    return 0;
}

static bool serial_has_custom_mac(const BleSerialConfig* config) {
    static const uint8_t zero[6] = {0};
    return memcmp(config->mac, zero, sizeof(zero)) != 0;
}

static void serial_make_random_static_addr(uint8_t address[6]) {
    address[0] = (address[0] & 0x3F) | 0xC0;
}

static esp_err_t serial_register_services(void) {
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(serial_services);
    if(rc != 0) return ESP_FAIL;
    rc = ble_gatts_add_svcs(serial_services);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

void ble_serial_get_default_mac(uint8_t mac[6]) {
    if(mac) (void)esp_read_mac(mac, ESP_MAC_BT);
}

BleSerial* ble_serial_alloc(const BleSerialConfig* config) {
    if(!config) return NULL;
    if(ble_core_stack_is_up()) {
        ESP_LOGE(TAG, "refusing profile start while a previous NimBLE profile still owns the host");
        return NULL;
    }

    if(!s_serial.mutex) s_serial.mutex = xSemaphoreCreateMutex();
    if(!s_serial.mutex) return NULL;

    BleSerial* serial = calloc(1, sizeof(*serial));
    if(!serial) return NULL;
    serial->mutex = xSemaphoreCreateMutex();
    serial->callbacks_drained = xSemaphoreCreateBinary();
    if(!serial->mutex || !serial->callbacks_drained) {
        if(serial->callbacks_drained) vSemaphoreDelete(serial->callbacks_drained);
        if(serial->mutex) vSemaphoreDelete(serial->mutex);
        free(serial);
        return NULL;
    }
    memcpy(&serial->config, config, sizeof(serial->config));
    serial->config.device_name[BLE_SERIAL_DEVICE_NAME_LEN] = '\0';
    serial->conn_handle = BLE_HS_CONN_HANDLE_NONE;

    serial_lock_global();
    if(s_serial.active) {
        serial_unlock_global();
        vSemaphoreDelete(serial->callbacks_drained);
        vSemaphoreDelete(serial->mutex);
        free(serial);
        return NULL;
    }
    s_serial.active = serial;
    s_serial.advertising = false;
    s_serial.advertising_requested = false;
    s_serial.tx_subscribed = false;
    s_serial.flow_subscribed = false;
    s_serial.rpc_subscribed = false;
    s_serial.rpc_status = FuriHalBtSerialRpcStatusNotActive;
    s_serial.flow_value_be = 0;
    serial_unlock_global();

    esp_err_t err = ble_core_stack_prepare();
    if(err != ESP_OK) goto error;

    const BleCoreConfig security = {
        .pairing = config->pairing == BleSerialPairingDisplayOnly ? BleCorePairingDisplayOnly :
                   config->pairing == BleSerialPairingInputOnly ? BleCorePairingInputOnly :
                                                                 BleCorePairingVerifyYesNo,
        .bonding = config->bonding,
        .request_security = true,
        /* Match the working Bluedroid serial profile: legacy MITM + bonding. */
        .secure_connections = false,
        .require_authenticated = true,
        .auto_confirm_numeric_comparison = true,
        .link_auth_timeout_ms = 0,
        .fail_limit = 0,
        .fast_link = false,
    };
    err = ble_core_apply_config(&security);
    if(err != ESP_OK) goto error;

    (void)ble_svc_gap_device_name_set(serial->config.device_name);
    err = ble_core_register_gap_callback(serial_gap_event_cb, NULL);
    if(err != ESP_OK) goto error;

    s_serial.custom_address = serial_has_custom_mac(&serial->config);
    if(s_serial.custom_address) {
        uint8_t address[6];
        memcpy(address, serial->config.mac, sizeof(address));
        serial_make_random_static_addr(address);
        int rc = ble_nimble_host_set_random_address(address);
        if(rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGE(TAG, "setting serial random address failed: %d", rc);
            err = ESP_FAIL;
            goto error;
        }
    }

    memset(&s_tx_handle, 0, sizeof(s_tx_handle));
    memset(&s_rx_handle, 0, sizeof(s_rx_handle));
    memset(&s_flow_handle, 0, sizeof(s_flow_handle));
    memset(&s_rpc_handle, 0, sizeof(s_rpc_handle));
    memset(&s_dis_manufacturer_handle, 0, sizeof(s_dis_manufacturer_handle));
    memset(&s_dis_hw_handle, 0, sizeof(s_dis_hw_handle));
    memset(&s_dis_sw_handle, 0, sizeof(s_dis_sw_handle));
    memset(&s_dis_api_handle, 0, sizeof(s_dis_api_handle));
    memset(&s_battery_handle, 0, sizeof(s_battery_handle));

    err = serial_register_services();
    if(err != ESP_OK) goto error;
    err = ble_core_stack_start();
    if(err != ESP_OK) goto error;
    if(!s_tx_handle || !s_rx_handle || !s_flow_handle || !s_rpc_handle ||
       !s_dis_manufacturer_handle || !s_dis_hw_handle || !s_dis_sw_handle ||
       !s_dis_api_handle || !s_battery_handle) {
        ESP_LOGE(TAG, "NimBLE did not assign every serial GATT value handle");
        err = ESP_FAIL;
        goto error;
    }

    s_serial.initialized = true;
    ESP_LOGI(
        TAG,
        "NimBLE serial GATT ready; Furi profile packet limit is %u bytes",
        BLE_PROFILE_SERIAL_PACKET_SIZE_MAX);
    return serial;

error:
    ESP_LOGE(TAG, "NimBLE serial startup failed: %s", esp_err_to_name(err));
    (void)serial_detach_and_wait(serial);
    (void)ble_core_stack_down();
    vSemaphoreDelete(serial->callbacks_drained);
    vSemaphoreDelete(serial->mutex);
    free(serial);
    return NULL;
}

bool ble_serial_free(BleSerial* serial) {
    if(!serial) return true;
    ble_serial_stop_advertising();
    if(serial->connected) ble_core_disconnect();
    if(!serial_detach_and_wait(serial)) return false;
    serial_lock_global();
    s_serial.initialized = false;
    s_serial.tx_subscribed = false;
    s_serial.flow_subscribed = false;
    s_serial.rpc_subscribed = false;
    serial_unlock_global();
    vSemaphoreDelete(serial->callbacks_drained);
    vSemaphoreDelete(serial->mutex);
    free(serial);
    return true;
}

void ble_serial_reset_initialized(void) {
    s_serial.initialized = false;
    s_serial.active = NULL;
    s_serial.advertising = false;
    s_serial.advertising_requested = false;
}

bool ble_serial_is_teardown_pending(void) {
    if(!s_serial.mutex) return false;
    bool pending = false;
    serial_lock_global();
    pending = s_serial.active != NULL;
    serial_unlock_global();
    return pending;
}

void ble_serial_set_state_callback(BleSerial* serial, BleSerialStateCallback callback, void* context) {
    if(!serial) return;
    serial_lock(serial);
    serial->state_callback = callback;
    serial->state_context = context;
    bool connected = serial->connected;
    serial_unlock(serial);
    if(callback) callback(connected, context);
}

bool ble_serial_is_connected(BleSerial* serial) {
    if(!serial) return false;
    serial_lock(serial);
    bool connected = serial->connected;
    serial_unlock(serial);
    return connected;
}

void ble_serial_set_event_callback(
    BleSerial* serial,
    uint16_t buff_size,
    SerialServiceEventCallback callback,
    void* context) {
    if(!serial) return;
    serial_lock(serial);
    serial->event_callback = callback;
    serial->event_context = context;
    serial->buff_size = buff_size;
    serial->bytes_ready = buff_size;
    bool connected = serial->connected;
    serial_unlock(serial);
    s_serial.flow_value_be = __builtin_bswap32((uint32_t)buff_size);
    serial_lock_global();
    bool notify_flow = connected && s_serial.flow_subscribed;
    serial_unlock_global();
    if(notify_flow) ble_gatts_chr_updated(s_flow_handle);
}

bool ble_serial_tx(BleSerial* serial, uint8_t* data, uint16_t size) {
    if(!serial || !data || !size) return false;
    serial_lock(serial);
    bool connected = serial->connected;
    uint16_t conn_handle = serial->conn_handle;
    serial_unlock(serial);
    serial_lock_global();
    bool subscribed = s_serial.tx_subscribed;
    serial_unlock_global();
    if(!connected || s_tx_handle == 0 || !subscribed) return false;

    const uint16_t mtu = ble_att_mtu(conn_handle);
    if(mtu < 3 || size > mtu - 3) {
        ESP_LOGW(TAG, "TX length %u exceeds negotiated ATT payload %u", size, mtu > 3 ? mtu - 3 : 0);
        return false;
    }
    return serial_send_custom(conn_handle, s_tx_handle, data, size, true);
}

void ble_serial_set_rpc_active(BleSerial* serial, FuriHalBtSerialRpcStatus status) {
    if(!serial) return;
    serial_lock(serial);
    bool connected = serial->connected;
    serial_unlock(serial);
    serial_lock_global();
    s_serial.rpc_status = (uint32_t)status;
    bool notify = connected && s_serial.rpc_subscribed;
    serial_unlock_global();
    if(notify) ble_gatts_chr_updated(s_rpc_handle);
}

void ble_serial_notify_buffer_is_empty(BleSerial* serial) {
    if(!serial) return;
    serial_lock(serial);
    bool was_empty = serial->bytes_ready == 0;
    if(was_empty) serial->bytes_ready = serial->buff_size;
    bool connected = serial->connected;
    uint16_t buff_size = serial->buff_size;
    serial_unlock(serial);
    serial_lock_global();
    bool subscribed = s_serial.flow_subscribed;
    serial_unlock_global();
    if(was_empty && connected && subscribed) {
        s_serial.flow_value_be = __builtin_bswap32((uint32_t)buff_size);
        ble_gatts_chr_updated(s_flow_handle);
    }
}

bool ble_serial_start_advertising(void) {
    if(!s_serial.mutex) return false;
    serial_lock_global();
    BleSerial* serial = s_serial.active;
    s_serial.advertising_requested = serial != NULL;
    bool already_started = s_serial.advertising || s_serial.advertising_start_inflight;
    serial_unlock_global();
    if(!serial) return false;
    bool started = serial_start_advertising(serial);
    serial_lock(serial);
    bool connected = serial->connected;
    serial_unlock(serial);
    return started || already_started || connected;
}

void ble_serial_stop_advertising(void) {
    if(!s_serial.mutex) return;
    serial_lock_global();
    s_serial.advertising_requested = false;
    bool was_advertising = s_serial.advertising;
    if(s_serial.advertising_start_inflight) s_serial.advertising_stop_after_start = true;
    serial_unlock_global();
    if(!was_advertising) return;

    int rc = serial_gap_adv_stop();
    serial_lock_global();
    if(rc == 0) s_serial.advertising = false;
    serial_unlock_global();
    if(rc != 0) ESP_LOGW(TAG, "advertising stop failed: %d", rc);
}

void ble_serial_refresh_advertising(void) {
    serial_lock_global();
    bool restart = s_serial.advertising_requested;
    if(s_serial.advertising) {
        int rc = serial_gap_adv_stop();
        if(rc == 0) s_serial.advertising = false;
        else ESP_LOGW(TAG, "advertising refresh stop failed: %d", rc);
    }
    s_serial.advertising_requested = restart;
    serial_unlock_global();
    if(restart) (void)ble_serial_start_advertising();
}

bool ble_serial_is_advertising(void) {
    serial_lock_global();
    BleSerial* serial = s_serial.active;
    bool requested = s_serial.advertising_requested && s_serial.advertising;
    serial_unlock_global();
    if(!serial) return false;
    serial_lock(serial);
    bool connected = serial->connected;
    serial_unlock(serial);
    return requested && !connected;
}

bool ble_serial_is_active(void) {
    serial_lock_global();
    bool active = s_serial.initialized && s_serial.active != NULL;
    serial_unlock_global();
    return active;
}

bool ble_serial_remove_pairing(void) {
    return ble_core_remove_all_bonds();
}

#endif /* CONFIG_BT_NIMBLE_ENABLED */
