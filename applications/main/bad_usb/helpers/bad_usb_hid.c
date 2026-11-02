#include "bad_usb_hid.h"

#include <ble_hid/ble_hid.h>
#include <btshim.h>
#include <soc/soc_caps.h>
#include <storage/storage.h>

#define TAG "BadUSB HID"
#define HID_BT_KEYS_STORAGE_NAME ".bt_hid.keys"

static bool ble_profile_dirty;

bool bad_usb_hid_supports_usb(void) {
#if defined(SOC_USB_OTG_SUPPORTED) && SOC_USB_OTG_SUPPORTED
    return true;
#else
    return false;
#endif
}

void hid_usb_adjust_config(BadUsbHidConfig* hid_cfg) {
    if(hid_cfg->usb.vid == 0) hid_cfg->usb.vid = HID_VID_DEFAULT;
    if(hid_cfg->usb.pid == 0) hid_cfg->usb.pid = HID_PID_DEFAULT;
}

bool hid_usb_init(BadUsbHidConfig* hid_cfg, void** inst) {
    furi_assert(inst);
    *inst = NULL;
    hid_usb_adjust_config(hid_cfg);
    if(!bad_usb_hid_supports_usb()) {
        FURI_LOG_W(TAG, "USB HID is not supported on this target");
        return false;
    }

    if(!furi_hal_usb_set_config(&usb_hid, &hid_cfg->usb)) {
        FURI_LOG_W(TAG, "Failed to enable USB HID");
        return false;
    }

    return true;
}

void hid_usb_deinit(void* inst) {
    UNUSED(inst);
    if(!bad_usb_hid_supports_usb()) {
        return;
    }

    furi_check(furi_hal_usb_set_config(NULL, NULL));
    /* Leaving usb_hid only changes the active interface; esp_tinyusb remains
     * installed. BadUSB can sit on file selection after stopping a script, so
     * release the stack here to detach USB immediately, not only on app exit. */
    if(!furi_hal_usb_hid_release_stack()) {
        FURI_LOG_E(TAG, "Failed to release the USB HID stack after stop");
    }
}

void hid_usb_set_state_callback(void* inst, HidStateCallback cb, void* context) {
    UNUSED(inst);
    if(!bad_usb_hid_supports_usb()) {
        if(cb) {
            cb(false, context);
        }
        return;
    }
    furi_hal_hid_set_state_callback(cb, context);
}

bool hid_usb_is_connected(void* inst) {
    UNUSED(inst);
    return bad_usb_hid_supports_usb() && furi_hal_hid_is_connected();
}

bool hid_usb_kb_press(void* inst, uint16_t button) {
    UNUSED(inst);
    return bad_usb_hid_supports_usb() && furi_hal_hid_kb_press(button);
}

bool hid_usb_kb_release(void* inst, uint16_t button) {
    UNUSED(inst);
    return bad_usb_hid_supports_usb() && furi_hal_hid_kb_release(button);
}

bool hid_usb_mouse_press(void* inst, uint8_t button) {
    UNUSED(inst);
    return bad_usb_hid_supports_usb() && furi_hal_hid_mouse_press(button);
}

bool hid_usb_mouse_release(void* inst, uint8_t button) {
    UNUSED(inst);
    return bad_usb_hid_supports_usb() && furi_hal_hid_mouse_release(button);
}

bool hid_usb_mouse_scroll(void* inst, int8_t delta) {
    UNUSED(inst);
    return bad_usb_hid_supports_usb() && furi_hal_hid_mouse_scroll(delta);
}

bool hid_usb_mouse_move(void* inst, int8_t dx, int8_t dy) {
    UNUSED(inst);
    return bad_usb_hid_supports_usb() && furi_hal_hid_mouse_move(dx, dy);
}

bool hid_usb_mouse_release_all(void* inst) {
    UNUSED(inst);
    return bad_usb_hid_supports_usb() && furi_hal_hid_mouse_release(0);
}

bool hid_usb_consumer_press(void* inst, uint16_t button) {
    UNUSED(inst);
    return bad_usb_hid_supports_usb() && furi_hal_hid_consumer_key_press(button);
}

bool hid_usb_consumer_release(void* inst, uint16_t button) {
    UNUSED(inst);
    return bad_usb_hid_supports_usb() && furi_hal_hid_consumer_key_release(button);
}

bool hid_usb_release_all(void* inst) {
    UNUSED(inst);
    if(!bad_usb_hid_supports_usb()) {
        return false;
    }

    bool state = furi_hal_hid_kb_release_all();
    state &= furi_hal_hid_consumer_key_release_all();
    state &= hid_usb_mouse_release_all(inst);
    return state;
}

uint8_t hid_usb_get_led_state(void* inst) {
    UNUSED(inst);
    return bad_usb_hid_supports_usb() ? furi_hal_hid_get_led_state() : 0;
}

static const BadUsbHidApi hid_api_usb = {
    .adjust_config = hid_usb_adjust_config,
    .init = hid_usb_init,
    .deinit = hid_usb_deinit,
    .set_state_callback = hid_usb_set_state_callback,
    .is_connected = hid_usb_is_connected,

    .kb_press = hid_usb_kb_press,
    .kb_release = hid_usb_kb_release,
    .mouse_press = hid_usb_mouse_press,
    .mouse_release = hid_usb_mouse_release,
    .mouse_scroll = hid_usb_mouse_scroll,
    .mouse_move = hid_usb_mouse_move,
    .consumer_press = hid_usb_consumer_press,
    .consumer_release = hid_usb_consumer_release,
    .release_all = hid_usb_release_all,
    .get_led_state = hid_usb_get_led_state,
};

typedef struct {
    Bt* bt;
    FuriHalBleProfileBase* profile;
    HidStateCallback state_callback;
    void* callback_context;
    volatile bool is_connected;
} BleHidInstance;

static void hid_ble_reverse_mac(uint8_t dst[GAP_MAC_ADDR_SIZE], const uint8_t src[GAP_MAC_ADDR_SIZE]) {
    for(size_t i = 0; i < GAP_MAC_ADDR_SIZE; i++) {
        dst[i] = src[GAP_MAC_ADDR_SIZE - 1 - i];
    }
}

static bool hid_ble_mac_is_empty(const uint8_t mac[GAP_MAC_ADDR_SIZE]) {
    for(size_t i = 0; i < GAP_MAC_ADDR_SIZE; i++) {
        if(mac[i] != 0) {
            return false;
        }
    }
    return true;
}

static void hid_ble_connection_status_callback(BtStatus status, void* context) {
    BleHidInstance* ble_hid = context;
    furi_assert(ble_hid);

    ble_hid->is_connected = (status == BtStatusConnected);
    if(ble_hid->state_callback) {
        ble_hid->state_callback(ble_hid->is_connected, ble_hid->callback_context);
    }
}

void hid_ble_adjust_config(BadUsbHidConfig* hid_cfg) {
    uint8_t default_mac[GAP_MAC_ADDR_SIZE];

    if(hid_ble_mac_is_empty(hid_cfg->ble.mac)) {
        ble_hid_get_default_mac(default_mac);
        hid_ble_reverse_mac(hid_cfg->ble.mac, default_mac);
    }

    if(hid_cfg->ble.name[0] == '\0') {
        snprintf(
            hid_cfg->ble.name,
            sizeof(hid_cfg->ble.name),
            "BadUSB %s",
            furi_hal_version_get_name_ptr());
    }

    if(hid_cfg->ble.pairing >= GapPairingCount) {
        hid_cfg->ble.pairing = GapPairingPinCodeVerifyYesNo;
    }
}

bool hid_ble_init(BadUsbHidConfig* hid_cfg, void** inst) {
    furi_assert(inst);
    *inst = NULL;
    BleHidInstance* ble_hid = calloc(1, sizeof(BleHidInstance));
    if(!ble_hid) {
        FURI_LOG_E(TAG, "Failed to allocate BLE HID instance");
        return false;
    }

    hid_ble_adjust_config(hid_cfg);
    FURI_LOG_I(
        TAG,
        "hid_ble_init name=%s mac=%02x:%02x:%02x:%02x:%02x:%02x bonding=%d pairing=%d",
        hid_cfg->ble.name,
        hid_cfg->ble.mac[0],
        hid_cfg->ble.mac[1],
        hid_cfg->ble.mac[2],
        hid_cfg->ble.mac[3],
        hid_cfg->ble.mac[4],
        hid_cfg->ble.mac[5],
        hid_cfg->ble.bonding,
        hid_cfg->ble.pairing);
    ble_hid->bt = furi_record_open(RECORD_BT);
    bt_disconnect(ble_hid->bt);

    bt_keys_storage_set_storage_path(ble_hid->bt, APP_DATA_PATH(HID_BT_KEYS_STORAGE_NAME));
    /* bt_profile_start stops the active profile before attempting the new one. */
    ble_profile_dirty = true;
    ble_hid->profile = bt_profile_start(ble_hid->bt, ble_profile_hid_ext, &hid_cfg->ble);
    if(!ble_hid->profile) {
        FURI_LOG_E(TAG, "Failed to initialize BLE HID backend");
        /* Undo the app-local keys path we set above so the global BT service
         * does not keep loading/saving bonds from BadUSB's private file after
         * a failed start. Mirror the cleanup hid_ble_deinit does on success. */
        bt_keys_storage_set_default_path(ble_hid->bt);
        furi_record_close(RECORD_BT);
        free(ble_hid);
        return false;
    }
    FURI_LOG_I(TAG, "hid_ble_init profile started");

    /* Install the callback before advertising. A fast host can connect as soon
     * as advertising begins; registering afterwards can lose the only
     * connected transition, leaving BadUSB waiting in NotConnected/WillRun. */
    bt_set_status_changed_callback(ble_hid->bt, hid_ble_connection_status_callback, ble_hid);
    if(bt_is_enabled(ble_hid->bt)) {
        furi_hal_bt_start_advertising();
    }
    *inst = ble_hid;
    return true;
}

void hid_ble_deinit(void* inst) {
    BleHidInstance* ble_hid = inst;
    /* inst is NULL when hid_ble_init() failed (bt_profile_start returned NULL):
     * it frees its instance and returns NULL, but the worker's teardown still
     * calls deinit(hid_inst). furi_assert() only traps in FURI_DEBUG builds; a
     * release build fell through to ble_hid->bt and dereferenced NULL. Bail out
     * instead, matching every other hid_ble_* accessor and hid_usb_deinit. */
    if(!ble_hid) {
        FURI_LOG_W(TAG, "hid_ble_deinit skipped: instance is NULL");
        return;
    }

    uint32_t deinit_start = furi_get_tick();
    FURI_LOG_I(TAG, "hid_ble_deinit begin connected=%d", ble_hid->is_connected);
    if(ble_hid->bt) {
        bool was_connected = ble_hid->is_connected;
        FURI_LOG_I(TAG, "hid_ble_deinit unregister status callback begin");
        bt_set_status_changed_callback(ble_hid->bt, NULL, NULL);
        FURI_LOG_I(TAG, "hid_ble_deinit unregister status callback end");
        if(was_connected) {
            FURI_LOG_I(TAG, "hid_ble_deinit disconnect begin");
            bt_disconnect(ble_hid->bt);
            /* Allow the BLE stack to persist the just-disconnected bond. */
            furi_delay_ms(200);
            FURI_LOG_I(TAG, "hid_ble_deinit disconnect end");
        }
        FURI_LOG_I(TAG, "hid_ble_deinit stop advertising begin");
        furi_hal_bt_stop_advertising();
        FURI_LOG_I(TAG, "hid_ble_deinit stop advertising end");
        FURI_LOG_I(TAG, "hid_ble_deinit reset keys path begin");
        bt_keys_storage_set_default_path(ble_hid->bt);
        FURI_LOG_I(TAG, "hid_ble_deinit reset keys path end");
        /* A script can switch from BLE to USB without closing BadUSB. Restore
         * the serial profile here so the HID GATT server does not remain the
         * active profile while USB HID is in use. Queue it on the BT service:
         * profile teardown/startup performs asynchronous GATT work and must
         * not block the BadUSB worker or Back handling. */
        if(ble_profile_dirty) {
            bt_profile_restore_default_async(ble_hid->bt);
            ble_profile_dirty = false;
            FURI_LOG_I(TAG, "Default BLE profile restoration queued on HID deinit");
        }
        FURI_LOG_I(TAG, "hid_ble_deinit close BT record begin");
        furi_record_close(RECORD_BT);
        FURI_LOG_I(TAG, "hid_ble_deinit close BT record end");
    }

    free(ble_hid);
    FURI_LOG_I(
        TAG,
        "hid_ble_deinit end duration=%lu ms",
        (unsigned long)(furi_get_tick() - deinit_start));
}

void bad_usb_hid_ble_restore_default(void) {
    if(!ble_profile_dirty) return;

    Bt* bt = furi_record_open(RECORD_BT);
    bt_keys_storage_set_default_path(bt);
    bt_profile_restore_default_async(bt);
    ble_profile_dirty = false;
    furi_record_close(RECORD_BT);
    FURI_LOG_I(TAG, "Default BLE profile restoration queued");
}

void hid_ble_set_state_callback(void* inst, HidStateCallback cb, void* context) {
    BleHidInstance* ble_hid = inst;
    if(!ble_hid) {
        if(cb) {
            cb(false, context);
        }
        return;
    }

    ble_hid->state_callback = cb;
    ble_hid->callback_context = context;
    if(cb) {
        cb(ble_hid->is_connected, context);
    }
}

bool hid_ble_is_connected(void* inst) {
    BleHidInstance* ble_hid = inst;
    return ble_hid ? ble_hid->is_connected : false;
}

bool hid_ble_kb_press(void* inst, uint16_t button) {
    BleHidInstance* ble_hid = inst;
    return (ble_hid && ble_hid->profile) ? ble_profile_hid_kb_press(ble_hid->profile, button) :
                                           false;
}

bool hid_ble_kb_release(void* inst, uint16_t button) {
    BleHidInstance* ble_hid = inst;
    return (ble_hid && ble_hid->profile) ? ble_profile_hid_kb_release(ble_hid->profile, button) :
                                           false;
}

bool hid_ble_mouse_press(void* inst, uint8_t button) {
    BleHidInstance* ble_hid = inst;
    return (ble_hid && ble_hid->profile) ? ble_profile_hid_mouse_press(ble_hid->profile, button) :
                                           false;
}

bool hid_ble_mouse_release(void* inst, uint8_t button) {
    BleHidInstance* ble_hid = inst;
    return (ble_hid && ble_hid->profile) ?
               ble_profile_hid_mouse_release(ble_hid->profile, button) :
               false;
}

bool hid_ble_mouse_scroll(void* inst, int8_t delta) {
    BleHidInstance* ble_hid = inst;
    return (ble_hid && ble_hid->profile) ? ble_profile_hid_mouse_scroll(ble_hid->profile, delta) :
                                           false;
}

bool hid_ble_mouse_move(void* inst, int8_t dx, int8_t dy) {
    BleHidInstance* ble_hid = inst;
    return (ble_hid && ble_hid->profile) ? ble_profile_hid_mouse_move(ble_hid->profile, dx, dy) :
                                           false;
}

bool hid_ble_consumer_press(void* inst, uint16_t button) {
    BleHidInstance* ble_hid = inst;
    return (ble_hid && ble_hid->profile) ?
               ble_profile_hid_consumer_key_press(ble_hid->profile, button) :
               false;
}

bool hid_ble_consumer_release(void* inst, uint16_t button) {
    BleHidInstance* ble_hid = inst;
    return (ble_hid && ble_hid->profile) ?
               ble_profile_hid_consumer_key_release(ble_hid->profile, button) :
               false;
}

bool hid_ble_release_all(void* inst) {
    BleHidInstance* ble_hid = inst;
    bool state = false;

    if(ble_hid && ble_hid->profile) {
        state = ble_profile_hid_kb_release_all(ble_hid->profile);
        state &= ble_profile_hid_consumer_key_release_all(ble_hid->profile);
        state &= ble_profile_hid_mouse_release_all(ble_hid->profile);
    }

    return state;
}

uint8_t hid_ble_get_led_state(void* inst) {
    BleHidInstance* ble_hid = inst;
    return (ble_hid && ble_hid->profile) ? ble_profile_hid_get_led_state(ble_hid->profile) : 0;
}

static const BadUsbHidApi hid_api_ble = {
    .adjust_config = hid_ble_adjust_config,
    .init = hid_ble_init,
    .deinit = hid_ble_deinit,
    .set_state_callback = hid_ble_set_state_callback,
    .is_connected = hid_ble_is_connected,

    .kb_press = hid_ble_kb_press,
    .kb_release = hid_ble_kb_release,
    .mouse_press = hid_ble_mouse_press,
    .mouse_release = hid_ble_mouse_release,
    .mouse_scroll = hid_ble_mouse_scroll,
    .mouse_move = hid_ble_mouse_move,
    .consumer_press = hid_ble_consumer_press,
    .consumer_release = hid_ble_consumer_release,
    .release_all = hid_ble_release_all,
    .get_led_state = hid_ble_get_led_state,
};

const BadUsbHidApi* bad_usb_hid_get_interface(BadUsbHidInterface interface) {
    if(interface == BadUsbHidInterfaceUsb) {
        return &hid_api_usb;
    } else {
        return &hid_api_ble;
    }
}

void bad_usb_hid_ble_remove_pairing(void) {
    if(!ble_hid_remove_pairing()) {
        FURI_LOG_W(TAG, "BLE pairing removal failed");
    }
}
