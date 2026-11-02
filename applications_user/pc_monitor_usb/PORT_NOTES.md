# PC Monitor USB - ESP32 port notes

Upstream: https://github.com/DonJulve/Flipper-Zero-PC-Monitor-USB (commit 66100de, GPL-3.0). Pure FAP
(`apps/USB/pc_monitor_usb.fap`). Needs the PC back-end from the upstream README.

## What was changed for this firmware
- **USB serial port**: a stock Flipper always has its CDC port up. Here the USB-C port is the ESP32-S3
  USB-Serial-JTAG (console/flasher) until a TinyUSB composite is installed on demand; only that composite has a CDC
  port the PC back-end can open (it identifies as a Flipper Zero, 0483:5740). The app installs the composite itself and
  removes it on exit, which also brings USB-Serial-JTAG (flashing) back without a reboot. While it runs, the serial
  console on that port is gone (same as qFlipper / USB storage).
- **Ownership**: there is a single CDC port. If qFlipper (lock menu), the CLI or another app owns it, or HID/U2F
  holds USB, the screen says "USB port is busy" instead of taking the port (new `furi_hal_cdc_is_owned()`).
- Light sleep is inhibited while the port is claimed (the USB peripheral would go away).
- Packets: all queued complete packets are drained and the newest shown (the bars no longer lag one packet).
  Connection timeout uses monotonic ticks instead of the RTC (SNTP can move the RTC).
- Back is read through `components/input/input_canonical.h` (one Short per click on this driver).
- "Waiting for connection..." was wider than the 128 px screen; shortened.

## Firmware exports added
`furi_hal_cdc_{set_callbacks,receive,is_owned}`, `furi_hal_usb_composite_{install,uninstall,is_installed}`,
`furi_hal_usb_is_locked`, `usb_cdc_single`, `usb_cdc_dual`, `elements_progress_bar_with_text`,
`sequence_blink_blue_10`, `sequence_display_backlight_on`, `furi_hal_power_light_sleep_inhibit_{enter,exit}`.
