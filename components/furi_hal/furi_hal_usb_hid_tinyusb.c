#include "furi_hal_usb.h"
#include "furi_hal_usb_hid.h"
#include "furi_hal_usb_hid_backend.h"
#include "furi_hal_usb_tinyusb_composite.h"

#include <furi.h>
#include <furi_hal_power.h>
#include <furi_hal_version.h>
#include <freertos/FreeRTOS.h>
#include <string.h>
#include <stdio.h>

#include "sdkconfig.h"
#include "tinyusb.h"
#include "class/hid/hid_device.h"
#if CONFIG_IDF_TARGET_ESP32S3
#include "furi_hal_usb_hid_u2f.h"
#endif

#define TAG "FuriHalUsbHid"

#define REPORT_ID_KEYBOARD 1
#define REPORT_ID_MOUSE    2
#define REPORT_ID_CONSUMER 3

#define HID_EP_IN       0x81
#define HID_EP_BUF_SIZE 16
#define HID_POLL_MS     5
#define HID_TX_TIMEOUT_MS 25
#define USB_REENUMERATE_DISCONNECT_MS 300

static const uint8_t hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(REPORT_ID_KEYBOARD)),
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(REPORT_ID_MOUSE)),
    TUD_HID_REPORT_DESC_CONSUMER(HID_REPORT_ID(REPORT_ID_CONSUMER)),
};

#define HID_CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)

static const uint8_t hid_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, HID_CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(
        0,
        4,
        false,
        sizeof(hid_report_descriptor),
        HID_EP_IN,
        HID_EP_BUF_SIZE,
        HID_POLL_MS),
};

static tusb_desc_device_t hid_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0,
    .bDeviceSubClass = 0,
    .bDeviceProtocol = 0,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = HID_VID_DEFAULT,
    .idProduct = HID_PID_DEFAULT,
    .bcdDevice = 0x0100,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

static char s_manuf[HID_MANUF_PRODUCT_NAME_LEN + 1];
static char s_product[HID_MANUF_PRODUCT_NAME_LEN + 1];
static char s_serial[17];
/* String descriptor 0 is the LANGID table (0x0409, English US). esp_tinyusb only
 * keeps the pointers and TinyUSB reads this one on every enumeration, so it must
 * have static storage: a block-scope compound literal lives on the stack and
 * dangles as soon as backend_start() returns. */
static const char s_langid_descriptor[] = {0x09, 0x04};
static const char* s_string_descriptor[5];

typedef struct {
    bool installed;
    bool mounted;
    uint8_t led_state;
    uint8_t modifiers;
    uint8_t keys[HID_KB_MAX_KEYS];
    uint8_t mouse_buttons;
    uint16_t consumer[HID_CONSUMER_MAX_KEYS];
} HidState;

static HidState s_state = {0};
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static HidStateCallback s_user_cb = NULL;
static void* s_user_ctx = NULL;
static uint16_t s_user_callbacks_in_flight;
static bool s_light_sleep_inhibited;

static void hid_state_lock(void) {
    portENTER_CRITICAL(&s_state_lock);
}

static void hid_state_unlock(void) {
    portEXIT_CRITICAL(&s_state_lock);
}

static void hid_publish_mount(bool mounted) {
    s_state.mounted = mounted;
    if(!mounted) {
        memset(s_state.keys, 0, sizeof(s_state.keys));
        memset(s_state.consumer, 0, sizeof(s_state.consumer));
        s_state.modifiers = 0;
        s_state.mouse_buttons = 0;
        s_state.led_state = 0;
    }
}

static void hid_callback_release(void) {
    hid_state_lock();
    furi_check(s_user_callbacks_in_flight > 0);
    s_user_callbacks_in_flight--;
    hid_state_unlock();
}

static void hid_callback_wait_idle(void) {
    while(true) {
        hid_state_lock();
        bool idle = s_user_callbacks_in_flight == 0;
        hid_state_unlock();
        if(idle) return;
        furi_delay_ms(1);
    }
}

static void hid_notify_mount(bool mounted) {
    hid_state_lock();
    HidStateCallback callback = s_user_cb;
    void* context = s_user_ctx;
    if(callback) {
        furi_check(s_user_callbacks_in_flight < UINT16_MAX);
        s_user_callbacks_in_flight++;
    }
    hid_state_unlock();
    if(callback) {
        callback(mounted, context);
        hid_callback_release();
    }
}

/* TinyUSB mount callbacks - invoked from TinyUSB task */
void tud_mount_cb(void) {
#if CONFIG_IDF_TARGET_ESP32S3
    if(furi_hal_usb_u2f_backend_is_active()) {
        furi_hal_usb_u2f_backend_set_mounted(true);
        return;
    }
#endif
    portENTER_CRITICAL(&s_state_lock);
    hid_publish_mount(true);
    portEXIT_CRITICAL(&s_state_lock);
    FURI_LOG_I(TAG, "TinyUSB device mounted");
    hid_notify_mount(true);
}

void tud_umount_cb(void) {
#if CONFIG_IDF_TARGET_ESP32S3
    if(furi_hal_usb_u2f_backend_is_active()) {
        furi_hal_usb_u2f_backend_set_mounted(false);
        return;
    }
#endif
    portENTER_CRITICAL(&s_state_lock);
    hid_publish_mount(false);
    portEXIT_CRITICAL(&s_state_lock);
    FURI_LOG_I(TAG, "TinyUSB device unmounted");
    hid_notify_mount(false);
}

void tud_suspend_cb(bool remote_wakeup_en) {
    (void)remote_wakeup_en;
#if CONFIG_IDF_TARGET_ESP32S3
    if(furi_hal_usb_u2f_backend_is_active()) {
        /* Cable removal can arrive as suspend before the controller reports
         * an unplug event. Publish it as disconnected so U2F does not keep a
         * stale connected state until the USB stack is restarted. A later
         * tud_resume_cb() republishes the mounted state on reconnection. */
        FURI_LOG_I(TAG, "U2F USB suspended; reporting disconnected");
        furi_hal_usb_u2f_backend_set_mounted(false);
        return;
    }
#endif
    portENTER_CRITICAL(&s_state_lock);
    hid_publish_mount(false);
    portEXIT_CRITICAL(&s_state_lock);
    FURI_LOG_I(TAG, "TinyUSB device suspended");
    hid_notify_mount(false);
}

void tud_resume_cb(void) {
    bool mounted = tud_mounted();
#if CONFIG_IDF_TARGET_ESP32S3
    if(furi_hal_usb_u2f_backend_is_active()) {
        furi_hal_usb_u2f_backend_set_mounted(mounted);
        return;
    }
#endif
    portENTER_CRITICAL(&s_state_lock);
    hid_publish_mount(mounted);
    portEXIT_CRITICAL(&s_state_lock);
    FURI_LOG_I(TAG, "TinyUSB device resumed mounted=%u", (unsigned)mounted);
    hid_notify_mount(mounted);
}

/* TinyUSB HID callbacks */
uint8_t const* tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
#if CONFIG_IDF_TARGET_ESP32S3
    if(furi_hal_usb_u2f_backend_is_active()) {
        return furi_hal_usb_u2f_report_descriptor();
    }
#endif
    return hid_report_descriptor;
}

/* Exposed for the Composite Device (HID + CDC + MSC) descriptor in
 * furi_hal_usb_tinyusb_composite.c which needs to patch wDescriptorLength. */
const uint8_t* furi_hal_usb_hid_report_desc(size_t* out_len) {
    if(out_len) *out_len = sizeof(hid_report_descriptor);
    return hid_report_descriptor;
}

uint16_t tud_hid_get_report_cb(
    uint8_t instance,
    uint8_t report_id,
    hid_report_type_t report_type,
    uint8_t* buffer,
    uint16_t reqlen) {
    (void)instance;
#if CONFIG_IDF_TARGET_ESP32S3
    if(furi_hal_usb_u2f_backend_is_active()) return 0;
#endif
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(
    uint8_t instance,
    uint8_t report_id,
    hid_report_type_t report_type,
    uint8_t const* buffer,
    uint16_t bufsize) {
    (void)instance;
#if CONFIG_IDF_TARGET_ESP32S3
    if(furi_hal_usb_u2f_backend_is_active()) {
        if(report_type == HID_REPORT_TYPE_OUTPUT && report_id == 0) {
            furi_hal_hid_u2f_receive_report(buffer, bufsize);
        }
        return;
    }
#endif
    if(report_type == HID_REPORT_TYPE_OUTPUT && report_id == REPORT_ID_KEYBOARD && buffer &&
       bufsize >= 1) {
        hid_state_lock();
        s_state.led_state = buffer[0];
        hid_state_unlock();
    }
}

void furi_hal_usb_backend_set_light_sleep_inhibited(bool inhibited) {
    if(s_light_sleep_inhibited == inhibited) return;
    if(inhibited) {
        furi_hal_power_light_sleep_inhibit_enter();
    } else {
        furi_hal_power_light_sleep_inhibit_exit();
    }
    s_light_sleep_inhibited = inhibited;
}

bool furi_hal_usb_backend_force_reenumeration(void) {
    if(!tud_inited()) {
        FURI_LOG_E(TAG, "Cannot re-enumerate USB before TinyUSB is initialized");
        return false;
    }

    FURI_LOG_I(TAG, "Requesting USB detach for host re-enumeration");
    if(!tud_disconnect()) {
        FURI_LOG_E(TAG, "TinyUSB disconnect request failed");
        return false;
    }
    furi_delay_ms(USB_REENUMERATE_DISCONNECT_MS);
    if(!tud_connect()) {
        FURI_LOG_E(TAG, "TinyUSB reconnect request failed");
        return false;
    }
    FURI_LOG_I(TAG, "TinyUSB reconnect requested");
    return true;
}

/* Backend start/stop - called from furi_hal_usb.c */
bool furi_hal_usb_hid_backend_start(const FuriHalUsbHidConfig* cfg) {
#if CONFIG_IDF_TARGET_ESP32S3
    if(furi_hal_usb_u2f_backend_is_active()) {
        FURI_LOG_E(TAG, "Cannot start USB HID while U2F is active");
        return false;
    }
#endif
    if(furi_hal_usb_composite_is_installed()) {
        FURI_LOG_E(TAG, "Cannot start USB HID while the composite USB device is active");
        return false;
    }

    if(s_state.installed) {
        /* Already installed. This happens when re-entering BadUsb after a config-
         * menu visit: leaving the work scene calls set_config(NULL) -> backend_stop,
         * which sets mounted=false, and returning calls set_config(usb_hid) ->
         * backend_start. Re-enumerate on every activation so the host cannot keep
         * using the previous USB session after a mode switch or suspend. */
        furi_hal_usb_backend_set_light_sleep_inhibited(true);
        hid_state_lock();
        bool was_mounted = s_state.mounted;
        if(was_mounted) hid_publish_mount(false);
        hid_state_unlock();
        if(was_mounted) hid_notify_mount(false);
        if(!furi_hal_usb_backend_force_reenumeration()) {
            FURI_LOG_E(TAG, "USB HID re-enumeration request failed on restart");
        }
        return true;
    }

    const char* manuf = (cfg && cfg->manuf[0]) ? cfg->manuf : "Flipper Devices Inc.";
    const char* product =
        (cfg && cfg->product[0]) ? cfg->product : furi_hal_version_get_name_ptr();
    if(!product || !product[0]) product = "Flipper Zero";
    uint16_t vid = (cfg && cfg->vid) ? (uint16_t)cfg->vid : HID_VID_DEFAULT;
    uint16_t pid = (cfg && cfg->pid) ? (uint16_t)cfg->pid : HID_PID_DEFAULT;

    snprintf(s_manuf, sizeof(s_manuf), "%s", manuf);
    snprintf(s_product, sizeof(s_product), "%s", product);
    snprintf(s_serial, sizeof(s_serial), "FZESP32");

    hid_device_descriptor.idVendor = vid;
    hid_device_descriptor.idProduct = pid;

    s_string_descriptor[0] = s_langid_descriptor;
    s_string_descriptor[1] = s_manuf;
    s_string_descriptor[2] = s_product;
    s_string_descriptor[3] = s_serial;
    s_string_descriptor[4] = "HID";

    tinyusb_config_t tusb_cfg = {
        .device_descriptor = &hid_device_descriptor,
        .string_descriptor = s_string_descriptor,
        .string_descriptor_count =
            sizeof(s_string_descriptor) / sizeof(s_string_descriptor[0]),
        .external_phy = false,
#if (TUD_OPT_HIGH_SPEED)
        .fs_configuration_descriptor = hid_configuration_descriptor,
        .hs_configuration_descriptor = hid_configuration_descriptor,
        .qualifier_descriptor = NULL,
#else
        .configuration_descriptor = hid_configuration_descriptor,
#endif
    };

    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "tinyusb_driver_install failed: %d", err);
        return false;
    }

    s_state.installed = true;
    furi_hal_usb_backend_set_light_sleep_inhibited(true);
    FURI_LOG_I(TAG, "TinyUSB HID installed vid=%04x pid=%04x", vid, pid);
    if(!furi_hal_usb_backend_force_reenumeration()) {
        FURI_LOG_E(TAG, "USB HID installed but host re-enumeration request failed");
    }
    return true;
}

void furi_hal_usb_hid_backend_stop(void) {
    /* esp_tinyusb in ESP-IDF v5 does not expose a reliable uninstall.
     * We reset state and leave the stack running; re-entry to usb_hid
     * short-circuits via the installed flag. */
    hid_state_lock();
    bool was_mounted = s_state.mounted;
    if(was_mounted) hid_publish_mount(false);
    hid_state_unlock();
    if(was_mounted) hid_notify_mount(false);
    /* The TinyUSB stack stays installed, so keep light sleep inhibited until
     * release_stack() tears the controller down. */
}

bool furi_hal_usb_hid_backend_release_stack(void) {
    if(!s_state.installed) return true;
    if(furi_hal_usb_composite_is_installed()) {
        FURI_LOG_E(TAG, "Refusing to release TinyUSB while composite USB is active");
        return false;
    }

    FURI_LOG_I(TAG, "TinyUSB HID stack release begin");
    esp_err_t err = tinyusb_driver_uninstall();
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "tinyusb_driver_uninstall failed: %d", err);
        return false;
    }

    hid_state_lock();
    s_state.installed = false;
    bool was_mounted = s_state.mounted;
    if(was_mounted) hid_publish_mount(false);
    hid_state_unlock();
    if(was_mounted) hid_notify_mount(false);
    furi_hal_usb_backend_set_light_sleep_inhibited(false);
    furi_hal_usb_composite_restore_serial_jtag();
    FURI_LOG_I(TAG, "TinyUSB HID stack released; USB-Serial-JTAG restored");
    return true;
}

/* Public HID API */
bool furi_hal_hid_is_connected(void) {
    hid_state_lock();
    bool mounted = s_state.mounted;
    hid_state_unlock();
    return mounted;
}

uint8_t furi_hal_hid_get_led_state(void) {
    hid_state_lock();
    uint8_t led_state = s_state.led_state;
    hid_state_unlock();
    return led_state;
}

void furi_hal_hid_set_state_callback(HidStateCallback cb, void* ctx) {
    if(!cb) {
        hid_state_lock();
        s_user_cb = NULL;
        s_user_ctx = NULL;
        hid_state_unlock();
        hid_callback_wait_idle();
        return;
    }

    hid_state_lock();
    s_user_cb = NULL;
    s_user_ctx = NULL;
    hid_state_unlock();
    hid_callback_wait_idle();

    hid_state_lock();
    s_user_cb = cb;
    s_user_ctx = ctx;
    bool mounted = s_state.mounted;
    furi_check(s_user_callbacks_in_flight < UINT16_MAX);
    s_user_callbacks_in_flight++;
    hid_state_unlock();
    cb(mounted, ctx);
    hid_callback_release();
}

/* Wait until the HID IN endpoint can accept a new report.
 *
 * tud_hid_*_report() only queues into the endpoint FIFO; the report is "in
 * flight" (tud_hid_ready() == false) until the host polls it (every HID_POLL_MS).
 * BadUsb fires kb_press() immediately followed by kb_release() with no delay, so
 * without this wait the release report is dropped while the press is still in
 * flight -> the host never sees key-up -> auto-repeat / garbled output. This is
 * especially visible on the composite device (HID shares the bus with CDC+MSC).
 *
 * Must be called with the state lock held; the lock is released transiently
 * while delaying so TinyUSB mount/umount callbacks can still run. Returns true
 * if the endpoint is ready to accept a report. */
static bool hid_wait_tx_ready_locked(void) {
    uint32_t timeout_ms = HID_TX_TIMEOUT_MS;
    while(s_state.mounted && !tud_hid_ready()) {
        if(timeout_ms-- == 0) break;
        hid_state_unlock();
        furi_delay_ms(1);
        hid_state_lock();
    }
    return s_state.mounted && tud_hid_ready();
}

static bool send_keyboard_report_locked(void) {
    if(!hid_wait_tx_ready_locked()) return false;
    return tud_hid_keyboard_report(REPORT_ID_KEYBOARD, s_state.modifiers, s_state.keys);
}

bool furi_hal_hid_kb_press(uint16_t button) {
    uint8_t keycode = button & 0xFF;
    uint8_t mods = (button >> 8) & 0xFF;

    hid_state_lock();
    s_state.modifiers |= mods;
    if(keycode) {
        bool present = false;
        for(int i = 0; i < HID_KB_MAX_KEYS; i++) {
            if(s_state.keys[i] == keycode) {
                present = true;
                break;
            }
        }
        if(!present) {
            for(int i = 0; i < HID_KB_MAX_KEYS; i++) {
                if(s_state.keys[i] == 0) {
                    s_state.keys[i] = keycode;
                    break;
                }
            }
        }
    }
    bool result = send_keyboard_report_locked();
    hid_state_unlock();
    return result;
}

bool furi_hal_hid_kb_release(uint16_t button) {
    uint8_t keycode = button & 0xFF;
    uint8_t mods = (button >> 8) & 0xFF;

    hid_state_lock();
    s_state.modifiers &= ~mods;
    if(keycode) {
        uint8_t compact[HID_KB_MAX_KEYS] = {0};
        int idx = 0;
        for(int i = 0; i < HID_KB_MAX_KEYS; i++) {
            if(s_state.keys[i] && s_state.keys[i] != keycode) {
                compact[idx++] = s_state.keys[i];
            }
        }
        memcpy(s_state.keys, compact, sizeof(s_state.keys));
    }
    bool result = send_keyboard_report_locked();
    hid_state_unlock();
    return result;
}

bool furi_hal_hid_kb_release_all(void) {
    hid_state_lock();
    s_state.modifiers = 0;
    memset(s_state.keys, 0, sizeof(s_state.keys));
    bool result = send_keyboard_report_locked();
    hid_state_unlock();
    return result;
}

static bool send_mouse_report_locked(int8_t dx, int8_t dy, int8_t scroll) {
    if(!hid_wait_tx_ready_locked()) return false;
    return tud_hid_mouse_report(
        REPORT_ID_MOUSE, s_state.mouse_buttons, dx, dy, scroll, 0);
}

bool furi_hal_hid_mouse_move(int8_t dx, int8_t dy) {
    hid_state_lock();
    bool result = send_mouse_report_locked(dx, dy, 0);
    hid_state_unlock();
    return result;
}

bool furi_hal_hid_mouse_press(uint8_t button) {
    hid_state_lock();
    s_state.mouse_buttons |= button;
    bool result = send_mouse_report_locked(0, 0, 0);
    hid_state_unlock();
    return result;
}

bool furi_hal_hid_mouse_release(uint8_t button) {
    hid_state_lock();
    s_state.mouse_buttons &= ~button;
    bool result = send_mouse_report_locked(0, 0, 0);
    hid_state_unlock();
    return result;
}

bool furi_hal_hid_mouse_scroll(int8_t delta) {
    hid_state_lock();
    bool result = send_mouse_report_locked(0, 0, delta);
    hid_state_unlock();
    return result;
}

static bool send_consumer_report_locked(void) {
    if(!hid_wait_tx_ready_locked()) return false;
    /* Standard TUD_HID_REPORT_DESC_CONSUMER emits one 16-bit usage.
     * We pick the most-recently pressed key that is still active. */
    uint16_t usage = 0;
    for(int i = HID_CONSUMER_MAX_KEYS - 1; i >= 0; i--) {
        if(s_state.consumer[i]) {
            usage = s_state.consumer[i];
            break;
        }
    }
    return tud_hid_report(REPORT_ID_CONSUMER, &usage, sizeof(usage));
}

bool furi_hal_hid_consumer_key_press(uint16_t button) {
    hid_state_lock();
    bool already = false;
    for(int i = 0; i < HID_CONSUMER_MAX_KEYS; i++) {
        if(s_state.consumer[i] == button) {
            already = true;
            break;
        }
    }
    if(!already) {
        for(int i = 0; i < HID_CONSUMER_MAX_KEYS; i++) {
            if(s_state.consumer[i] == 0) {
                s_state.consumer[i] = button;
                break;
            }
        }
    }
    bool result = send_consumer_report_locked();
    hid_state_unlock();
    return result;
}

bool furi_hal_hid_consumer_key_release(uint16_t button) {
    hid_state_lock();
    for(int i = 0; i < HID_CONSUMER_MAX_KEYS; i++) {
        if(s_state.consumer[i] == button) s_state.consumer[i] = 0;
    }
    bool result = send_consumer_report_locked();
    hid_state_unlock();
    return result;
}

bool furi_hal_hid_consumer_key_release_all(void) {
    hid_state_lock();
    memset(s_state.consumer, 0, sizeof(s_state.consumer));
    bool result = send_consumer_report_locked();
    hid_state_unlock();
    return result;
}
