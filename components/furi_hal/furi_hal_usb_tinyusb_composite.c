#include "furi_hal_usb_tinyusb_composite.h"

#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32S2

#include <furi.h>
#include <string.h>
#include <stdio.h>

#include "tinyusb.h"
#include "tusb_cdc_acm.h"
#include "class/hid/hid_device.h"
#include "class/msc/msc_device.h"

/* Low-level access to switch the internal USB FSLS PHY mux between the OTG
 * controller and the USB-Serial-JTAG controller (see composite_uninstall). */
#include "hal/usb_serial_jtag_ll.h"

#define TAG "FuriHalUsbComp"

/* Default identity if backend doesn't supply one. */
/* Masquerade as a real Flipper Zero so qFlipper (and other host tools that
 * filter strictly on VID/PID) detect the device. ESP32-S3 native USB-OTG lets
 * us choose any descriptor identity. */
#define COMP_VID_DEFAULT 0x0483 /* STMicroelectronics (real Flipper Zero) */
#define COMP_PID_DEFAULT 0x5740 /* CDC / Virtual ComPort */

/* HID Report IDs - shared with furi_hal_usb_hid_tinyusb.c via convention */
#define REPORT_ID_KEYBOARD 1
#define REPORT_ID_MOUSE    2
#define REPORT_ID_CONSUMER 3

/* Endpoint layout */
#define HID_EP_IN       0x81
#define HID_EP_BUF_SIZE 16
#define HID_POLL_MS     5

#define CDC_EP_NOTIF      0x82
#define CDC_EP_NOTIF_SIZE 8
#define CDC_EP_OUT        0x03
#define CDC_EP_IN         0x83
#define CDC_EP_BUF_SIZE   64

#define MSC_EP_OUT      0x04
#define MSC_EP_IN       0x84
#define MSC_EP_BUF_SIZE 64

#define ITF_NUM_HID       0
#define ITF_NUM_CDC_NOTIF 1
#define ITF_NUM_CDC_DATA  2
#define ITF_NUM_MSC       3
#define ITF_TOTAL         4

#define COMP_CONFIG_TOTAL_LEN \
    (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN)

/* Forward-declared from furi_hal_usb_hid_tinyusb.c */
extern const uint8_t* furi_hal_usb_hid_report_desc(size_t* out_len);

/* Bridge from tinyusb_cdcacm events to user-supplied CdcCallbacks.
 * Implemented in furi_hal_usb_cdc.c. */
extern void furi_hal_cdc_internal_register_tinyusb_callbacks(void);

static const uint8_t s_composite_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(
        1,
        ITF_TOTAL,
        0,
        COMP_CONFIG_TOTAL_LEN,
        TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP,
        100),
    /* HID */
    TUD_HID_DESCRIPTOR(
        ITF_NUM_HID,
        4 /* iInterface = "HID" */,
        false,
        0 /* report_desc_len patched at install-time */,
        HID_EP_IN,
        HID_EP_BUF_SIZE,
        HID_POLL_MS),
    /* CDC */
    TUD_CDC_DESCRIPTOR(
        ITF_NUM_CDC_NOTIF,
        5 /* iInterface = "Flipper Serial" */,
        CDC_EP_NOTIF,
        CDC_EP_NOTIF_SIZE,
        CDC_EP_OUT,
        CDC_EP_IN,
        CDC_EP_BUF_SIZE),
    /* MSC */
    TUD_MSC_DESCRIPTOR(
        ITF_NUM_MSC,
        6 /* iInterface = "Flipper Storage" */,
        MSC_EP_OUT,
        MSC_EP_IN,
        MSC_EP_BUF_SIZE),
};

static uint8_t s_config_desc_writable[sizeof(s_composite_config_desc)];

static tusb_desc_device_t s_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    /* IAD device class - required for HID + CDC composite */
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = COMP_VID_DEFAULT,
    .idProduct = COMP_PID_DEFAULT,
    .bcdDevice = 0x0100,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

static char s_manuf[64];
static char s_product[64];
static char s_serial[17];
static const char* s_string_descriptor[7];

static bool s_installed = false;

bool furi_hal_usb_composite_is_installed(void) {
    return s_installed;
}

bool furi_hal_usb_composite_install(
    uint16_t vid,
    uint16_t pid,
    const char* manuf,
    const char* product) {
    FURI_LOG_I(TAG, "composite_install entered: vid=%04x pid=%04x s_installed=%d", vid, pid, (int)s_installed);
    if(s_installed) {
        /* Identity changes after first install need a reboot to take effect. */
        if((vid && vid != s_device_descriptor.idVendor) ||
           (pid && pid != s_device_descriptor.idProduct)) {
            FURI_LOG_W(
                TAG,
                "VID/PID change ignored (composite already installed). Reboot to apply.");
        }
        return true;
    }

    /* Strings */
    const char* manuf_str = (manuf && manuf[0]) ? manuf : "Flipper Devices Inc.";
    const char* product_str = (product && product[0]) ? product : "Flipper Zero";
    snprintf(s_manuf, sizeof(s_manuf), "%s", manuf_str);
    snprintf(s_product, sizeof(s_product), "%s", product_str);
    snprintf(s_serial, sizeof(s_serial), "FZESP32");

    s_string_descriptor[0] = (const char[]){0x09, 0x04};
    s_string_descriptor[1] = s_manuf;
    s_string_descriptor[2] = s_product;
    s_string_descriptor[3] = s_serial;
    s_string_descriptor[4] = "Flipper HID";
    s_string_descriptor[5] = "Flipper Serial";
    s_string_descriptor[6] = "Flipper Storage";

    /* VID/PID */
    if(vid) s_device_descriptor.idVendor = vid;
    if(pid) s_device_descriptor.idProduct = pid;

    /* Patch the HID descriptor length (offset 16 inside TUD_HID_DESCRIPTOR within
     * the config blob: TUD_CONFIG (9 bytes) + TUD_HID up to wDescriptorLength.
     * TUD_HID_DESCRIPTOR layout is 9-byte interface + 9-byte hid + 7-byte EP.
     * wDescriptorLength is inside the 9-byte HID class descriptor at offset 7..8.
     * Absolute: 9 (config) + 9 (interface) + 7 (in HID class desc up to wDesc) = 25.
     */
    memcpy(s_config_desc_writable, s_composite_config_desc, sizeof(s_composite_config_desc));
    size_t hid_report_len = 0;
    const uint8_t* hid_report = furi_hal_usb_hid_report_desc(&hid_report_len);
    (void)hid_report; /* TinyUSB pulls this via tud_hid_descriptor_report_cb */
    s_config_desc_writable[9 + 9 + 7] = (uint8_t)(hid_report_len & 0xFF);
    s_config_desc_writable[9 + 9 + 8] = (uint8_t)((hid_report_len >> 8) & 0xFF);

    tinyusb_config_t tusb_cfg = {
        .device_descriptor = &s_device_descriptor,
        .string_descriptor = s_string_descriptor,
        .string_descriptor_count =
            sizeof(s_string_descriptor) / sizeof(s_string_descriptor[0]),
        .external_phy = false,
#if (TUD_OPT_HIGH_SPEED)
        .fs_configuration_descriptor = s_config_desc_writable,
        .hs_configuration_descriptor = s_config_desc_writable,
        .qualifier_descriptor = NULL,
#else
        .configuration_descriptor = s_config_desc_writable,
#endif
    };

    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "tinyusb_driver_install failed: %d", err);
        return false;
    }

    /* CDC-ACM driver init for ITF 0 (logical CDC index, mapping to TinyUSB
     * interfaces ITF_NUM_CDC_NOTIF/DATA). */
    tinyusb_config_cdcacm_t cdc_cfg = {
        .usb_dev = TINYUSB_USBDEV_0,
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = NULL,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = NULL,
        .callback_line_coding_changed = NULL,
    };
    err = tusb_cdc_acm_init(&cdc_cfg);
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "tusb_cdc_acm_init failed: %d", err);
        return false;
    }

    /* Wire CDC events into furi_hal_cdc state. */
    furi_hal_cdc_internal_register_tinyusb_callbacks();

    s_installed = true;
    FURI_LOG_I(
        TAG,
        "TinyUSB Composite installed: vid=%04x pid=%04x (HID + CDC)",
        s_device_descriptor.idVendor,
        s_device_descriptor.idProduct);
    return true;
}

bool furi_hal_usb_composite_uninstall(void) {
    if(!s_installed) return true;

    /* 1) Tear down the TinyUSB stack: stops the device task, runs tusb_teardown
     *    (a no-op in this esp_tinyusb build) and deletes the OTG PHY, which
     *    disables the USB WRAP and drops the D+ pull-up — the host sees the
     *    composite disconnect. */
    esp_err_t err = tinyusb_driver_uninstall();
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "tinyusb_driver_uninstall failed: %d", err);
        /* Fall through and try the PHY switch anyway — partial teardown still
         * left the OTG controller idle. */
    }

    /* 2) Re-route the shared internal FSLS PHY from the OTG controller back to
     *    the USB-Serial-JTAG controller and re-enable its pads. The USJ bus
     *    clock has been running since boot (it was the console/flash port);
     *    OTG install never touched it. Re-applying the USJ pull-up makes the
     *    host re-enumerate the JTAG/serial device, so esptool can flash again.
     *
     *    usb_serial_jtag_ll_phy_enable_external(false) sets:
     *      USB_SERIAL_JTAG.conf0.phy_sel = 0
     *      RTCCNTL.usb_conf.sw_hw_usb_phy_sel = 1   (software mux control)
     *      RTCCNTL.usb_conf.sw_usb_phy_sel   = 0   (internal PHY -> USJ) */
    usb_serial_jtag_ll_phy_enable_external(false);
    usb_serial_jtag_ll_phy_enable_pad(true);

    s_installed = false;
    FURI_LOG_I(TAG, "Composite uninstalled, USB-Serial-JTAG restored");
    return true;
}

#else /* !ESP32-S3 / S2 */

bool furi_hal_usb_composite_install(
    uint16_t vid,
    uint16_t pid,
    const char* manuf,
    const char* product) {
    (void)vid;
    (void)pid;
    (void)manuf;
    (void)product;
    return false;
}

bool furi_hal_usb_composite_is_installed(void) {
    return false;
}

bool furi_hal_usb_composite_uninstall(void) {
    return false;
}

#endif
