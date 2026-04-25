/*
 * SPDX-FileCopyrightText: 2023-2024 Espressif Systems (Shanghai) CO LTD
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
#include "tusb_cdc_acm.h"
#include "vfs_tinyusb.h"

#define VFS_PATH "/dev/usb-cdc1"

static const tusb_desc_device_t cdc_device_descriptor = {
    .bLength = sizeof(cdc_device_descriptor),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_ESPRESSIF_VID,
    .idProduct = 0x4002,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01
};

static const uint16_t cdc_desc_config_len = TUD_CONFIG_DESC_LEN + CFG_TUD_CDC * TUD_CDC_DESC_LEN;
static const uint8_t cdc_desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 4, 0, cdc_desc_config_len, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_CDC_DESCRIPTOR(0, 4, 0x81, 8, 0x02, 0x82, (TUD_OPT_HIGH_SPEED ? 512 : 64)),
    TUD_CDC_DESCRIPTOR(2, 4, 0x83, 8, 0x04, 0x84, (TUD_OPT_HIGH_SPEED ? 512 : 64)),
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

static void tinyusb_cdc_rx_callback(int itf, cdcacm_event_t *event)
{
}

/**
 * @brief TinyUSB CDC testcase
 *
 * This is not a 'standard' testcase, as it never exits. The testcase runs in a loop where it echoes back all the data received.
 *
 * - Init TinyUSB with standard CDC device and configuration descriptors
 * - Init 2 CDC-ACM interfaces
 * - Map CDC1 to Virtual File System
 * - In a loop: Read data from CDC0 and CDC1. Echo received data back
 *
 * Note: CDC0 appends 'novfs' to echoed data, so the host (test runner) can easily determine which port is which.
 */
TEST_CASE("tinyusb_cdc", "[esp_tinyusb][cdc]")
{
    // Install TinyUSB driver
    const tinyusb_config_t tusb_cfg = {
        .device_descriptor = &cdc_device_descriptor,
        .string_descriptor = NULL,
        .string_descriptor_count = 0,
        .external_phy = false,
#if (TUD_OPT_HIGH_SPEED)
        .fs_configuration_descriptor = cdc_desc_configuration,
        .hs_configuration_descriptor = cdc_desc_configuration,
        .qualifier_descriptor = &device_qualifier,
#else
        .configuration_descriptor = cdc_desc_configuration,
#endif // TUD_OPT_HIGH_SPEED
    };

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));

    tinyusb_config_cdcacm_t acm_cfg = {
        .usb_dev = TINYUSB_USBDEV_0,
        .cdc_port = TINYUSB_CDC_ACM_0,
        .rx_unread_buf_sz = 64,
        .callback_rx = &tinyusb_cdc_rx_callback,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = NULL,
        .callback_line_coding_changed = NULL
    };

    // Init CDC 0
    TEST_ASSERT_FALSE(tusb_cdc_acm_initialized(TINYUSB_CDC_ACM_0));
    TEST_ASSERT_EQUAL(ESP_OK, tusb_cdc_acm_init(&acm_cfg));
    TEST_ASSERT_TRUE(tusb_cdc_acm_initialized(TINYUSB_CDC_ACM_0));

    // Init CDC 1
    acm_cfg.cdc_port = TINYUSB_CDC_ACM_1;
    acm_cfg.callback_rx = NULL;
    TEST_ASSERT_FALSE(tusb_cdc_acm_initialized(TINYUSB_CDC_ACM_1));
    TEST_ASSERT_EQUAL(ESP_OK, tusb_cdc_acm_init(&acm_cfg));
    TEST_ASSERT_TRUE(tusb_cdc_acm_initialized(TINYUSB_CDC_ACM_1));

    // Install VFS to CDC 1
    TEST_ASSERT_EQUAL(ESP_OK, esp_vfs_tusb_cdc_register(TINYUSB_CDC_ACM_1, VFS_PATH));
    esp_vfs_tusb_cdc_set_rx_line_endings(ESP_LINE_ENDINGS_CRLF);
    esp_vfs_tusb_cdc_set_tx_line_endings(ESP_LINE_ENDINGS_LF);
    FILE *cdc = fopen(VFS_PATH, "r+");
    TEST_ASSERT_NOT_NULL(cdc);

    uint8_t buf[CONFIG_TINYUSB_CDC_RX_BUFSIZE + 1];
    while (true) {
        size_t b = fread(buf, 1, sizeof(buf), cdc);
        if (b > 0) {
            printf("Intf VFS, RX %d bytes\n", b);
            //ESP_LOG_BUFFER_HEXDUMP("test", buf, b, ESP_LOG_INFO);
            fwrite(buf, 1, b, cdc);
        }
        vTaskDelay(1);

        size_t rx_size = 0;
        int itf = 0;
        ESP_ERROR_CHECK(tinyusb_cdcacm_read(itf, buf, CONFIG_TINYUSB_CDC_RX_BUFSIZE, &rx_size));
        if (rx_size > 0) {
            printf("Intf %d, RX %d bytes\n", itf, rx_size);

            // Add 'novfs' to reply so the host can identify the port
            strcpy((char *)&buf[rx_size - 2], "novfs\r\n");
            tinyusb_cdcacm_write_queue(itf, buf, rx_size + sizeof("novfs") - 1);

            tinyusb_cdcacm_write_flush(itf, 0);
        }
    }
}

#endif
