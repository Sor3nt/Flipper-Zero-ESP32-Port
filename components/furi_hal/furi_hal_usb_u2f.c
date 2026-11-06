#include "furi_hal_usb_hid_u2f.h"

#include "furi_hal_usb_hid_backend.h"
#include "furi_hal_usb_tinyusb_composite.h"

#include <furi.h>
#include <string.h>
#include <freertos/FreeRTOS.h>

#include "sdkconfig.h"
#include "tinyusb.h"
#include "class/hid/hid_device.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#error "U2F USB backend is supported only on ESP32-S3"
#endif

#define TAG "FuriHalU2F"

#define U2F_PACKET_SIZE 64
#define U2F_ENDPOINT_IN 0x81
#define U2F_ENDPOINT_OUT 0x01
#define U2F_ENDPOINT_INTERVAL_MS 5
#define U2F_RING_SIZE 16

#if CFG_TUD_HID_EP_BUFSIZE < U2F_PACKET_SIZE
#error "CFG_TUD_HID_EP_BUFSIZE must be at least 64 for U2F"
#endif

static const uint8_t s_report_descriptor[] = {
    0x06, 0xD0, 0xF1, /* Usage Page (FIDO Alliance) */
    0x09, 0x01,       /* Usage (U2F Authenticator Device) */
    0xA1, 0x01,       /* Collection (Application) */
    0x09, 0x20,       /* Usage (Input Report Data) */
    0x15, 0x00,       /* Logical Minimum (0) */
    0x26, 0xFF, 0x00, /* Logical Maximum (255) */
    0x75, 0x08,       /* Report Size (8) */
    0x95, 0x40,       /* Report Count (64) */
    0x81, 0x02,       /* Input (Data, Variable, Absolute) */
    0x09, 0x21,       /* Usage (Output Report Data) */
    0x15, 0x00,
    0x26, 0xFF, 0x00,
    0x75, 0x08,
    0x95, 0x40,
    0x91, 0x02,       /* Output (Data, Variable, Absolute) */
    0xC0,             /* End Collection */
};

static const uint8_t s_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, 41, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    9, TUSB_DESC_INTERFACE, 0, 0, 2, TUSB_CLASS_HID, 0, 0, 4,
    9, HID_DESC_TYPE_HID, 0x11, 0x01, 0, 1, HID_DESC_TYPE_REPORT,
    sizeof(s_report_descriptor), 0,
    7, TUSB_DESC_ENDPOINT, U2F_ENDPOINT_IN, TUSB_XFER_INTERRUPT,
    U2F_PACKET_SIZE, 0, U2F_ENDPOINT_INTERVAL_MS,
    7, TUSB_DESC_ENDPOINT, U2F_ENDPOINT_OUT, TUSB_XFER_INTERRUPT,
    U2F_PACKET_SIZE, 0, U2F_ENDPOINT_INTERVAL_MS,
};

static tusb_desc_device_t s_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0,
    .bDeviceSubClass = 0,
    .bDeviceProtocol = 0,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x0483,
    .idProduct = 0x5741,
    .bcdDevice = 0x0100,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

static char s_manufacturer[] = "Flipper Devices Inc.";
static char s_product[] = "Flipper Zero";
static char s_serial[] = "FZESP32";
static const char* s_string_descriptor[] = {
    (const char[]){0x09, 0x04},
    s_manufacturer,
    s_product,
    s_serial,
    "U2F",
};

typedef struct {
    uint8_t packet[U2F_PACKET_SIZE];
} U2fReport;

static U2fReport s_rx_ring[U2F_RING_SIZE];
static uint8_t s_rx_head;
static uint8_t s_rx_tail;
static uint8_t s_rx_count;
static uint32_t s_rx_dropped;
static bool s_active;
static bool s_mounted;
static bool s_stopping;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static FuriSemaphore* s_rx_semaphore;
static uint32_t s_callbacks_in_flight;
static HidU2fCallback s_callback;
static void* s_callback_context;
static uint32_t s_callback_generation;

static void furi_hal_usb_u2f_callback_finish(void) {
    portENTER_CRITICAL(&s_state_lock);
    furi_check(s_callbacks_in_flight > 0);
    s_callbacks_in_flight--;
    portEXIT_CRITICAL(&s_state_lock);
}

static void furi_hal_usb_u2f_callback_wait_idle(void) {
    while(true) {
        portENTER_CRITICAL(&s_state_lock);
        bool idle = s_callbacks_in_flight == 0;
        portEXIT_CRITICAL(&s_state_lock);
        if(idle) return;
        furi_delay_ms(1);
    }
}

static void furi_hal_usb_u2f_callback_invoke(
    HidU2fCallback callback,
    void* context,
    HidU2fEvent event) {
    if(!callback) return;
    callback(event, context);
    furi_hal_usb_u2f_callback_finish();
}

static bool furi_hal_usb_u2f_callback_reserve(
    HidU2fCallback* callback,
    void** context) {
    portENTER_CRITICAL(&s_state_lock);
    *callback = s_callback;
    *context = s_callback_context;
    if(*callback) s_callbacks_in_flight++;
    portEXIT_CRITICAL(&s_state_lock);
    return *callback != NULL;
}

const uint8_t* furi_hal_usb_u2f_report_descriptor(void) {
    return s_report_descriptor;
}

bool furi_hal_usb_u2f_backend_is_active(void) {
    return __atomic_load_n(&s_active, __ATOMIC_ACQUIRE);
}

bool furi_hal_usb_u2f_backend_is_mounted(void) {
    portENTER_CRITICAL(&s_state_lock);
    bool mounted = s_mounted;
    portEXIT_CRITICAL(&s_state_lock);
    return mounted;
}

void furi_hal_usb_u2f_backend_set_mounted(bool mounted) {
    HidU2fCallback callback;
    void* context;
    portENTER_CRITICAL(&s_state_lock);
    if(s_stopping) {
        portEXIT_CRITICAL(&s_state_lock);
        return;
    }
    if(s_mounted == mounted) {
        portEXIT_CRITICAL(&s_state_lock);
        return;
    }
    s_mounted = mounted;
    if(!mounted) {
        s_rx_head = 0;
        s_rx_tail = 0;
        s_rx_count = 0;
        s_rx_dropped = 0;
    }
    callback = s_callback;
    context = s_callback_context;
    if(callback) s_callbacks_in_flight++;
    portEXIT_CRITICAL(&s_state_lock);

    if(!mounted && s_rx_semaphore) {
        while(furi_semaphore_acquire(s_rx_semaphore, 0) == FuriStatusOk) {
        }
    }
    if(callback) {
        furi_hal_usb_u2f_callback_invoke(
            callback, context, mounted ? HidU2fConnected : HidU2fDisconnected);
    }
}

bool furi_hal_usb_u2f_backend_start(void) {
    if(furi_hal_usb_composite_is_installed()) {
        FURI_LOG_E(TAG, "Cannot start U2F while the composite USB device is installed");
        return false;
    }

    if(furi_hal_usb_u2f_backend_is_active()) return true;

    if(!s_rx_semaphore) s_rx_semaphore = furi_semaphore_alloc(U2F_RING_SIZE, 0);
    if(!s_rx_semaphore) {
        FURI_LOG_E(TAG, "Failed to allocate U2F report queue");
        return false;
    }

    if(!furi_hal_usb_hid_backend_release_stack()) {
        FURI_LOG_E(TAG, "Failed to release the previous HID USB stack");
        return false;
    }

    portENTER_CRITICAL(&s_state_lock);
    s_rx_head = 0;
    s_rx_tail = 0;
    s_rx_count = 0;
    s_rx_dropped = 0;
    s_mounted = false;
    s_stopping = false;
    __atomic_store_n(&s_active, true, __ATOMIC_RELEASE);
    portEXIT_CRITICAL(&s_state_lock);
    while(furi_semaphore_acquire(s_rx_semaphore, 0) == FuriStatusOk) {
    }

    tinyusb_config_t tusb_cfg = {
        .device_descriptor = &s_device_descriptor,
        .string_descriptor = s_string_descriptor,
        .string_descriptor_count = sizeof(s_string_descriptor) / sizeof(s_string_descriptor[0]),
        .external_phy = false,
#if (TUD_OPT_HIGH_SPEED)
        .fs_configuration_descriptor = s_configuration_descriptor,
        .hs_configuration_descriptor = s_configuration_descriptor,
        .qualifier_descriptor = NULL,
#else
        .configuration_descriptor = s_configuration_descriptor,
#endif
    };

    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if(err != ESP_OK) {
        portENTER_CRITICAL(&s_state_lock);
        __atomic_store_n(&s_active, false, __ATOMIC_RELEASE);
        s_stopping = true;
        portEXIT_CRITICAL(&s_state_lock);
        FURI_LOG_E(TAG, "tinyusb_driver_install failed: %d", err);
        furi_hal_usb_composite_restore_serial_jtag();
        return false;
    }

    FURI_LOG_I(TAG, "U2F HID installed (VID:PID %04x:%04x)", 0x0483, 0x5741);
    furi_hal_usb_backend_set_light_sleep_inhibited(true);
    if(!furi_hal_usb_backend_force_reenumeration()) {
        FURI_LOG_E(TAG, "U2F installed but host re-enumeration request failed");
    }
    return true;
}

bool furi_hal_usb_u2f_backend_stop(void) {
    if(!furi_hal_usb_u2f_backend_is_active()) return true;

    HidU2fCallback saved_callback;
    void* saved_context;
    uint32_t detached_generation;
    bool restore_callback = false;
    portENTER_CRITICAL(&s_state_lock);
    s_stopping = true;
    saved_callback = s_callback;
    saved_context = s_callback_context;
    s_callback = NULL;
    s_callback_context = NULL;
    detached_generation = ++s_callback_generation;
    portEXIT_CRITICAL(&s_state_lock);
    furi_hal_usb_u2f_callback_wait_idle();

    esp_err_t err = tinyusb_driver_uninstall();
    if(err != ESP_OK) {
        portENTER_CRITICAL(&s_state_lock);
        if(!s_callback && s_callback_generation == detached_generation) {
            s_callback = saved_callback;
            s_callback_context = saved_context;
            s_callback_generation++;
        }
        s_stopping = false;
        portEXIT_CRITICAL(&s_state_lock);
        FURI_LOG_E(TAG, "tinyusb_driver_uninstall failed: %d", err);
        return false;
    }

    portENTER_CRITICAL(&s_state_lock);
    s_mounted = false;
    s_rx_head = 0;
    s_rx_tail = 0;
    s_rx_count = 0;
    s_rx_dropped = 0;
    __atomic_store_n(&s_active, false, __ATOMIC_RELEASE);
    if(!s_callback && s_callback_generation == detached_generation) {
        s_callback = saved_callback;
        s_callback_context = saved_context;
        s_callback_generation++;
        restore_callback = saved_callback != NULL;
    }
    if(restore_callback) s_callbacks_in_flight++;
    portEXIT_CRITICAL(&s_state_lock);

    while(furi_semaphore_acquire(s_rx_semaphore, 0) == FuriStatusOk) {
    }
    if(restore_callback) {
        furi_hal_usb_u2f_callback_invoke(
            saved_callback, saved_context, HidU2fDisconnected);
    }
    furi_hal_usb_composite_restore_serial_jtag();
    furi_hal_usb_backend_set_light_sleep_inhibited(false);
    return true;
}

void furi_hal_hid_u2f_set_callback(HidU2fCallback callback, void* context) {
    bool mounted;
    bool callback_reserved = false;
    bool request_reserved = false;
    portENTER_CRITICAL(&s_state_lock);
    s_callback = callback;
    s_callback_context = context;
    s_callback_generation++;
    mounted = s_mounted;
    if(callback && mounted) {
        s_callbacks_in_flight++;
        callback_reserved = true;
        if(s_rx_count > 0) {
            s_callbacks_in_flight++;
            request_reserved = true;
        }
    }
    portEXIT_CRITICAL(&s_state_lock);

    if(callback_reserved) {
        furi_hal_usb_u2f_callback_invoke(callback, context, HidU2fConnected);
        if(request_reserved) {
            furi_hal_usb_u2f_callback_invoke(callback, context, HidU2fRequest);
        }
    } else if(!callback) {
        furi_hal_usb_u2f_callback_wait_idle();
    }
}

uint32_t furi_hal_hid_u2f_get_request(uint8_t* buffer) {
    if(!buffer || !s_rx_semaphore ||
       furi_semaphore_acquire(s_rx_semaphore, 0) != FuriStatusOk) {
        return 0;
    }

    uint32_t dropped;
    portENTER_CRITICAL(&s_state_lock);
    if(s_rx_count == 0) {
        portEXIT_CRITICAL(&s_state_lock);
        return 0;
    }
    memcpy(buffer, s_rx_ring[s_rx_tail].packet, U2F_PACKET_SIZE);
    s_rx_tail = (s_rx_tail + 1) % U2F_RING_SIZE;
    s_rx_count--;
    dropped = s_rx_dropped;
    s_rx_dropped = 0;
    portEXIT_CRITICAL(&s_state_lock);
    if(dropped) FURI_LOG_W(TAG, "Dropped %lu U2F reports because the RX queue was full", dropped);
    return U2F_PACKET_SIZE;
}

bool furi_hal_hid_u2f_send_response(const uint8_t* buffer, uint32_t length) {
    if(!buffer || length != U2F_PACKET_SIZE || !furi_hal_usb_u2f_backend_is_active()) {
        return false;
    }

    for(uint32_t timeout = 0; timeout < 500; timeout++) {
        if(!tud_mounted()) {
            FURI_LOG_W(TAG, "U2F response dropped: USB is not mounted");
            return false;
        }
        if(tud_hid_ready()) {
            if(tud_hid_report(0, buffer, length)) return true;
            FURI_LOG_W(TAG, "U2F response rejected by TinyUSB");
            return false;
        }
        furi_delay_ms(1);
    }
    FURI_LOG_W(TAG, "U2F interrupt IN endpoint timed out");
    return false;
}

void furi_hal_hid_u2f_receive_report(const uint8_t* buffer, uint16_t length) {
    if(!buffer || !s_rx_semaphore) return;
    if(length != U2F_PACKET_SIZE) {
        FURI_LOG_W(TAG, "Ignoring U2F report with unexpected length %u", (unsigned)length);
        return;
    }

    bool queued = false;
    portENTER_CRITICAL(&s_state_lock);
    if(__atomic_load_n(&s_active, __ATOMIC_ACQUIRE) && s_mounted && !s_stopping) {
        if(s_rx_count < U2F_RING_SIZE) {
            memcpy(s_rx_ring[s_rx_head].packet, buffer, U2F_PACKET_SIZE);
            s_rx_head = (s_rx_head + 1) % U2F_RING_SIZE;
            s_rx_count++;
            queued = true;
        } else {
            s_rx_dropped++;
        }
    }
    portEXIT_CRITICAL(&s_state_lock);

    if(!queued) return;
    furi_check(furi_semaphore_release(s_rx_semaphore) == FuriStatusOk);

    HidU2fCallback callback;
    void* context;
    if(furi_hal_usb_u2f_callback_reserve(&callback, &context)) {
        furi_hal_usb_u2f_callback_invoke(callback, context, HidU2fRequest);
    }
}

_Static_assert(sizeof(s_report_descriptor) < 256, "U2F HID report descriptor too large");
_Static_assert(sizeof(s_configuration_descriptor) == 41, "U2F USB descriptor size mismatch");
