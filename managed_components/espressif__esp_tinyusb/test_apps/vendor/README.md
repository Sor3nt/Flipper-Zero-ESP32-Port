| Supported Targets | ESP32-H4 | ESP32-P4 | ESP32-S2 | ESP32-S3 |
| ----------------- | -------- | -------- | -------- | -------- |

# Espressif's Additions to TinyUSB - Vendor specific device Test Application

This directory contains Unity tests that validate Espressif-specific integration of TinyUSB.

The tests focus on:

- Creating a vendor specific device using esp_tinyusb and validating communication with the device using pyusb

## Running the test locally on Linux host PC:

- User needs to [set permissions](../README.md#set-root-permissions-for-low-level-access-to-usb-devices) to the USB device, to successfully run test app on Linux host PC
