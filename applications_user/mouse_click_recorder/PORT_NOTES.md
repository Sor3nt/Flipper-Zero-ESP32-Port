# Click Recorder - ESP32 port notes

Upstream: https://github.com/0x78f1935/Click-Recorder (commit e39b3b3, GPL-3.0). Pure FAP (`apps/USB/mouse_click_recorder.fap`).

## USB transport
- Takes the USB port as an exclusive lease (`furi_hal_usb_lease_acquire`) and selects HID with `set_config_for_owner`.
  Refused (the screen says "USB is busy") while USB is locked, used by an RPC session, USB storage or the USB-UART bridge.
  The old version called `furi_hal_usb_unlock()` first, which would have pulled USB away from a running RPC session: gone.
- Release: HID is stopped, `furi_hal_usb_hid_release_for_owner` tears the TinyUSB HID stack down and puts the PHY back on
  USB-Serial-JTAG (plain HID stop leaves the stack installed, the host keeps seeing the mouse and the flash/console port
  stays off), then `furi_hal_usb_lease_release` restores the previous interface and resumes a paused qFlipper session.
  A failed release keeps the lease (and the sleep veto) and is retried; it is also retried at exit.
- A held button is released before the transport goes away (it used to be dropped when going back to the mode menu).
- Starting HID can fail (busy, TinyUSB) and `bt_profile_start()` can fail: both end in the "HID error" screen, never a reset.

## BLE
Advertising only when Bluetooth is enabled; pairing is the HID profile default. Switching USB -> BLE first releases any USB
lease left over from a failed release.

## Input
All board drivers send the stock stream (tap = Press, Short, Release; hold = Press, Long, Repeat, Release), so `input_canonical.h`
is not used. `hid_active` is atomic (the 10 ms playback timer reads it while the transport goes away).

Light sleep is inhibited while a transport is active; supported boards only: T-Embed, Paradoxt, ThorChimera, Thormini.
