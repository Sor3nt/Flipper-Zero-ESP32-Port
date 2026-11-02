[← Back to index](README.md)

# BLE / Pairing — Know the Current (Partly Reverted) State Before Changing It

The Bluetooth stack here is **Bluedroid**, not NimBLE — any pasted/ported
code that assumes NimBLE APIs (`nimble_port_init`, `ble_gap_disc`, …) will not
compile and has to be rewritten against the Bluedroid GAP/GATT APIs actually
in this tree (see `ble_spam/ble_spam_hal.c` and `ble_tracker_hal.c` for the
reference pattern).

A full SC+MITM pairing security overhaul (new `ble_core` component, encrypted
GATT perms, numeric-comparison confirm dialogs) was built and then, after
several rounds of real hardware testing kept breaking the **Flipper Mobile
app's serial/RPC connection**, the **serial/RPC connection path specifically**
was reverted back to the original, known-working, unauthenticated pairing —
while keeping the rest of the BLE work (HID/BadUSB security via `ble_core`,
BLE Spam, wardriving scanners, etc.) on the new stack. Practical implications:

- `components/ble_serial/ble_serial.c` and the serial-specific parts of
  `btshim.c`/`bt_i.h` are intentionally back to the pre-overhaul behavior.
  Don't "reapply" the SC/MITM pairing UI to serial/RPC without a hardware
  test plan — it broke phone connectivity three times in a row before being
  reverted.
- `ble_core`, `ble_hid.c` (HID/BadUSB pairing security) and the newer BLE
  scanners are **not** reverted and still use the hardened path.
- Momentum's "Allow BLE RPC While Locked" setting defaults to **true** here
  (flipped from Momentum's own `false` default) specifically so a locked
  device doesn't silently refuse Flipper Mobile's RPC session. If you touch
  `momentum_settings.c` defaults, know that an existing saved settings file
  on SD overrides the new default — only fresh/absent/version-bumped
  settings files pick it up.
- If "pairing shows no PIN / fails silently / retries the same way" ever
  comes back as a report, check for a **stale bond on the phone first** —
  Bluedroid silently rejects an LTK request from a stale bond with no
  app-visible event, which looks exactly like "nothing happens." This is
  expected after any pairing-security change; the phone has to forget and
  re-pair once.
