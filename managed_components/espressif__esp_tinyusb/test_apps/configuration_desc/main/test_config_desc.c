/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "soc/soc_caps.h"

#if SOC_USB_OTG_SUPPORTED

#include <stdio.h>
#include <string.h>
#include "esp_system.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "unity.h"
#include "tinyusb.h"

static const char *TAG = "config_test";

#define TEARDOWN_DEVICE_ATTACH_TIMEOUT_MS   1000
#define TEARDOWN_DEVICE_DETACH_DELAY_MS     1000

// ========================= TinyUSB descriptors ===============================

// Here we need to create dual CDC device, to match the CONFIG_TINYUSB_CDC_COUNT from sdkconfig.defaults
static const uint16_t cdc_desc_config_len = TUD_CONFIG_DESC_LEN + CFG_TUD_CDC * TUD_CDC_DESC_LEN;
static const uint8_t test_fs_configuration_descriptor[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, CFG_TUD_CDC * 2, 0, cdc_desc_config_len, TUSB_DESC_CONFIG_ATT_SELF_POWERED | TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_CDC_DESCRIPTOR(0, 4, 0x81, 8, 0x02, 0x82, 64),
#if CFG_TUD_CDC > 1
    TUD_CDC_DESCRIPTOR(2, 4, 0x83, 8, 0x04, 0x84, 64),
#endif
};

#if (TUD_OPT_HIGH_SPEED)
static const uint8_t test_hs_configuration_descriptor[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, 4, 0, cdc_desc_config_len, TUSB_DESC_CONFIG_ATT_SELF_POWERED | TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_CDC_DESCRIPTOR(0, 4, 0x81, 8, 0x02, 0x82, 512),
    TUD_CDC_DESCRIPTOR(2, 4, 0x83, 8, 0x04, 0x84, 512),
};

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

// ========================== Private logic ====================================
SemaphoreHandle_t wait_mount = NULL;

/**
 * @brief Creates test additional resources.
 *
 * Is called before start test to create/init internal resources.
 */
static bool __test_init(void)
{
    wait_mount = xSemaphoreCreateBinary();
    return (wait_mount != NULL);
}

/**
 * @brief Indicates device connection event.
 *
 * Is called in tud_mount callback.
 */
static void __test_conn(void)
{
    if (wait_mount) {
        xSemaphoreGive(wait_mount);
    }
}

/**
 * @brief Awaits device connection event.
 *
 * Is used for waiting the event in test logic.
 * Timeout could be configured via TEARDOWN_DEVICE_ATTACH_TIMEOUT_MS define.
 */
static esp_err_t __test_wait_conn(void)
{
    if (!wait_mount) {
        return ESP_ERR_INVALID_STATE;
    }

    return ( xSemaphoreTake(wait_mount, pdMS_TO_TICKS(TEARDOWN_DEVICE_ATTACH_TIMEOUT_MS))
             ? ESP_OK
             : ESP_ERR_TIMEOUT );
}

/**
 * @brief Releases test resources.
 *
 * Is called in the end of the test to release internal resources.
 */
static void __test_release(void)
{
    if (wait_mount) {
        vSemaphoreDelete(wait_mount);
    }
}

/**
 * @brief One round of setup configuration for TinyUSB.
 *
 * Steps:
 * 1. Init internal resources
 * 2. Installs TinyUSB with provided configuration
 * 3. Waits for the connection event from the Host
 * 4. Gives some extra time for Host to configure the device (configures via TEARDOWN_DEVICE_DETACH_DELAY_MS)
 * 5. Uninstall TinyUSB
 * 6. Release internal resources
 *
 * Is called in the end of the test to release internal resources.
 */
static void __test_tinyusb_set_config(const tinyusb_config_t *tusb_cfg)
{
    TEST_ASSERT_EQUAL(true, __test_init());
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(tusb_cfg));
    TEST_ASSERT_EQUAL(ESP_OK, __test_wait_conn());
    vTaskDelay(pdMS_TO_TICKS(TEARDOWN_DEVICE_DETACH_DELAY_MS));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());
    __test_release();
}

// ========================== Callbacks ========================================
/**
 * @brief TinyUSB callback for device mount.
 *
 * @note
 * For Linux-based Hosts: Reflects the SetConfiguration() request from the Host Driver.
 * For Win-based Hosts: SetConfiguration() request is present only with available Class in device descriptor.
 */
void tud_mount_cb(void)
{
    ESP_LOGD(TAG, "%s", __FUNCTION__);
    __test_conn();
}

// ============================= Tests =========================================
/**
 * @brief TinyUSB Configuration test case.
 *
 * Verifies:
 * Configuration without specifying any parameters.
 * Configuration & descriptors are provided by esp_tinyusb wrapper.
 */
TEST_CASE("descriptors_config_all_default", "[esp_tinyusb][usb_device][config]")
{
    const tinyusb_config_t tusb_cfg = {
        .external_phy = false,
        .device_descriptor = NULL,
        .configuration_descriptor = NULL,
#if (CONFIG_TINYUSB_RHPORT_HS)
        .hs_configuration_descriptor = NULL,
#endif // CONFIG_TINYUSB_RHPORT_HS
    };
    // Install TinyUSB driver
    __test_tinyusb_set_config(&tusb_cfg);
}

/**
 * @brief TinyUSB Configuration test case.
 *
 * Verifies:
 * Configuration with specifying only device descriptor.
 * For High-speed qualifier descriptor as well.
 */
TEST_CASE("descriptors_config_device", "[esp_tinyusb][usb_device][config]")
{
    const tinyusb_config_t tusb_cfg = {
        .external_phy = false,
        .device_descriptor = &test_device_descriptor,
        .configuration_descriptor = NULL,
#if (CONFIG_TINYUSB_RHPORT_HS)
        .hs_configuration_descriptor = NULL,
        .qualifier_descriptor = &device_qualifier,
#endif // CONFIG_TINYUSB_RHPORT_HS
    };
    __test_tinyusb_set_config(&tusb_cfg);
}

/**
 * @brief TinyUSB Configuration test case.
 *
 * Verifies:
 * Configuration with specifying device & configuration descriptors.
 *
 * For High-speed:
 * - HS configuration descriptor is not provided by user (legacy compatibility) and default HS config descriptor is using when possible.
 * - Qualifier descriptor.
 */
TEST_CASE("descriptors_config_device_and_config", "[esp_tinyusb][usb_device][config]")
{
    const tinyusb_config_t tusb_cfg = {
        .external_phy = false,
        .device_descriptor = &test_device_descriptor,
        .configuration_descriptor = test_fs_configuration_descriptor,
#if (CONFIG_TINYUSB_RHPORT_HS)
        .hs_configuration_descriptor = NULL,
        .qualifier_descriptor = &device_qualifier,
#endif // CONFIG_TINYUSB_RHPORT_HS
    };
    __test_tinyusb_set_config(&tusb_cfg);
}

#if (CONFIG_IDF_TARGET_ESP32P4)
/**
 * @brief TinyUSB High-speed Configuration test case.
 *
 * Verifies:
 * Configuration with specifying device & HS configuration descriptor only.
 * FS configuration descriptor is not provided by user (legacy compatibility) and default configuration descriptor for FS is using when possible.
 */
TEST_CASE("descriptors_config_device_and_hs_config_only", "[esp_tinyusb][usb_device][config]")
{
    const tinyusb_config_t tusb_cfg = {
        .external_phy = false,
        .device_descriptor = &test_device_descriptor,
        .configuration_descriptor = NULL,
        .hs_configuration_descriptor = test_hs_configuration_descriptor,
        .qualifier_descriptor = &device_qualifier,
    };
    __test_tinyusb_set_config(&tusb_cfg);
}

/**
 * @brief TinyUSB High-speed Configuration test case.
 *
 * Verifies:
 * Configuration with specifying all descriptors by user.
 */
TEST_CASE("descriptors_config_all_configured", "[esp_tinyusb][usb_device][config]")
{
    const tinyusb_config_t tusb_cfg = {
        .external_phy = false,
        .device_descriptor = &test_device_descriptor,
        .fs_configuration_descriptor = test_fs_configuration_descriptor,
        .hs_configuration_descriptor = test_hs_configuration_descriptor,
        .qualifier_descriptor = &device_qualifier,
    };
    __test_tinyusb_set_config(&tusb_cfg);
}
#endif // CONFIG_IDF_TARGET_ESP32P4

#endif // SOC_USB_OTG_SUPPORTED
