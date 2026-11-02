/**
 * @file chameleon.c
 * @brief NimBLE GATT-client transport + ChameleonUltra NUS protocol.
 *
 * NimBLE replaces the old Bluedroid central path: stop BT service → initialize
 * the NimBLE host in BLE mode → GAP scan → connect → service/characteristic
 * discovery, extended with
 * notification subscription (CCCD) and the ChameleonUltra frame protocol.
 */
#include "chameleon.h"

#include <sdkconfig.h>
#if CONFIG_BT_NIMBLE_ENABLED
#include <ble_core.h>
#include <host/ble_att.h>
#include <host/ble_gap.h>
#include <host/ble_gatt.h>
#include <host/ble_hs.h>
#include <host/ble_hs_id.h>
#include <host/ble_hs_mbuf.h>
#include <host/ble_uuid.h>
#include <nimble/ble.h>
#include <os/os_mbuf.h>
#else
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_gap_ble_api.h>
#include <esp_gattc_api.h>
#include <esp_gatt_common_api.h>
#endif
#include <esp_log.h>
#include <furi.h>
#include <btshim.h>
#include <esp_attr.h>
#include <string.h>

#define TAG               "Chameleon"
#define CHAM_DEV_NAME     "ChameleonUltra"

#if CONFIG_BT_NIMBLE_ENABLED
typedef uint8_t ChamAddress[6];
typedef uint8_t ChamAddressType;
typedef struct {
    uint16_t len;
    union {
        uint16_t uuid16;
        uint32_t uuid32;
        uint8_t uuid128[16];
    } uuid;
} ChamUuid;
#define CHAM_UUID_LEN_16 2
#define CHAM_UUID_LEN_128 16
#else
typedef esp_bd_addr_t ChamAddress;
typedef esp_ble_addr_type_t ChamAddressType;
typedef esp_bt_uuid_t ChamUuid;
#define CHAM_UUID_LEN_16 ESP_UUID_LEN_16
#define CHAM_UUID_LEN_128 ESP_UUID_LEN_128
#define CHAM_GATTC_APP_ID 0x43
#endif

/* Nordic UART Service UUIDs, stored in BLE's little-endian 128-bit wire order:
 * 6E400001-B5A3-F393-E0A9-E50E24DCCA9E (service), -0002 TX(write), -0003 RX(notify) */
static const uint8_t NUS_SVC[16] =
    {0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
     0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E};
static const uint8_t NUS_TX[16] =
    {0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
     0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E};
static const uint8_t NUS_RX[16] =
    {0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
     0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E};
static const uint16_t CCCD_UUID = 0x2902;

/* HF14A_RAW option bits: b7 activateRfField, b6 waitResponse, b5 appendCrc,
 * b4 autoSelect, b3 keepRfField, b2 checkResponseCrc. Default = a normal
 * ISO14443-3A exchange with auto-anticollision and CRC handling. */
#define CHAMELEON_RAW_OPT_DEFAULT ((1 << 6) | (1 << 5) | (1 << 4) | (1 << 2)) /* 0x74 */

#if CONFIG_BT_NIMBLE_ENABLED
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_connect_complete;
static volatile bool s_chars_done;
static volatile bool s_descriptors_done;
static bool s_cccd_found;
static uint16_t s_cccd_handle;
#else
static esp_gatt_if_t s_gattc_if = ESP_GATT_IF_NONE;
static uint16_t s_conn_id = 0;
#endif
static volatile bool s_connected = false;
static bool s_hal_started = false;
/* True if BT was disabled in settings when we connected: we force-started the
 * stack so the ESP controller gets initialized, and must disable it again on
 * disconnect to honor the user's setting. */
static bool s_bt_was_disabled = false;

static volatile bool s_scanning = false;
static volatile bool s_dev_found = false;
static ChamAddress s_dev_addr;
static ChamAddressType s_dev_addr_type;

#define CHAM_MAX_SVC 24
typedef struct {
    ChamUuid uuid;
    uint16_t start;
    uint16_t end;
} ChamSvc;
static EXT_RAM_BSS_ATTR ChamSvc s_svcs[CHAM_MAX_SVC];
static volatile uint16_t s_svc_count = 0;

static volatile bool s_search_done = false;
static uint16_t s_svc_start = 0;
static uint16_t s_svc_end = 0;
static uint16_t s_write_handle = 0;
static uint16_t s_notify_handle = 0;
static volatile bool s_notify_registered = false;
static volatile bool s_cccd_written = false;

static int s_device_mode = -1; /* cache to skip redundant CHANGE_DEVICE_MODE */

static void cham_hal_stop(void);

/* RX frame reassembly + last parsed response (commands are serialized) */
static EXT_RAM_BSS_ATTR uint8_t s_acc[CHAMELEON_RESP_DATA_MAX + 32];
static size_t s_acc_len = 0;
static ChameleonResp s_last_resp;
static volatile bool s_resp_ready = false;

static bool uuid_is(const ChamUuid* u, const uint8_t ref[16]) {
    if(u->len != CHAM_UUID_LEN_128) return false;
    if(memcmp(u->uuid.uuid128, ref, 16) == 0) return true;
    /* Be byte-order tolerant: also accept the reversed representation */
    for(int i = 0; i < 16; i++)
        if(u->uuid.uuid128[i] != ref[15 - i]) return false;
    return true;
}

static void uuid_log(const char* tag, const ChamUuid* u) {
    if(u->len == CHAM_UUID_LEN_16) {
        ESP_LOGD(TAG, "%s uuid16=%04X", tag, u->uuid.uuid16);
    } else if(u->len == CHAM_UUID_LEN_128) {
        const uint8_t* b = u->uuid.uuid128;
        ESP_LOGD(
            TAG,
            "%s uuid128=%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
            tag, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10],
            b[11], b[12], b[13], b[14], b[15]);
    } else {
        ESP_LOGD(TAG, "%s uuid len=%d", tag, u->len);
    }
}

static uint8_t cham_lrc(const uint8_t* d, size_t n) {
    uint8_t s = 0;
    for(size_t i = 0; i < n; i++) s += d[i];
    return (uint8_t)((0x100 - (s & 0xff)) & 0xff);
}

/* Try to extract one complete frame from the accumulation buffer. */
static void cham_try_parse(void) {
    /* Resync to SOF 0x11 0xEF */
    while(s_acc_len >= 2 && !(s_acc[0] == 0x11 && s_acc[1] == 0xEF)) {
        memmove(s_acc, s_acc + 1, --s_acc_len);
    }
    if(s_acc_len < 10) return;

    uint16_t data_len = ((uint16_t)s_acc[6] << 8) | s_acc[7];
    if(data_len > CHAMELEON_RESP_DATA_MAX) {
        /* Bogus length — drop SOF and resync */
        memmove(s_acc, s_acc + 2, s_acc_len -= 2);
        return;
    }
    size_t frame_len = 10u + data_len;
    if(s_acc_len < frame_len) return;

    s_last_resp.command = ((uint16_t)s_acc[2] << 8) | s_acc[3];
    s_last_resp.status = s_acc[5];
    s_last_resp.data_len = data_len;
    if(data_len) memcpy(s_last_resp.data, &s_acc[9], data_len);
    s_resp_ready = true;

    /* Consume the frame; keep any trailing bytes for the next parse */
    size_t rest = s_acc_len - frame_len;
    if(rest) memmove(s_acc, s_acc + frame_len, rest);
    s_acc_len = rest;
}

/* ---------------------------------------------------------------- GAP ---- */

#if CONFIG_BT_NIMBLE_ENABLED
static bool ad_name_matches(const uint8_t* data, uint8_t length) {
    uint8_t offset = 0;
    while(data && offset < length) {
        const uint8_t field_len = data[offset];
        if(field_len == 0 || (uint16_t)offset + field_len + 1U > length) break;
        const uint8_t type = data[offset + 1U];
        if((type == 0x08 || type == 0x09) && field_len > 1) {
            const size_t name_len = field_len - 1U;
            return name_len == strlen(CHAM_DEV_NAME) &&
                   memcmp(&data[offset + 2U], CHAM_DEV_NAME, name_len) == 0;
        }
        offset = (uint8_t)(offset + field_len + 1U);
    }
    return false;
}

static void cham_process_scan_report(
    const uint8_t* data,
    uint8_t length,
    const ble_addr_t* address) {
    if(s_dev_found || !address || !ad_name_matches(data, length)) return;
    for(size_t i = 0; i < sizeof(s_dev_addr); ++i) {
        s_dev_addr[i] = address->val[5U - i];
    }
    s_dev_addr_type = address->type;
    s_dev_found = true;
    (void)ble_gap_disc_cancel();
}

static void cham_handle_notification(struct os_mbuf* om) {
    if(!om) return;
    const size_t packet_len = OS_MBUF_PKTLEN(om);
    size_t space = sizeof(s_acc) - s_acc_len;
    size_t copy_len = packet_len;
    if(copy_len > space) {
        s_acc_len = 0;
        copy_len = packet_len > sizeof(s_acc) ? sizeof(s_acc) : packet_len;
    }
    uint16_t copied = 0;
    if(ble_hs_mbuf_to_flat(om, s_acc + s_acc_len, (uint16_t)copy_len, &copied) == 0) {
        s_acc_len += copied;
        cham_try_parse();
    }
}

static int cham_profile_gap_cb(struct ble_gap_event* event, void* context) {
    (void)context;
    if(event && event->type == BLE_GAP_EVENT_NOTIFY_RX &&
       event->notify_rx.attr_handle == s_notify_handle) {
        cham_handle_notification(event->notify_rx.om);
    }
    return 0;
}

static int cham_gap_cb(struct ble_gap_event* event, void* context) {
    (void)context;
    if(!event) return 0;
    switch(event->type) {
    case BLE_GAP_EVENT_DISC:
        cham_process_scan_report(
            event->disc.data, event->disc.length_data, &event->disc.addr);
        break;
#if CONFIG_BT_NIMBLE_EXT_ADV
    case BLE_GAP_EVENT_EXT_DISC:
        if(event->ext_disc.data_status != BLE_GAP_EXT_ADV_DATA_STATUS_INCOMPLETE) {
            cham_process_scan_report(
                event->ext_disc.data, event->ext_disc.length_data, &event->ext_disc.addr);
        }
        break;
#endif
    case BLE_GAP_EVENT_DISC_COMPLETE:
        s_scanning = false;
        break;
    case BLE_GAP_EVENT_CONNECT:
        s_connect_complete = true;
        if(event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_connected = true;
            int rc = ble_gattc_exchange_mtu(s_conn_handle, NULL, NULL);
            if(rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGW(TAG, "MTU exchange request failed: %d", rc);
        } else {
            s_connected = false;
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_connected = false;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_connect_complete = true;
        break;
    default:
        break;
    }
    return 0;
}

static bool uuid128_is(const ble_uuid_t* uuid, const uint8_t ref[16]) {
    return uuid && uuid->type == BLE_UUID_TYPE_128 &&
           memcmp(((const ble_uuid128_t*)uuid)->value, ref, 16) == 0;
}

static void cham_uuid_from_nimble(ChamUuid* out, const ble_uuid_any_t* uuid) {
    memset(out, 0, sizeof(*out));
    switch(uuid->u.type) {
    case BLE_UUID_TYPE_16:
        out->len = CHAM_UUID_LEN_16;
        out->uuid.uuid16 = uuid->u16.value;
        break;
    case BLE_UUID_TYPE_32:
        out->len = 4;
        out->uuid.uuid32 = uuid->u32.value;
        break;
    case BLE_UUID_TYPE_128:
        out->len = CHAM_UUID_LEN_128;
        memcpy(out->uuid.uuid128, uuid->u128.value, sizeof(out->uuid.uuid128));
        break;
    default:
        break;
    }
}

static int cham_service_cb(uint16_t conn_handle, const struct ble_gatt_error* error,
                           const struct ble_gatt_svc* service, void* context) {
    (void)conn_handle;
    (void)context;
    if(error && error->status == 0 && service && s_svc_count < CHAM_MAX_SVC) {
        ChamSvc* out = &s_svcs[s_svc_count++];
        cham_uuid_from_nimble(&out->uuid, &service->uuid);
        out->start = service->start_handle;
        out->end = service->end_handle;
    } else if(error && error->status != 0) {
        if(error->status != BLE_HS_EDONE) ESP_LOGW(TAG, "service discovery ended: %u", error->status);
        s_search_done = true;
    }
    return 0;
}

static int cham_characteristic_cb(uint16_t conn_handle, const struct ble_gatt_error* error,
                                  const struct ble_gatt_chr* chr, void* context) {
    (void)conn_handle;
    (void)context;
    if(error && error->status == 0 && chr) {
        if(uuid128_is(&chr->uuid.u, NUS_TX)) s_write_handle = chr->val_handle;
        else if(uuid128_is(&chr->uuid.u, NUS_RX)) s_notify_handle = chr->val_handle;
    } else if(error && error->status != 0) {
        if(error->status != BLE_HS_EDONE) ESP_LOGW(TAG, "characteristic discovery ended: %u", error->status);
        s_chars_done = true;
    }
    return 0;
}

static int cham_write_done_cb(uint16_t conn_handle, const struct ble_gatt_error* error,
                              struct ble_gatt_attr* attr, void* context) {
    (void)conn_handle;
    (void)attr;
    (void)context;
    if(error && error->status != 0) ESP_LOGW(TAG, "NUS write failed: %u", error->status);
    return 0;
}

static int cham_cccd_write_cb(uint16_t conn_handle, const struct ble_gatt_error* error,
                              struct ble_gatt_attr* attr, void* context) {
    (void)conn_handle;
    (void)attr;
    (void)context;
    s_cccd_written = error && error->status == 0;
    s_notify_registered = s_cccd_written;
    return 0;
}

static int cham_descriptor_cb(uint16_t conn_handle, const struct ble_gatt_error* error,
                              uint16_t chr_val_handle, const struct ble_gatt_dsc* dsc,
                              void* context) {
    (void)conn_handle;
    (void)context;
    if(error && error->status == 0 && dsc && chr_val_handle == s_notify_handle &&
       dsc->uuid.u.type == BLE_UUID_TYPE_16 && dsc->uuid.u16.value == CCCD_UUID) {
        s_cccd_found = true;
        s_cccd_handle = dsc->handle;
    } else if(error && error->status != 0) {
        if(error->status != BLE_HS_EDONE) ESP_LOGW(TAG, "descriptor discovery ended: %u", error->status);
        s_descriptors_done = true;
    }
    return 0;
}

#else
static void cham_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
    switch(event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        if(param->scan_param_cmpl.status == ESP_BT_STATUS_SUCCESS)
            esp_ble_gap_start_scanning(0);
        break;
    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        s_scanning = (param->scan_start_cmpl.status == ESP_BT_STATUS_SUCCESS);
        break;
    case ESP_GAP_BLE_SCAN_RESULT_EVT:
        if(param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT && !s_dev_found) {
            char name[33] = "";
            for(int pass = 0; pass < 2 && name[0] == '\0'; pass++) {
                uint8_t* adv;
                uint8_t adv_len;
                if(pass == 0) {
                    adv = param->scan_rst.ble_adv;
                    adv_len = param->scan_rst.adv_data_len;
                } else {
                    adv = param->scan_rst.ble_adv + param->scan_rst.adv_data_len;
                    adv_len = param->scan_rst.scan_rsp_len;
                }
                uint8_t pos = 0;
                while(pos < adv_len) {
                    uint8_t len = adv[pos];
                    if(len == 0 || pos + len >= adv_len) break;
                    uint8_t type = adv[pos + 1];
                    if(type == 0x09 || type == 0x08) {
                        uint8_t nl = len - 1;
                        if(nl > 32) nl = 32;
                        memcpy(name, &adv[pos + 2], nl);
                        name[nl] = '\0';
                        break;
                    }
                    pos += len + 1;
                }
            }
            if(strcmp(name, CHAM_DEV_NAME) == 0) {
                memcpy(s_dev_addr, param->scan_rst.bda, 6);
                s_dev_addr_type = param->scan_rst.ble_addr_type;
                s_dev_found = true;
                esp_ble_gap_stop_scanning();
            }
        }
        break;
    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        s_scanning = false;
        break;
    default:
        break;
    }
}

/* --------------------------------------------------------------- GATTC --- */

static void cham_gattc_cb(
    esp_gattc_cb_event_t event,
    esp_gatt_if_t gattc_if,
    esp_ble_gattc_cb_param_t* param) {
    switch(event) {
    case ESP_GATTC_REG_EVT:
        if(param->reg.status == ESP_GATT_OK) s_gattc_if = gattc_if;
        break;
    case ESP_GATTC_OPEN_EVT:
        if(param->open.status == ESP_GATT_OK) {
            s_conn_id = param->open.conn_id;
            s_connected = true;
            esp_ble_gattc_send_mtu_req(gattc_if, s_conn_id);
        } else {
            s_connected = false;
        }
        break;
    case ESP_GATTC_CFG_MTU_EVT:
        ESP_LOGD(TAG, "MTU=%d", param->cfg_mtu.mtu);
        break;
    case ESP_GATTC_SEARCH_RES_EVT:
        uuid_log("SVC", &param->search_res.srvc_id.uuid);
        if(s_svc_count < CHAM_MAX_SVC) {
            ChamSvc* s = &s_svcs[s_svc_count];
            memcpy(&s->uuid, &param->search_res.srvc_id.uuid, sizeof(esp_bt_uuid_t));
            s->start = param->search_res.start_handle;
            s->end = param->search_res.end_handle;
            s_svc_count++;
        }
        if(uuid_is(&param->search_res.srvc_id.uuid, NUS_SVC)) {
            s_svc_start = param->search_res.start_handle;
            s_svc_end = param->search_res.end_handle;
        }
        break;
    case ESP_GATTC_SEARCH_CMPL_EVT:
        ESP_LOGD(TAG, "search complete: %u services", s_svc_count);
        s_search_done = true;
        break;
    case ESP_GATTC_REG_FOR_NOTIFY_EVT:
        ESP_LOGD(TAG, "reg_for_notify status=%d", param->reg_for_notify.status);
        if(param->reg_for_notify.status == ESP_GATT_OK) s_notify_registered = true;
        break;
    case ESP_GATTC_WRITE_DESCR_EVT:
        ESP_LOGD(TAG, "write_descr status=%d", param->write.status);
        s_cccd_written = true;
        break;
    case ESP_GATTC_NOTIFY_EVT:
        ESP_LOGD(TAG, "NOTIFY len=%u h=%u", param->notify.value_len, param->notify.handle);
        if(param->notify.value_len > 0) {
            size_t space = sizeof(s_acc) - s_acc_len;
            size_t n = param->notify.value_len;
            if(n > space) {
                /* Overflow — reset accumulator and keep latest chunk */
                s_acc_len = 0;
                n = (param->notify.value_len > sizeof(s_acc)) ? sizeof(s_acc) :
                                                                param->notify.value_len;
            }
            memcpy(s_acc + s_acc_len, param->notify.value, n);
            s_acc_len += n;
            cham_try_parse();
        }
        break;
    case ESP_GATTC_DISCONNECT_EVT:
        s_connected = false;
        break;
    default:
        break;
    }
}
#endif /* CONFIG_BT_NIMBLE_ENABLED */

/* --------------------------------------------------- HAL start / stop ---- */

static bool cham_hal_start(void) {
    if(s_hal_started) return true;

    Bt* bt = furi_record_open(RECORD_BT);
    /* If BT is disabled in settings the radio stack was never started, so the
     * ESP BT controller is uninitialized. Bring it up through the normal
     * service path first; otherwise
     * the controller takeover below dereferences an uninitialized controller
     * and crashes. */
    s_bt_was_disabled = !bt_is_enabled(bt);
    if(s_bt_was_disabled) {
        ESP_LOGI(TAG, "BT disabled in settings, force-starting stack first");
        bt_start_stack(bt);
        furi_delay_ms(300);
    }
    bt_stop_stack(bt);
    furi_record_close(RECORD_BT);
    furi_delay_ms(100);

#if CONFIG_BT_NIMBLE_ENABLED
    if(ble_core_stack_is_up()) {
        ESP_LOGE(TAG, "refusing Chameleon takeover while btshim still owns the NimBLE host");
        Bt* restore = furi_record_open(RECORD_BT);
        if(s_bt_was_disabled) {
            bt_stop_stack(restore);
            s_bt_was_disabled = false;
        } else {
            bt_start_stack(restore);
        }
        furi_record_close(RECORD_BT);
        return false;
    }
    esp_err_t err = ble_core_stack_up();
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE host start failed: %s", esp_err_to_name(err));
        (void)ble_core_stack_down();
        Bt* restore = furi_record_open(RECORD_BT);
        bt_start_stack(restore);
        if(s_bt_was_disabled) {
            furi_delay_ms(200);
            bt_stop_stack(restore);
            s_bt_was_disabled = false;
        }
        furi_record_close(RECORD_BT);
        return false;
    }
    s_hal_started = true;
    err = ble_core_register_gap_callback(cham_profile_gap_cb, NULL);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE GAP listener registration failed: %s", esp_err_to_name(err));
        cham_hal_stop();
        return false;
    }
    (void)ble_att_set_preferred_mtu(247);
    return true;
#else
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_bt_controller_init(&bt_cfg);
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) return false;
    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) return false;

    esp_bluedroid_config_t bd_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    err = esp_bluedroid_init_with_cfg(&bd_cfg);
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) return false;
    err = esp_bluedroid_enable();
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) return false;

    s_gattc_if = ESP_GATT_IF_NONE;
    s_connected = false;

    if(esp_ble_gap_register_callback(cham_gap_cb) != ESP_OK) return false;
    if(esp_ble_gattc_register_callback(cham_gattc_cb) != ESP_OK) return false;
    if(esp_ble_gattc_app_register(CHAM_GATTC_APP_ID) != ESP_OK) return false;

    for(int i = 0; i < 40 && s_gattc_if == ESP_GATT_IF_NONE; i++) furi_delay_ms(50);
    if(s_gattc_if == ESP_GATT_IF_NONE) return false;

    esp_ble_gatt_set_local_mtu(247);
    s_hal_started = true;
    return true;
#endif
}

static void cham_hal_stop(void) {
#if CONFIG_BT_NIMBLE_ENABLED
    if(s_scanning) {
        int rc = ble_gap_disc_cancel();
        if(rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGW(TAG, "NimBLE scan cancel: %d", rc);
        for(int i = 0; i < 40 && s_scanning; ++i) furi_delay_ms(5);
    }
    if(s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        int rc = ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        if(rc != 0 && rc != BLE_HS_ENOTCONN) ESP_LOGW(TAG, "NimBLE disconnect: %d", rc);
        for(int i = 0; i < 40 && s_connected; ++i) furi_delay_ms(50);
    }
    if(ble_core_stack_is_up()) {
        esp_err_t err = ble_core_stack_down();
        if(err != ESP_OK) {
            ESP_LOGE(TAG, "NimBLE host teardown failed: %s", esp_err_to_name(err));
            return;
        }
    }
    Bt* bt = furi_record_open(RECORD_BT);
    bt_start_stack(bt);
    if(s_bt_was_disabled) {
        furi_delay_ms(200);
        bt_stop_stack(bt);
        s_bt_was_disabled = false;
    }
    furi_record_close(RECORD_BT);

    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_connected = false;
    s_scanning = false;
    s_hal_started = false;
    s_device_mode = -1;
#else
    if(s_connected) {
        esp_ble_gattc_close(s_gattc_if, s_conn_id);
        furi_delay_ms(100);
    }
    if(s_scanning) {
        esp_ble_gap_stop_scanning();
        furi_delay_ms(50);
    }
    if(s_gattc_if != ESP_GATT_IF_NONE) {
        esp_ble_gattc_app_unregister(s_gattc_if);
        furi_delay_ms(50);
    }
    if(esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_UNINITIALIZED) {
        esp_bluedroid_disable();
        furi_delay_ms(50);
        esp_bluedroid_deinit();
        furi_delay_ms(50);
    }
    if(esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_IDLE) {
        esp_bt_controller_disable();
        furi_delay_ms(50);
    }
    if(esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
        esp_bt_controller_deinit();
        furi_delay_ms(50);
    }

    Bt* bt = furi_record_open(RECORD_BT);
    bt_start_stack(bt);
    if(s_bt_was_disabled) {
        /* BT was off in settings before we connected — put it back off so we
         * don't leave the device advertising against the user's choice. */
        furi_delay_ms(200);
        bt_stop_stack(bt);
        s_bt_was_disabled = false;
    }
    furi_record_close(RECORD_BT);

    s_gattc_if = ESP_GATT_IF_NONE;
    s_hal_started = false;
    s_device_mode = -1;
#endif
}

/* ----------------------------------------------------- public API -------- */

static bool chameleon_connect_nimble(volatile bool* abort_flag) {
    if(!cham_hal_start()) {
        ESP_LOGE(TAG, "NimBLE HAL start failed");
        cham_hal_stop();
        return false;
    }

    s_dev_found = false;
    s_connected = false;
    s_connect_complete = false;
    s_search_done = false;
    s_chars_done = false;
    s_descriptors_done = false;
    s_notify_registered = false;
    s_cccd_written = false;
    s_cccd_found = false;
    s_svc_start = s_svc_end = s_write_handle = s_notify_handle = 0;
    s_svc_count = 0;
    s_acc_len = 0;
    s_resp_ready = false;

    uint8_t own_addr_type = BLE_OWN_ADDR_PUBLIC;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if(rc != 0) {
        ESP_LOGE(TAG, "cannot infer NimBLE central address type: %d", rc);
        cham_hal_stop();
        return false;
    }
    const struct ble_gap_disc_params scan_params = {
        .itvl = 0x50,
        .window = 0x30,
        .filter_policy = BLE_HCI_SCAN_FILT_NO_WL,
        .limited = 0,
        .passive = 0,
        .filter_duplicates = 0,
    };
    rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &scan_params, cham_gap_cb, NULL);
    if(rc != 0) {
        ESP_LOGE(TAG, "NimBLE scan start failed: %d", rc);
        cham_hal_stop();
        return false;
    }
    s_scanning = true;
    for(int i = 0; i < 300 && !s_dev_found; ++i) {
        if(abort_flag && *abort_flag) {
            (void)ble_gap_disc_cancel();
            cham_hal_stop();
            return false;
        }
        furi_delay_ms(50);
    }
    if(!s_dev_found) {
        ESP_LOGW(TAG, "ChameleonUltra not found");
        cham_hal_stop();
        return false;
    }
    if(s_scanning) (void)ble_gap_disc_cancel();
    for(int i = 0; i < 40 && s_scanning; ++i) furi_delay_ms(5);

    ble_addr_t peer = {.type = s_dev_addr_type};
    for(size_t i = 0; i < sizeof(peer.val); ++i) peer.val[i] = s_dev_addr[5U - i];
    s_connect_complete = false;
    rc = ble_gap_connect(own_addr_type, &peer, 5000, NULL, cham_gap_cb, NULL);
    if(rc != 0) {
        ESP_LOGW(TAG, "NimBLE connection request failed: %d", rc);
        cham_hal_stop();
        return false;
    }
    for(int i = 0; i < 100 && !s_connect_complete; ++i) {
        if(abort_flag && *abort_flag) {
            (void)ble_gap_conn_cancel();
            break;
        }
        furi_delay_ms(50);
    }
    if(!s_connect_complete) {
        (void)ble_gap_conn_cancel();
        for(int i = 0; i < 60 && !s_connect_complete; ++i) furi_delay_ms(50);
    }
    if(!s_connected) {
        ESP_LOGW(TAG, "NimBLE connection timed out or failed");
        cham_hal_stop();
        return false;
    }

    s_search_done = false;
    s_svc_count = 0;
    rc = ble_gattc_disc_all_svcs(s_conn_handle, cham_service_cb, NULL);
    if(rc != 0) {
        ESP_LOGW(TAG, "NimBLE service discovery start failed: %d", rc);
        cham_hal_stop();
        return false;
    }
    for(int i = 0; i < 150 && !s_search_done; ++i) furi_delay_ms(20);
    if(!s_search_done) {
        ESP_LOGW(TAG, "NimBLE service discovery timed out");
        cham_hal_stop();
        return false;
    }
    for(uint16_t i = 0; i < s_svc_count; ++i) {
        if(uuid_is(&s_svcs[i].uuid, NUS_SVC)) {
            s_svc_start = s_svcs[i].start;
            s_svc_end = s_svcs[i].end;
            break;
        }
    }
    if(s_svc_start == 0) {
        ESP_LOGW(TAG, "NUS service not found among %u services", s_svc_count);
        cham_hal_stop();
        return false;
    }

    s_chars_done = false;
    rc = ble_gattc_disc_all_chrs(
        s_conn_handle, s_svc_start, s_svc_end, cham_characteristic_cb, NULL);
    if(rc != 0) {
        ESP_LOGW(TAG, "NimBLE characteristic discovery start failed: %d", rc);
        cham_hal_stop();
        return false;
    }
    for(int i = 0; i < 100 && !s_chars_done; ++i) furi_delay_ms(20);
    if(!s_chars_done || !s_write_handle || !s_notify_handle) {
        ESP_LOGW(TAG, "NUS characteristics missing (write=%u notify=%u)",
                 s_write_handle, s_notify_handle);
        cham_hal_stop();
        return false;
    }

    s_descriptors_done = false;
    s_cccd_found = false;
    s_cccd_handle = 0;
    s_cccd_written = false;
    s_notify_registered = false;
    rc = ble_gattc_disc_all_dscs(
        s_conn_handle, s_notify_handle + 1U, s_svc_end, cham_descriptor_cb, NULL);
    if(rc != 0) {
        ESP_LOGW(TAG, "NimBLE CCCD discovery start failed: %d", rc);
        cham_hal_stop();
        return false;
    }
    for(int i = 0; i < 100 && !s_descriptors_done; ++i) furi_delay_ms(20);
    if(s_cccd_found && s_cccd_handle) {
        const uint8_t cccd_value[2] = {0x01, 0x00};
        rc = ble_gattc_write_flat(s_conn_handle, s_cccd_handle, cccd_value,
                                  sizeof(cccd_value), cham_cccd_write_cb, NULL);
        if(rc != 0) {
            ESP_LOGW(TAG, "NimBLE CCCD write request failed: %d", rc);
            cham_hal_stop();
            return false;
        }
        for(int i = 0; i < 100 && !s_cccd_written; ++i) furi_delay_ms(20);
    }
    if(!s_cccd_found || !s_cccd_written || !s_notify_registered) {
        ESP_LOGW(TAG, "NUS notification subscription failed (found=%d written=%d)",
                 s_cccd_found, s_cccd_written);
        cham_hal_stop();
        return false;
    }

    ESP_LOGD(TAG, "NimBLE connected and subscribed to NUS notifications");
    return true;
}

bool chameleon_connect(volatile bool* abort_flag) {
    if(s_connected) return true;
#if CONFIG_BT_NIMBLE_ENABLED
    return chameleon_connect_nimble(abort_flag);
#else
    if(!cham_hal_start()) {
        ESP_LOGE(TAG, "HAL start failed");
        cham_hal_stop();
        return false;
    }

    /* Scan for the device by name */
    s_dev_found = false;
    s_connected = false;
    s_search_done = false;
    s_notify_registered = false;
    s_cccd_written = false;
    s_svc_start = s_svc_end = s_write_handle = s_notify_handle = 0;
    s_svc_count = 0;
    s_acc_len = 0;
    s_resp_ready = false;

    esp_ble_scan_params_t sp = {
        .scan_type = BLE_SCAN_TYPE_ACTIVE,
        .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval = 0x50,
        .scan_window = 0x30,
    };
    esp_ble_gap_set_scan_params(&sp); /* scan starts in GAP cb */

    for(int i = 0; i < 300 && !s_dev_found; i++) { /* up to ~15 s */
        if(abort_flag && *abort_flag) {
            esp_ble_gap_stop_scanning();
            cham_hal_stop();
            return false;
        }
        furi_delay_ms(50);
    }
    if(!s_dev_found) {
        ESP_LOGW(TAG, "device not found");
        cham_hal_stop();
        return false;
    }
    esp_ble_gap_stop_scanning();
    furi_delay_ms(100);

    /* Connect */
    if(esp_ble_gattc_open(s_gattc_if, s_dev_addr, s_dev_addr_type, true) != ESP_OK) {
        cham_hal_stop();
        return false;
    }
    for(int i = 0; i < 100 && !s_connected; i++) {
        if(abort_flag && *abort_flag) {
            esp_ble_gattc_close(s_gattc_if, 0);
            cham_hal_stop();
            return false;
        }
        furi_delay_ms(50);
    }
    if(!s_connected) {
        ESP_LOGW(TAG, "connect timeout");
        cham_hal_stop();
        return false;
    }

    /* Discover ALL services (NULL filter — a 128-bit UUID filter can be
     * unreliable with a peer's cached GATT database), then
     * match the NUS service from the collected list. */
    s_search_done = false;
    s_svc_count = 0;
    esp_ble_gattc_search_service(s_gattc_if, s_conn_id, NULL);
    for(int i = 0; i < 150 && !s_search_done; i++) furi_delay_ms(20);
    if(!s_search_done) {
        ESP_LOGW(TAG, "service discovery timeout");
        cham_hal_stop();
        return false;
    }
    for(int i = 0; i < s_svc_count; i++) {
        if(uuid_is(&s_svcs[i].uuid, NUS_SVC)) {
            s_svc_start = s_svcs[i].start;
            s_svc_end = s_svcs[i].end;
            break;
        }
    }
    if(s_svc_start == 0) {
        ESP_LOGW(TAG, "NUS service not among %u services", s_svc_count);
        cham_hal_stop();
        return false;
    }
    ESP_LOGD(TAG, "NUS svc handles %u..%u", s_svc_start, s_svc_end);

    /* Find TX (write) + RX (notify) characteristic handles */
    uint16_t count = 16;
    esp_gattc_char_elem_t chars[16];
    if(esp_ble_gattc_get_all_char(
           s_gattc_if, s_conn_id, s_svc_start, s_svc_end, chars, &count, 0) != ESP_GATT_OK) {
        cham_hal_stop();
        return false;
    }
    for(int i = 0; i < count; i++) {
        uuid_log("CHR", &chars[i].uuid);
        if(uuid_is(&chars[i].uuid, NUS_TX)) s_write_handle = chars[i].char_handle;
        else if(uuid_is(&chars[i].uuid, NUS_RX))
            s_notify_handle = chars[i].char_handle;
    }
    if(s_write_handle == 0 || s_notify_handle == 0) {
        ESP_LOGW(TAG, "NUS chars missing (w=%u n=%u)", s_write_handle, s_notify_handle);
        cham_hal_stop();
        return false;
    }

    /* Subscribe to notifications: register + write CCCD = 0x0001 */
    s_notify_registered = false;
    esp_ble_gattc_register_for_notify(s_gattc_if, s_dev_addr, s_notify_handle);
    for(int i = 0; i < 100 && !s_notify_registered; i++) furi_delay_ms(20);
    if(!s_notify_registered) {
        ESP_LOGW(TAG, "register_for_notify failed");
        cham_hal_stop();
        return false;
    }

    esp_bt_uuid_t cccd = {.len = ESP_UUID_LEN_16, .uuid.uuid16 = CCCD_UUID};
    esp_gattc_descr_elem_t descr;
    uint16_t dcount = 1;
    esp_gatt_status_t ds = esp_ble_gattc_get_descr_by_char_handle(
        s_gattc_if, s_conn_id, s_notify_handle, cccd, &descr, &dcount);
    if(ds == ESP_GATT_OK && dcount > 0) {
        uint8_t v[2] = {0x01, 0x00};
        s_cccd_written = false;
        esp_ble_gattc_write_char_descr(
            s_gattc_if, s_conn_id, descr.handle, sizeof(v), v,
            ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
        for(int i = 0; i < 60 && !s_cccd_written; i++) furi_delay_ms(20);
        ESP_LOGD(
            TAG, "CCCD handle=%u written=%d", descr.handle, (int)s_cccd_written);
    } else {
        ESP_LOGW(TAG, "CCCD descr not found (ds=%d dcount=%u)", ds, dcount);
    }

    ESP_LOGD(TAG, "connected & subscribed");
    return true;
#endif
}

void chameleon_disconnect(void) {
    if(!s_hal_started) return;
    cham_hal_stop();
    s_connected = false;
}

bool chameleon_is_connected(void) {
    return s_connected;
}

bool chameleon_cmd(
    uint16_t cmd,
    const uint8_t* data,
    uint16_t len,
    ChameleonResp* out,
    uint32_t timeout_ms) {
    if(!s_connected || s_write_handle == 0) return false;
    if(len > CHAMELEON_RESP_DATA_MAX) return false;

    uint8_t frame[CHAMELEON_RESP_DATA_MAX + 16];
    frame[0] = 0x11;
    frame[1] = 0xEF;
    frame[2] = (cmd >> 8) & 0xFF;
    frame[3] = cmd & 0xFF;
    frame[4] = 0x00;
    frame[5] = 0x00;
    frame[6] = (len >> 8) & 0xFF;
    frame[7] = len & 0xFF;
    frame[8] = cham_lrc(&frame[2], 6);
    if(len) memcpy(&frame[9], data, len);
    frame[9 + len] = cham_lrc(&frame[9], len);

    s_resp_ready = false;
    s_acc_len = 0;

#if CONFIG_BT_NIMBLE_ENABLED
    const uint16_t frame_len = 10U + len;
    const uint16_t mtu = ble_att_mtu(s_conn_handle);
    int write_rc;
    if(mtu >= 3U && frame_len <= mtu - 3U) {
        write_rc = ble_gattc_write_flat(
            s_conn_handle, s_write_handle, frame, frame_len, cham_write_done_cb, NULL);
    } else {
        struct os_mbuf* om = ble_hs_mbuf_from_flat(frame, frame_len);
        if(!om) return false;
        write_rc = ble_gattc_write_long(
            s_conn_handle, s_write_handle, 0, om, cham_write_done_cb, NULL);
    }
    if(write_rc != 0) {
        ESP_LOGW(TAG, "NimBLE GATT write request failed: %d", write_rc);
        return false;
    }
#else
    esp_err_t e = esp_ble_gattc_write_char(
        s_gattc_if, s_conn_id, s_write_handle, 10 + len, frame,
        ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
    ESP_LOGD(TAG, "cmd %u write -> %s (wh=%u)", cmd, esp_err_to_name(e), s_write_handle);
    if(e != ESP_OK) {
        ESP_LOGW(TAG, "write_char: %s", esp_err_to_name(e));
        return false;
    }
#endif

    uint32_t waited = 0;
    while(!s_resp_ready && waited < timeout_ms) {
        furi_delay_ms(10);
        waited += 10;
    }
    if(!s_resp_ready) {
        ESP_LOGW(TAG, "cmd %u TIMEOUT (no notify in %lums)", cmd, (unsigned long)timeout_ms);
        return false;
    }
    ESP_LOGD(
        TAG,
        "cmd %u resp: rxcmd=%u status=%02X len=%u",
        cmd,
        s_last_resp.command,
        s_last_resp.status,
        s_last_resp.data_len);
    if(out) *out = s_last_resp;
    return true;
}

bool chameleon_mf1_write_block(
    uint8_t block,
    uint8_t key_type,
    const uint8_t key[6],
    const uint8_t data16[16]) {
    uint8_t payload[2 + 6 + 16];
    payload[0] = key_type; /* 0x60 = key A, 0x61 = key B */
    payload[1] = block;
    memcpy(&payload[2], key, 6);
    memcpy(&payload[8], data16, 16);

    ChameleonResp r;
    if(!chameleon_cmd(ChameleonCmdMf1WriteOneBlock, payload, sizeof(payload), &r, 1500))
        return false;
    return r.status == CHAMELEON_STATUS_HF_TAG_OK;
}

bool chameleon_mfu_write_page(uint8_t page, const uint8_t data4[4]) {
    /* HF14A_RAW WRITE (0xA2 page d0 d1 d2 d3); the card answers a 4-bit ACK,
     * so accept HF_TAG_OK regardless of returned data length. */
    uint8_t frame[6] = {0xA2, page, data4[0], data4[1], data4[2], data4[3]};
    uint8_t payload[5 + sizeof(frame)];
    payload[0] = CHAMELEON_RAW_OPT_DEFAULT;
    payload[1] = 0x00;
    payload[2] = 200;
    payload[3] = 0x00;
    payload[4] = (uint8_t)(sizeof(frame) * 8);
    memcpy(&payload[5], frame, sizeof(frame));

    ChameleonResp r;
    if(!chameleon_cmd(ChameleonCmdHf14aRaw, payload, sizeof(payload), &r, 1500))
        return false;
    return r.status == CHAMELEON_STATUS_HF_TAG_OK;
}

static bool chameleon_cmd_ok(uint16_t cmd, const uint8_t* d, uint16_t n) {
    ChameleonResp r;
    if(!chameleon_cmd(cmd, d, n, &r, 1500)) return false;
    return r.status == CHAMELEON_STATUS_SUCCESS;
}

bool chameleon_slot_select(uint8_t slot, uint16_t tag_type) {
    if(slot < 1 || slot > 8) return false;
    uint8_t s = slot - 1;

    uint8_t en[3] = {s, 0x02 /* HF */, 0x01};
    if(!chameleon_cmd_ok(ChameleonCmdSetSlotEnable, en, sizeof(en))) return false;

    uint8_t act[1] = {s};
    if(!chameleon_cmd_ok(ChameleonCmdSetActiveSlot, act, sizeof(act))) return false;

    uint8_t ty[3] = {s, (uint8_t)(tag_type >> 8), (uint8_t)(tag_type & 0xFF)};
    if(!chameleon_cmd_ok(ChameleonCmdSetSlotTagType, ty, sizeof(ty))) return false;

    /* Initialise the slot to defaults for this tag type — required before
     * loading emulation data, otherwise the emulated card stays inert. */
    if(!chameleon_cmd_ok(ChameleonCmdSetSlotDataDefault, ty, sizeof(ty))) return false;

    return true;
}

bool chameleon_slot_config_save(void) {
    return chameleon_cmd_ok(ChameleonCmdSlotDataConfigSave, NULL, 0);
}

bool chameleon_mf1_eload(uint8_t start_block, const uint8_t* data, uint16_t nblocks) {
    /* Keep each BLE write well under the ATT MTU: 8 blocks (128 B) per chunk */
    const uint16_t chunk_blocks = 8;
    uint8_t buf[1 + chunk_blocks * 16];
    for(uint16_t b = 0; b < nblocks; b += chunk_blocks) {
        uint16_t cnt = (nblocks - b < chunk_blocks) ? (nblocks - b) : chunk_blocks;
        buf[0] = (uint8_t)(start_block + b);
        memcpy(&buf[1], &data[(size_t)b * 16], (size_t)cnt * 16);
        if(!chameleon_cmd_ok(ChameleonCmdMf1WriteEmuBlockData, buf, 1 + cnt * 16))
            return false;
    }
    return true;
}

bool chameleon_mfu_eload(uint8_t start_page, const uint8_t* data, uint16_t npages) {
    /* MF0_NTAG_WRITE_EMU_PAGE_DATA payload is [page_start, page_count, data]
     * (unlike MF1_WRITE_EMU_BLOCK_DATA which is just [block, data]). Omitting
     * the count byte shifts all pages by one and the clone reads back wrong. */
    const uint16_t chunk_pages = 30; /* 2 + 120 B */
    uint8_t buf[2 + chunk_pages * 4];
    for(uint16_t p = 0; p < npages; p += chunk_pages) {
        uint16_t cnt = (npages - p < chunk_pages) ? (npages - p) : chunk_pages;
        buf[0] = (uint8_t)(start_page + p);
        buf[1] = (uint8_t)cnt;
        memcpy(&buf[2], &data[(size_t)p * 4], (size_t)cnt * 4);
        if(!chameleon_cmd_ok(ChameleonCmdMf0NtagWriteEmuPageData, buf, 2 + cnt * 4))
            return false;
    }
    return true;
}

bool chameleon_set_anticoll(
    const uint8_t* uid,
    uint8_t uid_len,
    const uint8_t atqa[2],
    uint8_t sak) {
    if(uid_len != 4 && uid_len != 7) return false;
    uint8_t p[1 + 10 + 4];
    uint8_t i = 0;
    p[i++] = uid_len;
    memcpy(&p[i], uid, uid_len);
    i += uid_len;
    p[i++] = atqa[1];
    p[i++] = atqa[0];
    p[i++] = sak;
    p[i++] = 0x00; /* ATS length */
    return chameleon_cmd_ok(ChameleonCmdHf14aSetAntiCollData, p, i);
}

bool chameleon_get_app_version(uint8_t* major, uint8_t* minor) {
    ChameleonResp r;
    if(!chameleon_cmd(ChameleonCmdGetAppVersion, NULL, 0, &r, 1000)) return false;
    if(r.status != CHAMELEON_STATUS_SUCCESS || r.data_len < 2) return false;
    if(major) *major = r.data[1]; /* device sends [minor, major] */
    if(minor) *minor = r.data[0];
    return true;
}

bool chameleon_get_battery(uint16_t* millivolt, uint8_t* percent) {
    ChameleonResp r;
    if(!chameleon_cmd(ChameleonCmdGetBatteryInfo, NULL, 0, &r, 1000)) return false;
    if(r.status != CHAMELEON_STATUS_SUCCESS || r.data_len < 3) return false;
    if(millivolt) *millivolt = ((uint16_t)r.data[0] << 8) | r.data[1];
    if(percent) *percent = r.data[2];
    return true;
}

bool chameleon_set_device_mode(uint8_t mode) {
    if(s_device_mode == (int)mode) return true;
    ChameleonResp r;
    if(!chameleon_cmd(ChameleonCmdChangeMode, &mode, 1, &r, 1000)) return false;
    if(r.status != CHAMELEON_STATUS_SUCCESS) return false;
    s_device_mode = (int)mode;
    return true;
}

bool chameleon_hf14a_scan(uint8_t* uid, uint8_t* uid_len, uint8_t atqa[2], uint8_t* sak) {
    ChameleonResp r;
    if(!chameleon_cmd(ChameleonCmdHf14aScan, NULL, 0, &r, 1000)) return false;
    if(r.status != CHAMELEON_STATUS_HF_TAG_OK || r.data_len < 5) return false;
    /* Response data: [uid_len][uid...][atqa1][atqa0][sak] */
    uint8_t ul = r.data[0];
    if(ul == 0 || ul > 10 || (size_t)(4 + ul) > r.data_len) return false;
    if(uid) memcpy(uid, &r.data[1], ul);
    if(uid_len) *uid_len = ul;
    if(atqa) {
        atqa[1] = r.data[1 + ul];
        atqa[0] = r.data[2 + ul];
    }
    if(sak) *sak = r.data[3 + ul];
    return true;
}

bool chameleon_mf1_read_block(
    uint8_t block,
    uint8_t key_type,
    const uint8_t key[6],
    uint8_t out16[16]) {
    uint8_t payload[8];
    payload[0] = key_type; /* 0x60 = key A, 0x61 = key B */
    payload[1] = block;
    memcpy(&payload[2], key, 6);

    ChameleonResp r;
    if(!chameleon_cmd(ChameleonCmdMf1ReadOneBlock, payload, sizeof(payload), &r, 1200))
        return false;
    if(r.status != CHAMELEON_STATUS_HF_TAG_OK || r.data_len < 16) return false;
    memcpy(out16, r.data, 16);
    return true;
}

bool chameleon_hf14a_raw(
    const uint8_t* data,
    uint8_t len,
    uint8_t* out,
    uint16_t out_cap,
    uint16_t* out_len) {
    if(len == 0 || len > 240) return false;

    uint8_t payload[5 + 240];
    payload[0] = CHAMELEON_RAW_OPT_DEFAULT;
    payload[1] = 0x00;
    payload[2] = 200; /* timeout */
    payload[3] = 0x00;
    payload[4] = (uint8_t)(len * 8); /* bitlen */
    memcpy(&payload[5], data, len);

    ChameleonResp r;
    if(!chameleon_cmd(ChameleonCmdHf14aRaw, payload, 5 + len, &r, 1200)) return false;
    if(r.status != CHAMELEON_STATUS_HF_TAG_OK || r.data_len == 0) return false;
    uint16_t n = r.data_len;
    if(n > out_cap) n = out_cap;
    memcpy(out, r.data, n);
    if(out_len) *out_len = n;
    return true;
}
