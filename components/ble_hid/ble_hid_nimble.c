#include "ble_hid.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <ble_core.h>
#include "ble_nimble_host.h"
#include <esp_err.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <host/ble_att.h>
#include <host/ble_gap.h>
#include <host/ble_gatt.h>
#include <host/ble_hs.h>
#include <host/ble_hs_adv.h>
#include <host/ble_hs_id.h>
#include <host/ble_hs_mbuf.h>
#include <host/ble_uuid.h>
#include <os/os_mbuf.h>
#include <services/gap/ble_svc_gap.h>
#include <services/gatt/ble_svc_gatt.h>

#include "ble_hid_report_map.h"

#define TAG "ble_hid"
#define HID_ADV_MIN_INTERVAL 0x20
#define HID_ADV_MAX_INTERVAL 0x30
#define HID_STATE_QUEUE_DEPTH 8
#define HID_ADV_INSTANCE 0

enum HidAttribute {
    HidAttrInformation = 1,
    HidAttrReportMap,
    HidAttrControlPoint,
    HidAttrProtocolMode,
    HidAttrKeyboardInput,
    HidAttrKeyboardOutput,
    HidAttrMouseInput,
    HidAttrConsumerInput,
    HidAttrBattery,
    HidAttrManufacturer,
    HidAttrHardwareRevision,
    HidAttrSoftwareRevision,
    HidAttrPnpId,
};

typedef struct {
    uint8_t modifiers;
    uint8_t reserved;
    uint8_t keys[BLE_HID_KEYBOARD_KEYS_MAX];
} __attribute__((packed)) BleHidKeyboardReport;

typedef struct {
    uint8_t buttons;
    int8_t x;
    int8_t y;
    int8_t wheel;
} __attribute__((packed)) BleHidMouseReport;

typedef struct {
    uint16_t key;
} __attribute__((packed)) BleHidConsumerReport;

typedef struct BleHid {
    BleHidConfig config;
    BleHidStateCallback state_callback;
    void* state_context;
    BleHidKeyboardReport keyboard_report;
    BleHidMouseReport mouse_report;
    BleHidConsumerReport consumer_report;
    uint16_t conn_handle;
    uint32_t callback_refs;
    uint8_t led_state;
    uint8_t protocol_mode;
    uint8_t control_point;
    bool connected;
} BleHid;

typedef struct {
    BleHidStateCallback callback;
    void* context;
    SemaphoreHandle_t barrier;
    bool connected;
    bool is_barrier;
} BleHidStateEvent;

static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_callbacks_drained;
static SemaphoreHandle_t s_state_barrier;
static QueueHandle_t s_state_event_queue;
static TaskHandle_t s_state_event_task;
static BleHid* s_active;
static bool s_hid_service_ready;
static bool s_hidd_started;
static bool s_advertising_requested;
static bool s_advertising;
static bool s_adv_start_inflight;
static bool s_stop_after_adv_start;
static bool s_shutting_down;
static bool s_has_custom_address;
static bool s_keyboard_subscribed;
static bool s_mouse_subscribed;
static bool s_consumer_subscribed;
static bool s_battery_subscribed;

static uint16_t s_hid_information_handle;
static uint16_t s_report_map_handle;
static uint16_t s_control_point_handle;
static uint16_t s_protocol_mode_handle;
static uint16_t s_keyboard_input_handle;
static uint16_t s_keyboard_output_handle;
static uint16_t s_mouse_input_handle;
static uint16_t s_consumer_input_handle;
static uint16_t s_battery_handle;

static const uint8_t s_hid_information[4] = {0x11, 0x01, 0x00, 0x03};
static const char s_manufacturer[] = "Flipper Devices";
static const char s_hardware_revision[] = "ESP32";
static const char s_software_revision[] = "1.4.3";
static const uint8_t s_pnp_id[7] = {0x02, 0xC0, 0x16, 0xDF, 0x05, 0x00, 0x01};
static uint8_t s_battery_level = 100;
static uint8_t s_keyboard_input_ref[2] = {BLE_HID_REPORT_ID_KEYBOARD, 0x01};
static uint8_t s_keyboard_output_ref[2] = {BLE_HID_REPORT_ID_KEYBOARD, 0x02};
static uint8_t s_mouse_input_ref[2] = {BLE_HID_REPORT_ID_MOUSE, 0x01};
static uint8_t s_consumer_input_ref[2] = {BLE_HID_REPORT_ID_CONSUMER, 0x01};
static uint8_t s_battery_external_ref[2] = {0x0F, 0x18};

static int hid_gap_adv_stop(void) {
#if CONFIG_BT_NIMBLE_EXT_ADV
    return ble_nimble_host_stop_advertising(HID_ADV_INSTANCE);
#else
    int rc = ble_gap_adv_stop();
    return rc == BLE_HS_EALREADY ? 0 : rc;
#endif
}

static ble_uuid16_t s_hid_service_uuid = BLE_UUID16_INIT(0x1812);
static const ble_uuid16_t s_hid_information_uuid = BLE_UUID16_INIT(0x2A4A);
static const ble_uuid16_t s_report_map_uuid = BLE_UUID16_INIT(0x2A4B);
static const ble_uuid16_t s_control_point_uuid = BLE_UUID16_INIT(0x2A4C);
static const ble_uuid16_t s_report_uuid = BLE_UUID16_INIT(0x2A4D);
static const ble_uuid16_t s_protocol_mode_uuid = BLE_UUID16_INIT(0x2A4E);
static const ble_uuid16_t s_report_reference_uuid = BLE_UUID16_INIT(0x2908);
static const ble_uuid16_t s_external_report_reference_uuid = BLE_UUID16_INIT(0x2907);
static const ble_uuid16_t s_battery_service_uuid = BLE_UUID16_INIT(0x180F);
static const ble_uuid16_t s_battery_level_uuid = BLE_UUID16_INIT(0x2A19);
static const ble_uuid16_t s_device_info_service_uuid = BLE_UUID16_INIT(0x180A);
static const ble_uuid16_t s_manufacturer_uuid = BLE_UUID16_INIT(0x2A29);
static const ble_uuid16_t s_hardware_revision_uuid = BLE_UUID16_INIT(0x2A27);
static const ble_uuid16_t s_software_revision_uuid = BLE_UUID16_INIT(0x2A28);
static const ble_uuid16_t s_pnp_id_uuid = BLE_UUID16_INIT(0x2A50);

static int hid_access_cb(
    uint16_t conn_handle,
    uint16_t attr_handle,
    struct ble_gatt_access_ctxt* context,
    void* arg);
static int hid_report_reference_access_cb(
    uint16_t conn_handle,
    uint16_t attr_handle,
    struct ble_gatt_access_ctxt* context,
    void* arg);
static int hid_external_reference_access_cb(
    uint16_t conn_handle,
    uint16_t attr_handle,
    struct ble_gatt_access_ctxt* context,
    void* arg);
static int hid_gap_event_cb(struct ble_gap_event* event, void* context);
static int hid_adv_event_cb(struct ble_gap_event* event, void* context);

static struct ble_gatt_dsc_def s_keyboard_input_descriptors[] = {
    {.uuid = &s_report_reference_uuid.u, .att_flags = BLE_ATT_F_READ,
     .access_cb = hid_report_reference_access_cb, .arg = s_keyboard_input_ref},
    {0},
};
static struct ble_gatt_dsc_def s_keyboard_output_descriptors[] = {
    {.uuid = &s_report_reference_uuid.u, .att_flags = BLE_ATT_F_READ,
     .access_cb = hid_report_reference_access_cb, .arg = s_keyboard_output_ref},
    {0},
};
static struct ble_gatt_dsc_def s_mouse_input_descriptors[] = {
    {.uuid = &s_report_reference_uuid.u, .att_flags = BLE_ATT_F_READ,
     .access_cb = hid_report_reference_access_cb, .arg = s_mouse_input_ref},
    {0},
};
static struct ble_gatt_dsc_def s_consumer_input_descriptors[] = {
    {.uuid = &s_report_reference_uuid.u, .att_flags = BLE_ATT_F_READ,
     .access_cb = hid_report_reference_access_cb, .arg = s_consumer_input_ref},
    {0},
};
static struct ble_gatt_dsc_def s_report_map_descriptors[] = {
    {.uuid = &s_external_report_reference_uuid.u, .att_flags = BLE_ATT_F_READ,
     .access_cb = hid_external_reference_access_cb, .arg = (void*)s_battery_external_ref},
    {0},
};

#define HID_VALUE_ARG(attribute) ((void*)(uintptr_t)(attribute))
#define HID_ENCRYPTED_READ (BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC)
#define HID_ENCRYPTED_WRITE (BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | \
                             BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | \
                             BLE_GATT_CHR_F_WRITE_ENC)

static const struct ble_gatt_chr_def s_hid_characteristics[] = {
    {.uuid = &s_hid_information_uuid.u, .access_cb = hid_access_cb,
     .arg = HID_VALUE_ARG(HidAttrInformation), .flags = HID_ENCRYPTED_READ,
     .val_handle = &s_hid_information_handle},
    {.uuid = &s_report_map_uuid.u, .access_cb = hid_access_cb,
     .arg = HID_VALUE_ARG(HidAttrReportMap), .descriptors = s_report_map_descriptors,
     .flags = HID_ENCRYPTED_READ, .val_handle = &s_report_map_handle},
    {.uuid = &s_control_point_uuid.u, .access_cb = hid_access_cb,
     .arg = HID_VALUE_ARG(HidAttrControlPoint), .flags = BLE_GATT_CHR_F_WRITE_NO_RSP |
             BLE_GATT_CHR_F_WRITE_ENC, .val_handle = &s_control_point_handle},
    {.uuid = &s_protocol_mode_uuid.u, .access_cb = hid_access_cb,
     .arg = HID_VALUE_ARG(HidAttrProtocolMode), .flags = HID_ENCRYPTED_WRITE,
     .val_handle = &s_protocol_mode_handle},
    {.uuid = &s_report_uuid.u, .access_cb = hid_access_cb,
     .arg = HID_VALUE_ARG(HidAttrKeyboardInput), .descriptors = s_keyboard_input_descriptors,
     .flags = HID_ENCRYPTED_READ | BLE_GATT_CHR_F_NOTIFY,
     .val_handle = &s_keyboard_input_handle},
    {.uuid = &s_report_uuid.u, .access_cb = hid_access_cb,
     .arg = HID_VALUE_ARG(HidAttrKeyboardOutput), .descriptors = s_keyboard_output_descriptors,
     .flags = HID_ENCRYPTED_WRITE, .val_handle = &s_keyboard_output_handle},
    {.uuid = &s_report_uuid.u, .access_cb = hid_access_cb,
     .arg = HID_VALUE_ARG(HidAttrMouseInput), .descriptors = s_mouse_input_descriptors,
     .flags = HID_ENCRYPTED_READ | BLE_GATT_CHR_F_NOTIFY,
     .val_handle = &s_mouse_input_handle},
    {.uuid = &s_report_uuid.u, .access_cb = hid_access_cb,
     .arg = HID_VALUE_ARG(HidAttrConsumerInput), .descriptors = s_consumer_input_descriptors,
     .flags = HID_ENCRYPTED_READ | BLE_GATT_CHR_F_NOTIFY,
     .val_handle = &s_consumer_input_handle},
    {0},
};

static const struct ble_gatt_chr_def s_battery_characteristics[] = {
    {.uuid = &s_battery_level_uuid.u, .access_cb = hid_access_cb,
     .arg = HID_VALUE_ARG(HidAttrBattery),
     .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
     .val_handle = &s_battery_handle},
    {0},
};

static const struct ble_gatt_chr_def s_device_info_characteristics[] = {
    {.uuid = &s_manufacturer_uuid.u, .access_cb = hid_access_cb,
     .arg = HID_VALUE_ARG(HidAttrManufacturer), .flags = BLE_GATT_CHR_F_READ},
    {.uuid = &s_hardware_revision_uuid.u, .access_cb = hid_access_cb,
     .arg = HID_VALUE_ARG(HidAttrHardwareRevision), .flags = BLE_GATT_CHR_F_READ},
    {.uuid = &s_software_revision_uuid.u, .access_cb = hid_access_cb,
     .arg = HID_VALUE_ARG(HidAttrSoftwareRevision), .flags = BLE_GATT_CHR_F_READ},
    {.uuid = &s_pnp_id_uuid.u, .access_cb = hid_access_cb,
     .arg = HID_VALUE_ARG(HidAttrPnpId), .flags = BLE_GATT_CHR_F_READ},
    {0},
};

static const struct ble_gatt_svc_def s_hid_services[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &s_battery_service_uuid.u,
     .characteristics = s_battery_characteristics},
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &s_device_info_service_uuid.u,
     .characteristics = s_device_info_characteristics},
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &s_hid_service_uuid.u,
     .characteristics = s_hid_characteristics},
    {0},
};

static void hid_state_event_task(void* context) {
    QueueHandle_t queue = context;
    BleHidStateEvent event;
    while(true) {
        if(xQueueReceive(queue, &event, portMAX_DELAY) != pdTRUE) continue;
        if(event.is_barrier) {
            if(event.barrier) xSemaphoreGive(event.barrier);
        } else if(event.callback) {
            event.callback(event.connected, event.context);
        }
    }
}

static bool hid_state_queue_init(void) {
    if(s_state_event_queue) return true;
    QueueHandle_t queue = xQueueCreate(HID_STATE_QUEUE_DEPTH, sizeof(BleHidStateEvent));
    if(!queue) return false;
    SemaphoreHandle_t barrier = xSemaphoreCreateBinary();
    if(!barrier) {
        vQueueDelete(queue);
        return false;
    }
    if(xTaskCreate(hid_state_event_task, "ble_hid_evt", 3072, queue, 5, &s_state_event_task) != pdPASS) {
        vSemaphoreDelete(barrier);
        vQueueDelete(queue);
        return false;
    }
    s_state_event_queue = queue;
    s_state_barrier = barrier;
    return true;
}

static void hid_queue_state_event(BleHidStateCallback callback, void* context, bool connected) {
    if(!s_state_event_queue || !callback) return;
    const BleHidStateEvent event = {
        .callback = callback,
        .context = context,
        .connected = connected,
        .is_barrier = false,
    };
    if(xQueueSend(s_state_event_queue, &event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "HID state event queue full; dropping one state callback");
    }
}

static bool hid_queue_barrier(void) {
    if(!s_state_event_queue || !s_state_barrier) return true;
    BleHidStateEvent event = {.barrier = s_state_barrier, .is_barrier = true};
    if(xQueueSend(s_state_event_queue, &event, portMAX_DELAY) != pdTRUE) return false;
    return xSemaphoreTake(s_state_barrier, portMAX_DELAY) == pdTRUE;
}

static BleHid* hid_callback_acquire(void) {
    BleHid* hid;
    portENTER_CRITICAL(&s_state_lock);
    hid = s_active;
    if(hid) hid->callback_refs++;
    portEXIT_CRITICAL(&s_state_lock);
    return hid;
}

static void hid_callback_release(BleHid* hid) {
    bool drained;
    if(!hid) return;
    portENTER_CRITICAL(&s_state_lock);
    if(hid->callback_refs) hid->callback_refs--;
    drained = hid->callback_refs == 0;
    portEXIT_CRITICAL(&s_state_lock);
    if(drained && s_callbacks_drained) xSemaphoreGive(s_callbacks_drained);
}

static bool hid_wait_for_callbacks(BleHid* hid) {
    while(true) {
        portENTER_CRITICAL(&s_state_lock);
        const uint32_t refs = hid->callback_refs;
        portEXIT_CRITICAL(&s_state_lock);
        if(refs == 0) return true;
        if(!s_callbacks_drained || xSemaphoreTake(s_callbacks_drained, portMAX_DELAY) != pdTRUE) {
            ESP_LOGE(TAG, "timed out waiting for NimBLE HID callbacks");
            return false;
        }
    }
}

static int hid_append(struct ble_gatt_access_ctxt* context, const void* value, size_t length) {
    if(length > UINT16_MAX) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    return os_mbuf_append(context->om, value, (uint16_t)length) == 0 ?
               0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int hid_report_reference_access_cb(
    uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt* context, void* arg) {
    (void)conn_handle;
    (void)attr_handle;
    if(!context || context->op != BLE_GATT_ACCESS_OP_READ_DSC || !arg) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    return hid_append(context, arg, 2);
}

static int hid_external_reference_access_cb(
    uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt* context, void* arg) {
    (void)conn_handle;
    (void)attr_handle;
    if(!context || context->op != BLE_GATT_ACCESS_OP_READ_DSC || !arg) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    return hid_append(context, arg, 2);
}

static int hid_access_cb(
    uint16_t conn_handle,
    uint16_t attr_handle,
    struct ble_gatt_access_ctxt* context,
    void* arg) {
    (void)conn_handle;
    (void)attr_handle;
    if(!context || !arg) return BLE_ATT_ERR_UNLIKELY;
    BleHid* hid = hid_callback_acquire();
    if(!hid) return BLE_ATT_ERR_UNLIKELY;
    const enum HidAttribute attribute = (enum HidAttribute)(uintptr_t)arg;
    int rc = BLE_ATT_ERR_UNLIKELY;

    if(context->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t byte_value;
        BleHidKeyboardReport keyboard;
        BleHidMouseReport mouse;
        BleHidConsumerReport consumer;
        portENTER_CRITICAL(&s_state_lock);
        keyboard = hid->keyboard_report;
        mouse = hid->mouse_report;
        consumer = hid->consumer_report;
        byte_value = attribute == HidAttrBattery ? s_battery_level :
                     attribute == HidAttrProtocolMode ? hid->protocol_mode :
                     attribute == HidAttrKeyboardOutput ? hid->led_state : 0;
        portEXIT_CRITICAL(&s_state_lock);
        switch(attribute) {
        case HidAttrInformation:
            rc = hid_append(context, s_hid_information, sizeof(s_hid_information));
            break;
        case HidAttrReportMap:
            rc = hid_append(context, ble_hid_report_map, sizeof(ble_hid_report_map));
            break;
        case HidAttrProtocolMode:
        case HidAttrBattery:
            rc = hid_append(context, &byte_value, sizeof(byte_value));
            break;
        case HidAttrKeyboardInput:
            rc = hid_append(context, &keyboard, sizeof(keyboard));
            break;
        case HidAttrKeyboardOutput:
            rc = hid_append(context, &byte_value, sizeof(byte_value));
            break;
        case HidAttrMouseInput:
            rc = hid_append(context, &mouse, sizeof(mouse));
            break;
        case HidAttrConsumerInput:
            rc = hid_append(context, &consumer, sizeof(consumer));
            break;
        case HidAttrManufacturer:
            rc = hid_append(context, s_manufacturer, sizeof(s_manufacturer) - 1U);
            break;
        case HidAttrHardwareRevision:
            rc = hid_append(context, s_hardware_revision, sizeof(s_hardware_revision) - 1U);
            break;
        case HidAttrSoftwareRevision:
            rc = hid_append(context, s_software_revision, sizeof(s_software_revision) - 1U);
            break;
        case HidAttrPnpId:
            rc = hid_append(context, s_pnp_id, sizeof(s_pnp_id));
            break;
        case HidAttrControlPoint:
            rc = BLE_ATT_ERR_READ_NOT_PERMITTED;
            break;
        }
    } else if(context->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t value[2] = {0};
        uint16_t copied = 0;
        if(OS_MBUF_PKTLEN(context->om) > sizeof(value) ||
           ble_hs_mbuf_to_flat(context->om, value, sizeof(value), &copied) != 0 || copied == 0) {
            rc = BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        } else {
            portENTER_CRITICAL(&s_state_lock);
            if(attribute == HidAttrKeyboardOutput && copied >= 1U) {
                hid->led_state = value[0];
                rc = 0;
            } else if(attribute == HidAttrProtocolMode && copied == 1U && value[0] <= 1U) {
                hid->protocol_mode = value[0];
                rc = 0;
            } else if(attribute == HidAttrControlPoint && copied == 1U && value[0] <= 1U) {
                hid->control_point = value[0];
                rc = 0;
            } else {
                rc = BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }
            portEXIT_CRITICAL(&s_state_lock);
        }
    }

    hid_callback_release(hid);
    return rc;
}

static bool hid_configure_advertising(const BleHidConfig* config) {
    const size_t name_len = strnlen(config->device_name, sizeof(config->device_name));
    const size_t max_name_len = BLE_HS_ADV_MAX_SZ - 3U - 4U - 2U;
    const size_t advertised_name_len = name_len > max_name_len ? max_name_len : name_len;
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids16 = &s_hid_service_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;
    fields.name = (const uint8_t*)config->device_name;
    fields.name_len = advertised_name_len;
    fields.name_is_complete = advertised_name_len == name_len;
#if CONFIG_BT_NIMBLE_EXT_ADV
    const uint8_t own_addr_type = s_has_custom_address ? BLE_OWN_ADDR_RANDOM : BLE_OWN_ADDR_PUBLIC;
    const int rc = ble_nimble_host_start_legacy_advertising(
        HID_ADV_INSTANCE,
        own_addr_type,
        HID_ADV_MIN_INTERVAL,
        HID_ADV_MAX_INTERVAL,
        &fields,
        hid_adv_event_cb,
        NULL);
#else
    const int rc = ble_gap_adv_set_fields(&fields);
#endif
    if(rc != 0) ESP_LOGE(TAG, "NimBLE HID advertising setup failed: %d", rc);
    return rc == 0;
}

static int hid_adv_event_cb(struct ble_gap_event* event, void* context) {
    (void)context;
    if(event && event->type == BLE_GAP_EVENT_REPEAT_PAIRING) {
        return ble_core_handle_gap_event(event);
    }
    if(event && (event->type == BLE_GAP_EVENT_ADV_COMPLETE || event->type == BLE_GAP_EVENT_CONNECT)) {
        portENTER_CRITICAL(&s_state_lock);
        s_advertising = false;
        portEXIT_CRITICAL(&s_state_lock);
    }
    return 0;
}

static bool hid_try_start_advertising(void) {
    bool can_start;
    BleHid* hid = hid_callback_acquire();
    if(!hid) return false;
    portENTER_CRITICAL(&s_state_lock);
    can_start = s_active == hid && s_hid_service_ready && s_hidd_started && s_advertising_requested &&
                !s_advertising && !s_adv_start_inflight && !s_shutting_down && !hid->connected;
    if(can_start) s_adv_start_inflight = true;
    portEXIT_CRITICAL(&s_state_lock);
    if(!can_start) {
        hid_callback_release(hid);
        return false;
    }

    if(!ble_nimble_host_is_synced() || !hid_configure_advertising(&hid->config)) {
        portENTER_CRITICAL(&s_state_lock);
        s_adv_start_inflight = false;
        portEXIT_CRITICAL(&s_state_lock);
        hid_callback_release(hid);
        return false;
    }
#if CONFIG_BT_NIMBLE_EXT_ADV
    int rc = 0;
#else
    const struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = HID_ADV_MIN_INTERVAL,
        .itvl_max = HID_ADV_MAX_INTERVAL,
    };
    const uint8_t own_addr_type = s_has_custom_address ? BLE_OWN_ADDR_RANDOM : BLE_OWN_ADDR_PUBLIC;
    int rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &params, hid_adv_event_cb, NULL);
#endif
    portENTER_CRITICAL(&s_state_lock);
    s_adv_start_inflight = false;
    if(rc == 0) s_advertising = true;
    const bool stop_after_start = s_stop_after_adv_start || !s_advertising_requested;
    s_stop_after_adv_start = false;
    portEXIT_CRITICAL(&s_state_lock);
    if(rc != 0) {
        ESP_LOGE(TAG, "NimBLE HID advertising start failed: %d", rc);
        hid_callback_release(hid);
        return false;
    }
    if(stop_after_start) {
        rc = hid_gap_adv_stop();
        portENTER_CRITICAL(&s_state_lock);
        if(rc == 0) s_advertising = false;
        portEXIT_CRITICAL(&s_state_lock);
    }
    hid_callback_release(hid);
    return rc == 0;
}

static void hid_update_connection(BleHid* hid, bool connected, uint16_t conn_handle) {
    BleHidStateCallback callback;
    void* context;
    portENTER_CRITICAL(&s_state_lock);
    hid->connected = connected;
    hid->conn_handle = connected ? conn_handle : BLE_HS_CONN_HANDLE_NONE;
    callback = hid->state_callback;
    context = hid->state_context;
    if(!connected) {
        s_keyboard_subscribed = false;
        s_mouse_subscribed = false;
        s_consumer_subscribed = false;
        s_battery_subscribed = false;
    }
    portEXIT_CRITICAL(&s_state_lock);
    if(callback) hid_queue_state_event(callback, context, connected);
}

static int hid_gap_event_cb(struct ble_gap_event* event, void* context) {
    (void)context;
    if(!event) return 0;
    BleHid* hid = hid_callback_acquire();
    if(!hid) return 0;
    switch(event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if(event->connect.status == 0) {
            portENTER_CRITICAL(&s_state_lock);
            s_advertising = false;
            portEXIT_CRITICAL(&s_state_lock);
            hid_update_connection(hid, true, event->connect.conn_handle);
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        hid_update_connection(hid, false, BLE_HS_CONN_HANDLE_NONE);
        (void)hid_try_start_advertising();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        portENTER_CRITICAL(&s_state_lock);
        if(event->subscribe.attr_handle == s_keyboard_input_handle) {
            s_keyboard_subscribed = event->subscribe.cur_notify;
        } else if(event->subscribe.attr_handle == s_mouse_input_handle) {
            s_mouse_subscribed = event->subscribe.cur_notify;
        } else if(event->subscribe.attr_handle == s_consumer_input_handle) {
            s_consumer_subscribed = event->subscribe.cur_notify;
        } else if(event->subscribe.attr_handle == s_battery_handle) {
            s_battery_subscribed = event->subscribe.cur_notify;
        }
        portEXIT_CRITICAL(&s_state_lock);
        break;
    default:
        break;
    }
    hid_callback_release(hid);
    return 0;
}

static bool hid_stack_init_once(void) {
    if(!s_callbacks_drained) s_callbacks_drained = xSemaphoreCreateBinary();
    if(!s_callbacks_drained || !hid_state_queue_init()) return false;
    return true;
}

void ble_hid_get_default_mac(uint8_t mac[6]) {
    if(mac) (void)esp_read_mac(mac, ESP_MAC_BT);
}

BleHid* ble_hid_alloc(const BleHidConfig* config) {
    if(!config || ble_core_stack_is_up() || !hid_stack_init_once()) return NULL;
    BleHid* hid = calloc(1, sizeof(*hid));
    if(!hid) return NULL;
    memcpy(&hid->config, config, sizeof(hid->config));
    hid->config.device_name[BLE_HID_DEVICE_NAME_LEN] = '\0';
    if(hid->config.device_name[0] == '\0') {
        free(hid);
        return NULL;
    }
    hid->conn_handle = BLE_HS_CONN_HANDLE_NONE;
    hid->protocol_mode = 1;

    portENTER_CRITICAL(&s_state_lock);
    if(s_active || s_shutting_down) {
        portEXIT_CRITICAL(&s_state_lock);
        free(hid);
        return NULL;
    }
    s_active = hid;
    s_hid_service_ready = false;
    s_hidd_started = false;
    s_advertising = false;
    s_advertising_requested = false;
    s_adv_start_inflight = false;
    s_stop_after_adv_start = false;
    s_shutting_down = false;
    s_has_custom_address = false;
    s_keyboard_subscribed = false;
    s_mouse_subscribed = false;
    s_consumer_subscribed = false;
    s_battery_subscribed = false;
    portEXIT_CRITICAL(&s_state_lock);

    esp_err_t err = ble_core_stack_prepare();
    if(err != ESP_OK) goto error;
    const BleCoreConfig security = {
        .pairing = config->pairing == BleHidPairingModeDisplayOnly ? BleCorePairingDisplayOnly :
                   config->pairing == BleHidPairingModeInputOnly ? BleCorePairingInputOnly :
                                                                   BleCorePairingVerifyYesNo,
        .bonding = config->bonding,
        .request_security = true,
        .just_works = config->just_works,
        .secure_connections = true,
        .require_authenticated = false,
        .auto_confirm_numeric_comparison = false,
        .link_auth_timeout_ms = 0,
        .fail_limit = 0,
        .fast_link = true,
    };
    err = ble_core_apply_config(&security);
    if(err != ESP_OK) goto error;

    static const uint8_t zero_addr[6] = {0};
    if(memcmp(config->mac, zero_addr, sizeof(zero_addr)) != 0) {
        uint8_t random_address[6];
        memcpy(random_address, config->mac, sizeof(random_address));
        random_address[0] = (random_address[0] & 0x3FU) | 0xC0U;
        int rc = ble_nimble_host_set_random_address(random_address);
        if(rc != 0) {
            ESP_LOGE(TAG, "NimBLE HID random address setup failed: %d", rc);
            err = ESP_FAIL;
            goto error;
        }
        s_has_custom_address = true;
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();
    err = ble_svc_gap_device_name_set(config->device_name);
    if(err != 0) {
        ESP_LOGE(TAG, "setting NimBLE HID device name failed: %d", err);
        goto error;
    }
    err = ble_svc_gap_device_appearance_set(0x03C1);
    if(err != 0) {
        ESP_LOGE(TAG, "setting NimBLE HID appearance failed: %d", err);
        goto error;
    }
    int rc = ble_gatts_count_cfg(s_hid_services);
    if(rc != 0) {
        ESP_LOGE(TAG, "NimBLE HID GATT capacity configuration failed: %d", rc);
        err = ESP_FAIL;
        goto error;
    }
    rc = ble_gatts_add_svcs(s_hid_services);
    if(rc != 0) {
        ESP_LOGE(TAG, "NimBLE HID GATT service registration failed: %d", rc);
        err = ESP_FAIL;
        goto error;
    }
    err = ble_core_register_gap_callback(hid_gap_event_cb, NULL);
    if(err != ESP_OK) goto error;
    err = ble_core_stack_start();
    if(err != ESP_OK) goto error;
    if(!s_hid_information_handle || !s_report_map_handle || !s_control_point_handle ||
       !s_protocol_mode_handle || !s_keyboard_input_handle || !s_keyboard_output_handle ||
       !s_mouse_input_handle || !s_consumer_input_handle || !s_battery_handle) {
        ESP_LOGE(
            TAG,
            "NimBLE HID GATT handles incomplete: info=%u map=%u control=%u protocol=%u "
            "keyboard-in=%u keyboard-out=%u mouse=%u consumer=%u battery=%u",
            (unsigned)s_hid_information_handle,
            (unsigned)s_report_map_handle,
            (unsigned)s_control_point_handle,
            (unsigned)s_protocol_mode_handle,
            (unsigned)s_keyboard_input_handle,
            (unsigned)s_keyboard_output_handle,
            (unsigned)s_mouse_input_handle,
            (unsigned)s_consumer_input_handle,
            (unsigned)s_battery_handle);
        err = ESP_FAIL;
        goto error;
    }
    portENTER_CRITICAL(&s_state_lock);
    s_hid_service_ready = true;
    s_hidd_started = true;
    portEXIT_CRITICAL(&s_state_lock);
    return hid;

error:
    ESP_LOGE(TAG, "NimBLE HID start failed: %s", esp_err_to_name(err));
    portENTER_CRITICAL(&s_state_lock);
    s_active = NULL;
    s_shutting_down = false;
    s_hid_service_ready = false;
    s_hidd_started = false;
    s_has_custom_address = false;
    portEXIT_CRITICAL(&s_state_lock);
    (void)ble_core_stack_down();
    free(hid);
    return NULL;
}

bool ble_hid_free(BleHid* hid) {
    if(!hid) return true;
    ble_hid_stop_advertising();
    if(ble_hid_is_connected(hid)) ble_core_disconnect();
    for(int i = 0; i < 40 && ble_hid_is_connected(hid); ++i) vTaskDelay(pdMS_TO_TICKS(50));

    portENTER_CRITICAL(&s_state_lock);
    if(s_active == hid) s_active = NULL;
    s_shutting_down = true;
    portEXIT_CRITICAL(&s_state_lock);
    if(!hid_wait_for_callbacks(hid)) return false;
    if(!hid_queue_barrier()) return false;

    portENTER_CRITICAL(&s_state_lock);
    s_hid_service_ready = false;
    s_hidd_started = false;
    s_advertising = false;
    s_advertising_requested = false;
    s_adv_start_inflight = false;
    s_stop_after_adv_start = false;
    s_has_custom_address = false;
    s_shutting_down = false;
    portEXIT_CRITICAL(&s_state_lock);
    free(hid);
    return true;
}

void ble_hid_set_state_callback(BleHid* hid, BleHidStateCallback callback, void* context) {
    if(!hid) return;
    portENTER_CRITICAL(&s_state_lock);
    hid->state_callback = callback;
    hid->state_context = context;
    const bool connected = hid->connected;
    portEXIT_CRITICAL(&s_state_lock);
    if(callback) callback(connected, context);
}

bool ble_hid_is_connected(BleHid* hid) {
    if(!hid) return false;
    portENTER_CRITICAL(&s_state_lock);
    const bool connected = s_active == hid && hid->connected;
    portEXIT_CRITICAL(&s_state_lock);
    return connected;
}

static bool hid_send_report(BleHid* hid, uint16_t attr_handle, const void* data,
                            uint16_t length, bool subscribed) {
    uint16_t conn_handle;
    bool can_send;
    portENTER_CRITICAL(&s_state_lock);
    can_send = s_active == hid && hid->connected && subscribed;
    conn_handle = hid->conn_handle;
    portEXIT_CRITICAL(&s_state_lock);
    if(!can_send || !data || length == 0) return false;
    struct os_mbuf* om = ble_hs_mbuf_from_flat(data, length);
    if(!om) return false;
    const int rc = ble_gatts_notify_custom(conn_handle, attr_handle, om);
    if(rc != 0) ESP_LOGW(TAG, "HID report notify failed (handle=%u rc=%d)", attr_handle, rc);
    return rc == 0;
}

bool ble_hid_kb_press(BleHid* hid, uint16_t button) {
    if(!hid) return false;
    BleHidKeyboardReport report;
    portENTER_CRITICAL(&s_state_lock);
    for(size_t i = 0; i < BLE_HID_KEYBOARD_KEYS_MAX; ++i) {
        if(hid->keyboard_report.keys[i] == (button & 0xFFU)) break;
        if(hid->keyboard_report.keys[i] == 0) {
            hid->keyboard_report.keys[i] = button & 0xFFU;
            break;
        }
    }
    hid->keyboard_report.modifiers |= button >> 8;
    report = hid->keyboard_report;
    const bool subscribed = s_keyboard_subscribed;
    portEXIT_CRITICAL(&s_state_lock);
    return hid_send_report(hid, s_keyboard_input_handle, &report, sizeof(report), subscribed);
}

bool ble_hid_kb_release(BleHid* hid, uint16_t button) {
    if(!hid) return false;
    BleHidKeyboardReport report;
    portENTER_CRITICAL(&s_state_lock);
    for(size_t i = 0; i < BLE_HID_KEYBOARD_KEYS_MAX; ++i) {
        if(hid->keyboard_report.keys[i] == (button & 0xFFU)) {
            hid->keyboard_report.keys[i] = 0;
            break;
        }
    }
    hid->keyboard_report.modifiers &= (uint8_t)~(button >> 8);
    report = hid->keyboard_report;
    const bool subscribed = s_keyboard_subscribed;
    portEXIT_CRITICAL(&s_state_lock);
    return hid_send_report(hid, s_keyboard_input_handle, &report, sizeof(report), subscribed);
}

bool ble_hid_kb_release_all(BleHid* hid) {
    if(!hid) return false;
    BleHidKeyboardReport report = {0};
    portENTER_CRITICAL(&s_state_lock);
    hid->keyboard_report = report;
    const bool subscribed = s_keyboard_subscribed;
    portEXIT_CRITICAL(&s_state_lock);
    return hid_send_report(hid, s_keyboard_input_handle, &report, sizeof(report), subscribed);
}

bool ble_hid_mouse_move(BleHid* hid, int8_t dx, int8_t dy) {
    if(!hid) return false;
    BleHidMouseReport report;
    portENTER_CRITICAL(&s_state_lock);
    hid->mouse_report.x = dx;
    hid->mouse_report.y = dy;
    report = hid->mouse_report;
    hid->mouse_report.x = 0;
    hid->mouse_report.y = 0;
    const bool subscribed = s_mouse_subscribed;
    portEXIT_CRITICAL(&s_state_lock);
    return hid_send_report(hid, s_mouse_input_handle, &report, sizeof(report), subscribed);
}

bool ble_hid_mouse_press(BleHid* hid, uint8_t button) {
    if(!hid) return false;
    BleHidMouseReport report;
    portENTER_CRITICAL(&s_state_lock);
    hid->mouse_report.buttons |= button;
    report = hid->mouse_report;
    const bool subscribed = s_mouse_subscribed;
    portEXIT_CRITICAL(&s_state_lock);
    return hid_send_report(hid, s_mouse_input_handle, &report, sizeof(report), subscribed);
}

bool ble_hid_mouse_release(BleHid* hid, uint8_t button) {
    if(!hid) return false;
    BleHidMouseReport report;
    portENTER_CRITICAL(&s_state_lock);
    hid->mouse_report.buttons &= (uint8_t)~button;
    report = hid->mouse_report;
    const bool subscribed = s_mouse_subscribed;
    portEXIT_CRITICAL(&s_state_lock);
    return hid_send_report(hid, s_mouse_input_handle, &report, sizeof(report), subscribed);
}

bool ble_hid_mouse_release_all(BleHid* hid) {
    if(!hid) return false;
    BleHidMouseReport report = {0};
    portENTER_CRITICAL(&s_state_lock);
    hid->mouse_report = report;
    const bool subscribed = s_mouse_subscribed;
    portEXIT_CRITICAL(&s_state_lock);
    return hid_send_report(hid, s_mouse_input_handle, &report, sizeof(report), subscribed);
}

bool ble_hid_mouse_scroll(BleHid* hid, int8_t delta) {
    if(!hid) return false;
    BleHidMouseReport report;
    portENTER_CRITICAL(&s_state_lock);
    hid->mouse_report.wheel = delta;
    report = hid->mouse_report;
    hid->mouse_report.wheel = 0;
    const bool subscribed = s_mouse_subscribed;
    portEXIT_CRITICAL(&s_state_lock);
    return hid_send_report(hid, s_mouse_input_handle, &report, sizeof(report), subscribed);
}

bool ble_hid_consumer_press(BleHid* hid, uint16_t button) {
    if(!hid) return false;
    BleHidConsumerReport report = {.key = button};
    portENTER_CRITICAL(&s_state_lock);
    hid->consumer_report = report;
    const bool subscribed = s_consumer_subscribed;
    portEXIT_CRITICAL(&s_state_lock);
    return hid_send_report(hid, s_consumer_input_handle, &report, sizeof(report), subscribed);
}

bool ble_hid_consumer_release(BleHid* hid, uint16_t button) {
    if(!hid) return false;
    BleHidConsumerReport report;
    portENTER_CRITICAL(&s_state_lock);
    if(hid->consumer_report.key == button) hid->consumer_report.key = 0;
    report = hid->consumer_report;
    const bool subscribed = s_consumer_subscribed;
    portEXIT_CRITICAL(&s_state_lock);
    return hid_send_report(hid, s_consumer_input_handle, &report, sizeof(report), subscribed);
}

bool ble_hid_consumer_release_all(BleHid* hid) {
    if(!hid) return false;
    BleHidConsumerReport report = {0};
    portENTER_CRITICAL(&s_state_lock);
    hid->consumer_report = report;
    const bool subscribed = s_consumer_subscribed;
    portEXIT_CRITICAL(&s_state_lock);
    return hid_send_report(hid, s_consumer_input_handle, &report, sizeof(report), subscribed);
}

uint8_t ble_hid_get_led_state(BleHid* hid) {
    if(!hid) return 0;
    portENTER_CRITICAL(&s_state_lock);
    const uint8_t state = hid->led_state;
    portEXIT_CRITICAL(&s_state_lock);
    return state;
}

bool ble_hid_start_advertising(void) {
    portENTER_CRITICAL(&s_state_lock);
    const bool active = s_active != NULL;
    if(active) s_advertising_requested = true;
    const bool started = s_advertising || s_adv_start_inflight;
    const bool connected = active && s_active->connected;
    portEXIT_CRITICAL(&s_state_lock);
    if(!active) return false;
    return hid_try_start_advertising() || started || connected;
}

void ble_hid_stop_advertising(void) {
    bool stop;
    portENTER_CRITICAL(&s_state_lock);
    s_advertising_requested = false;
    if(s_adv_start_inflight) s_stop_after_adv_start = true;
    stop = s_advertising;
    portEXIT_CRITICAL(&s_state_lock);
    if(!stop) return;
    const int rc = hid_gap_adv_stop();
    portENTER_CRITICAL(&s_state_lock);
    if(rc == 0) s_advertising = false;
    portEXIT_CRITICAL(&s_state_lock);
    if(rc != 0) ESP_LOGW(TAG, "NimBLE HID advertising stop failed: %d", rc);
}

bool ble_hid_is_advertising(void) {
    portENTER_CRITICAL(&s_state_lock);
    const bool advertising = s_active && s_advertising_requested && s_advertising && !s_active->connected;
    portEXIT_CRITICAL(&s_state_lock);
    return advertising;
}

bool ble_hid_is_active(void) {
    portENTER_CRITICAL(&s_state_lock);
    const bool active = s_active != NULL;
    portEXIT_CRITICAL(&s_state_lock);
    return active;
}

bool ble_hid_is_teardown_pending(void) {
    portENTER_CRITICAL(&s_state_lock);
    const bool pending = s_shutting_down;
    portEXIT_CRITICAL(&s_state_lock);
    return pending;
}

void ble_hid_reset_gatts_state(void) {
    portENTER_CRITICAL(&s_state_lock);
    s_active = NULL;
    s_hid_service_ready = false;
    s_hidd_started = false;
    s_advertising_requested = false;
    s_advertising = false;
    s_adv_start_inflight = false;
    s_stop_after_adv_start = false;
    s_shutting_down = false;
    s_has_custom_address = false;
    s_keyboard_subscribed = false;
    s_mouse_subscribed = false;
    s_consumer_subscribed = false;
    s_battery_subscribed = false;
    s_hid_information_handle = 0;
    s_report_map_handle = 0;
    s_control_point_handle = 0;
    s_protocol_mode_handle = 0;
    s_keyboard_input_handle = 0;
    s_keyboard_output_handle = 0;
    s_mouse_input_handle = 0;
    s_consumer_input_handle = 0;
    s_battery_handle = 0;
    portEXIT_CRITICAL(&s_state_lock);
}

bool ble_hid_remove_pairing(void) {
    return ble_core_remove_all_bonds();
}
