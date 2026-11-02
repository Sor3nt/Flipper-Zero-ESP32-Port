# Click Recorder - ESP32 port notes

Upstream: https://github.com/0x78f1935/Click-Recorder (commit e39b3b3, GPL-3.0). Pure FAP (`apps/USB/mouse_click_recorder.fap`).

## What was changed for this firmware
- **Buttons** (paradoxt driver, `BOARD_INPUT_SHORT_AFTER_RELEASE`): a click arrives as physical Press/Release
  followed by one Short event, and OK/Back never send `InputTypeLong`. Before the driver fix, Computer Mouse could
  double-click and "long Back" (quit menu / leave the app from the main menu) was unreachable. Input goes through
  `components/input/input_canonical.h`, which restores the stock stream: tap = Press, Short, Release; arrow held =
  Press, Repeat..., Release; OK/Back held >= 600 ms = Press, Long, Release (Long is sent on release).
- **No more `furi_check()` on HID start**: `furi_hal_usb_set_config(&usb_hid)` fails while a USB composite (qFlipper,
  USB storage) or U2F is active, `bt_profile_start()` fails when the BLE stack is busy. Both end in a "HID error"
  screen instead of resetting the device. BLE advertising is only started when Bluetooth is enabled (as BadUSB does).
- Light sleep is inhibited while a transport is active (10 ms playback timer, BLE link). `hid_deinit` lets an in-flight
  timer callback leave before the transport goes away; every HID call is guarded by `hid_active`.
- Input queue is no longer filled with `FuriWaitForever` from the GUI thread; only Repeat may be dropped (a lost
  Release would leave a mouse button held).
- USB identity uses the firmware's HID defaults. BLE pairing is the HID profile default (numeric comparison).

## Firmware exports added
`furi_hal_hid_mouse_{move,press,release}`, `ble_profile_hid_mouse_{move,press,release}`, `sequence_set_blue_255`,
`furi_hal_power_light_sleep_inhibit_{enter,exit}`.
