/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "soc/soc_caps.h"

#if SOC_USB_OTG_SUPPORTED

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "unity.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"

#define DEVICE_DETACH_TEST_ROUNDS       10
#define DEVICE_DETACH_ROUND_DELAY_MS    150
#define VBUS_GPIO_NUM                   GPIO_NUM_6

// Set to 1 to simulate the VBUS signal using GPIO. This is useful for automatic CI testing with no user interaction.
// Set to 0 to use the real VBUS signal from the USB port. This is useful for manual testing with a USB cable.
//     VBUS must be connected thorough a resistor divider to the GPIO.
#define SIMULATED_VBUS_SIGNAL           (1)

static unsigned int dev_mounted = 0;
static unsigned int dev_umounted = 0;

/**
 * @brief TinyUSB callback for device event
 *
 * @note
 * TinyUSB generates Attach event after the Host sends the SetConfiguration() request.
 * - Linux-based Hosts: SetConfiguration() is part of enumeration process.
 * - Win-based Hosts: SetConfiguration() request is present only if Windows has a driver for the device.
 */
void test_dconn_event_handler(tinyusb_event_t *event, void *arg)
{
    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        printf("ATTACHED event %d\n", dev_mounted);
        dev_mounted++;
        break;
    case TINYUSB_EVENT_DETACHED:
        printf("DETACHED event %d\n", dev_umounted);
        dev_umounted++;
        break;
    case TINYUSB_EVENT_SUSPENDED:
        printf("SUSPENDED event\n");
        break;
    case TINYUSB_EVENT_RESUMED:
        printf("RESUMED event\n");
        break;
    default:
        printf("Unprocessed event %d\n", event->id);
        break;
    }
}

/**
 * @brief TinyUSB Disconnect Detection test case
 *
 * In this test case, we either simulate VBUS on a GPIO or rely on the real VBUS signal.
 * The VBUS monitor only generates a TinyUSB DCD event on the falling edge (VBUS drop).
 *
 * Test logic:
 * - Install TinyUSB Device stack without any class
 * - In cycle:
 *      - Emulate the detachment (VBUS high -> low)
 *      - Verify the DETACHED event counter increases
 * - Verify that dev_umounted == DEVICE_DETACH_TEST_ROUNDS, where DEVICE_DETACH_TEST_ROUNDS is amount of rounds
 * - Uninstall TinyUSB Device stack
 *
 * SIMULATED_VBUS_SIGNAL usage:
 * - Set SIMULATED_VBUS_SIGNAL to 1 to drive VBUS_GPIO_NUM as output and toggle it high/low.
 *   This is useful for automated CI testing with no user interaction.
 * - Set SIMULATED_VBUS_SIGNAL to 0 to use the real VBUS signal from the USB port.
 *   VBUS must be connected through a resistor divider to VBUS_GPIO_NUM for manual testing.
 */
TEST_CASE("dconn_detection", "[esp_tinyusb][dconn]")
{
    dev_mounted = 0;
    dev_umounted = 0;

    gpio_install_isr_service(ESP_INTR_FLAG_LOWMED);

    // Install TinyUSB driver
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(test_dconn_event_handler);
    tusb_cfg.phy.self_powered = true;
    tusb_cfg.phy.vbus_monitor_io = VBUS_GPIO_NUM;
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));

#if SIMULATED_VBUS_SIGNAL
    // Enable output path for VBUS GPIO so we can manually control the VBUS signal
    // Input path is already enabled by the TinyUSB driver
    gpio_set_level(VBUS_GPIO_NUM, 1);
    gpio_set_direction(VBUS_GPIO_NUM, GPIO_MODE_INPUT_OUTPUT); // gpio_output_enable() is not available in older idf versions

    for (unsigned int i = 0; i < DEVICE_DETACH_TEST_ROUNDS; i++) {
        gpio_set_level(VBUS_GPIO_NUM, 1);
        vTaskDelay(pdMS_TO_TICKS(DEVICE_DETACH_ROUND_DELAY_MS));
        gpio_set_level(VBUS_GPIO_NUM, 0);
        vTaskDelay(pdMS_TO_TICKS(DEVICE_DETACH_ROUND_DELAY_MS));
    }

    // We cannot generate ATTACH events by manipulating the VBUS signal
    // ATTACH events are generated only if we force re-enumeration of the device
    //TEST_ASSERT_EQUAL(dev_umounted, dev_mounted);
    TEST_ASSERT_EQUAL(DEVICE_DETACH_TEST_ROUNDS, dev_umounted);
#else
    // Just an endless loop to keep the test running
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(DEVICE_DETACH_ROUND_DELAY_MS));
    }
#endif // SIMULATED_VBUS_SIGNAL
    // Cleanup
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());
    gpio_uninstall_isr_service();
}
#endif // SOC_USB_OTG_SUPPORTED
