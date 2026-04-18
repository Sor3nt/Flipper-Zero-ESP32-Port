/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "soc/soc_caps.h"
#if SOC_USB_OTG_SUPPORTED

#include <stdio.h>
#include <string.h>
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"

#include "unity.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"

static const char *TAG = "vendor_test";

#define TUSB_CFG_DESC_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + CFG_TUD_VENDOR * TUD_VENDOR_DESC_LEN)

static const uint8_t test_fs_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, CFG_TUD_VENDOR, 0, TUSB_CFG_DESC_TOTAL_LEN, 0x00, 100),
    TUD_VENDOR_DESCRIPTOR(0, 4, 0x02, 0x82, 64),
#if CFG_TUD_VENDOR > 1
    TUD_VENDOR_DESCRIPTOR(1, 4, 0x04, 0x84, 64),
#endif // CFG_TUD_VENDOR > 1
};

#if (TUD_OPT_HIGH_SPEED)

static const uint8_t test_hs_configuration_descriptor[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, CFG_TUD_VENDOR, 0, TUSB_CFG_DESC_TOTAL_LEN, 0x00, 100),
    TUD_VENDOR_DESCRIPTOR(0, 4, 0x02, 0x82, 512),
#if CFG_TUD_VENDOR > 1
    TUD_VENDOR_DESCRIPTOR(1, 4, 0x04, 0x84, 512),
#endif // CFG_TUD_VENDOR > 1
};

static const tusb_desc_device_qualifier_t device_qualifier = {
    .bLength = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_VENDOR_SPECIFIC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 0x01,
    .bReserved = 0
};
#endif // TUD_OPT_HIGH_SPEED

static const tusb_desc_device_t test_device_descriptor = {
    .bLength = sizeof(test_device_descriptor),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_VENDOR_SPECIFIC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A, // This is Espressif VID. This needs to be changed according to Users / Customers
    .idProduct = 0x4040,
    .bcdDevice = 0x100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01
};

char buffer_in[64];
#if (TUSB_VERSION_MINOR >= 17)
void tud_vendor_rx_cb(uint8_t itf, uint8_t const *buffer, uint16_t bufsize)
#else
void tud_vendor_rx_cb(uint8_t itf)
#endif // TUSB_VERSION_MINOR
{
    ESP_LOGI(TAG, "tud_vendor_rx_cb(itf=%d)", itf);
    int available = tud_vendor_n_available(itf);
    int read = tud_vendor_n_read(itf, buffer_in, available);
    ESP_LOGI(TAG, "actual read: %d. buffer message: %s", read, buffer_in);
}

// Invoked when a control transfer occurred on an interface of this class
// Driver response accordingly to the request and the transfer stage (setup/data/ack)
// return false to stall control endpoint (e.g unsupported request)
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request)
{
    // nothing to with DATA & ACK stage
    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }
    // stall unknown request
    return false;
}

/**
 * @brief TinyUSB Vendor specific testcase
 */
TEST_CASE("tinyusb_vendor", "[esp_tinyusb][vendor]")
{
    // Install TinyUSB driver
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    // Set descriptors
    tusb_cfg.descriptor.device = &test_device_descriptor;
    tusb_cfg.descriptor.full_speed_config = test_fs_configuration_descriptor;
#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.descriptor.qualifier = &device_qualifier;
    tusb_cfg.descriptor.high_speed_config = test_hs_configuration_descriptor;
#endif // TUD_OPT_HIGH_SPEED
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));
}

#endif
