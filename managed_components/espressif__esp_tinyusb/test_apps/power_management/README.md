| Supported Targets | ESP32-H4 | ESP32-P4 | ESP32-S2 | ESP32-S3 |
| ----------------- | -------- | -------- | -------- | -------- |

# Espressif's Additions to TinyUSB - Power management Test Application

This directory contains Unity tests that validate Espressif-specific integration of TinyUSB.

The tests focus on:

- Power management of the USB Device
- Testing tinyusb suspend/resume callbacks delivery
- Testing remote wakeup signalizing by the device

## Running the test locally on Linux host PC:

- User needs to [set permissions](../README.md#set-root-permissions-for-low-level-access-to-usb-devices) to the USB device, to successfully run test app on Linux host PC
