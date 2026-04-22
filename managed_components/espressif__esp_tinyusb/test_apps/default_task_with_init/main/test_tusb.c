/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "soc/soc_caps.h"
#if SOC_USB_OTG_SUPPORTED

//
#include <stdio.h>
#include <string.h>
//
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
//
#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"
//
#include "unity.h"
#include "tinyusb.h"

static const char *TAG = "default_task_with_init";

SemaphoreHandle_t wait_mount = NULL;

#define TEARDOWN_DEVICE_DELAY_MS       1000

#define TUSB_DESC_TOTAL_LEN                 (TUD_CONFIG_DESC_LEN)
static uint8_t const test_configuration_descriptor[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, 0, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_SELF_POWERED | TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
};

static const tusb_desc_device_t test_device_descriptor = {
    .bLength = sizeof(test_device_descriptor),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A, // This is Espressif VID. This needs to be changed according to Users / Customers
    .idProduct = 0x4002,
    .bcdDevice = 0x100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01
};

#if (TUD_OPT_HIGH_SPEED)
static const tusb_desc_device_qualifier_t device_qualifier = {
    .bLength = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 0x01,
    .bReserved = 0
};
#endif // TUD_OPT_HIGH_SPEED

// Invoked when device is mounted
void tud_mount_cb(void)
{
    xSemaphoreGive(wait_mount);
}

/**
 * @brief TinyUSB Task specific testcase
 *
 * Scenario:
 *     1. Install TinyUSB driver
 *     2. Wait tud_mount_cb() until TUSB_DEVICE_DELAY_MS
 *     3. Wait TUSB_DEVICE_DELAY_MS
 *     4. Teardown TinyUSB
 *     5. Release resources
 */
TEST_CASE("tinyusb_default_task_with_init", "[esp_tinyusb][tusb_task]")
{
    wait_mount = xSemaphoreCreateBinary();
    TEST_ASSERT_NOT_EQUAL(NULL, wait_mount);

    // TinyUSB driver configuration
    const tinyusb_config_t tusb_cfg = {
        .device_descriptor = &test_device_descriptor,
        .string_descriptor = NULL,
        .string_descriptor_count = 0,
        .external_phy = false,
#if (TUD_OPT_HIGH_SPEED)
        .fs_configuration_descriptor = test_configuration_descriptor,
        .hs_configuration_descriptor = test_configuration_descriptor,
        .qualifier_descriptor = &device_qualifier,
#else
        .configuration_descriptor = test_configuration_descriptor,
#endif // TUD_OPT_HIGH_SPEED
    };

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));
    // Wait for the usb event
    ESP_LOGD(TAG, "wait mount...");
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(wait_mount, pdMS_TO_TICKS(TEARDOWN_DEVICE_DELAY_MS)));
    ESP_LOGD(TAG, "mounted");

    // Teardown
    vTaskDelay(pdMS_TO_TICKS(TEARDOWN_DEVICE_DELAY_MS));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());
    // Remove primitives
    vSemaphoreDelete(wait_mount);
}

#endif
