# Morse Beacon (morse_beacon) — ESP32-S3 port notes

Upstream: https://github.com/jdmills-edu/flipper-morse-beacon. Sub-GHz CW/MCW station-ID and text
keyer. Keys Morse over the CC1101 via `subghz_devices_start_async_tx` + a `LevelDuration` callback
(OOK true-CW, MCW audio-tone, SSB offset). Beacon triggers on activity/interval; "Send text" keys
typed text; "Remote text" keys lines received over a serial link.

## What changed for this firmware

### BLE serial profile include path (link.c)
Upstream `#include <profiles/serial_profile.h>` → this firmware's path
`#include <ble_profile/extra_profiles/serial_profile.h>`.

### Remote text: BLE kept, UART removed (link.c, morse_beacon.c)
This firmware exposes **no `furi_hal_serial` API** (no `furi_hal_serial_control_acquire/init/async_rx/tx`),
so the GPIO 13/14 UART remote-text path cannot be ported. Changes:
- `link.c`: removed the `FuriHalSerialHandle* serial` field and the three UART functions
  (`link_uart_callback`, `link_start_uart`, `link_stop_uart`). `morse_link_start()` now rejects any
  non-BLE source defensively; `morse_link_stop()`/`morse_link_reply()` are BLE-only.
- `morse_beacon.c`: the "Remote" setting offers **one** source (BLE) so "UART 13/14" can't be selected;
  `link_source` is forced to `LinkSourceBle` (also clamped in config.c). The `LinkSourceUart` enum value
  is left in `link.h` so the status-string branches still compile (they're now unreachable).
- The **BLE** remote-text path is unchanged and fully supported (`ble_profile_serial_*`, `bt_profile_start`,
  `furi_hal_bt_start_advertising`, RPC-callback reclaim on connect).

### Region header include (radio.c, morse_beacon.c)
This firmware's `<furi_hal.h>` does not pull in the region API, so added `#include <furi_hal_region.h>`.
TX is gated by `furi_hal_region_is_frequency_allowed()` (kept from upstream; now exported), and the About
screen shows `furi_hal_region_get_name()`.

### Firmware API exports (firmware_api.c)
Added (via `tools/add_symbol.py`): `furi_hal_region_is_frequency_allowed`, `canvas_glyph_width`,
`elements_scrollbar`, `furi_message_queue_get_space`, `furi_string_vprintf` (region get/set/get_name and
the CC1101 presets were already added for rfsurvey). All are pre-existing firmware functions.
> A firmware rebuild + reflash is required for these exports to take effect on-device.

### Board gate
`morse_board_supported()` (first line of `morse_beacon_app`) gates to T-Embed CC1101, Paradoxt,
ThorChimera, Thormini. Added `requires` `dialogs`, `bt`, `subghz`; dropped unused `fap_icon_assets`.

### Input
No custom encoder remap needed: the custom views (beacon/sending/link/about) use only OK/Back/Up/Down,
which the encoder delivers natively; the settings screen is a standard `variable_item_list` (the firmware
GUI already remaps the wheel for it). D-pad behaviour is unchanged.

### Fixes 2026-11-15 — keying did not work like upstream
- **Morse timing garbled (firmware HAL bug, fixed in `components/furi_hal/furi_hal_subghz.c`).**
  Async TX splits each run into RMT symbols of at most `FURI_HAL_SUBGHZ_RMT_MAX_DURATION_TICKS`, which was
  `UINT16_MAX`, but `rmt_symbol_word_t.duration0/1` are **15-bit**. Any run over 32.767 ms was stored
  modulo 32768 and a chunk of exactly 32768 became 0 (RMT end marker, TX stops). At beacon speeds every
  element is longer than that (a 20 WPM dot is 60 ms), so dots/dashes/gaps came out wrong. Limit is now
  `0x7FFF` (same as `IR_RMT_MAX_DURATION`). **Needs a firmware rebuild + reflash**; it also fixes long
  gaps in any other Sub-GHz TX (RAW files, other TX apps).
- **Radio pinned to Internal.** This firmware has no external CC1101: `cc1101_ext` is an alias that drives
  the internal chip and always reports connected; its begin/end re-run `furi_hal_subghz_init()` /
  `furi_hal_subghz_shutdown()` on the internal radio. The "Radio" row now offers only "Internal" and
  `morse_radio_acquire()` forces `external_radio = false` (also overrides an old saved "External").
- **View models zeroed.** ESP-IDF `malloc` does not zero (Flipper's does); the four custom view models
  (beacon/sending/link/about: text buffers, counters, flags) are now cleared after allocation, so no view
  can draw a garbage, unterminated string before its first update.

Input re-checked: the custom views use only OK / Back / Up/Down (native on the encoder) and the rest are
standard modules, so no key-map change was needed; the hints ("ID now", "Back to stop") are key-neutral.

## Build / deploy
`./buildFap.sh applications_user/morse_beacon` → **all 153 symbols resolved, 0 missing**, no warnings;
deployed to `sdcard/apps/SubGhz/morse_beacon.fap`. `firmware_api.c` recompiles clean.

## Not done
- **No hardware test.** TX keying, BLE pairing/remote-text, region gating, and the beacon state machine
  need on-device confirmation.
- UART remote-text is intentionally absent (no firmware UART HAL). Use BLE.
