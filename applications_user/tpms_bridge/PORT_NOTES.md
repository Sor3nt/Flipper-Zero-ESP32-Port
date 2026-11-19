# TPMS Bridge (tpms_bridge) — ESP32-S3 port notes

Upstream: https://github.com/VishovVladimir/flipper-zero-tpms (`flipper/tpms_bridge`). Reads car
tyre-pressure sensors on 433.92 / 315 MHz with the CC1101, decodes 39 rtl_433-derived protocols
on-device, shows them on screen, and streams frames over a CLI command `tpms_rx` (JSON or raw-interval).
A 125 kHz LF "wake" pulse (RFID coil) can wake parked sensors.

## What changed for this firmware

### Firmware API exports (firmware_api.c)
The app compiled against this firmware's headers with **no source changes** to the decoder/radio/CLI
code — only 9 pre-existing firmware symbols needed exposing to external apps (via `tools/add_symbol.py`),
plus a `#include <furi_hal_rfid.h>`:
`cli_registry_add_command_ex`, `pipe_spaces_available`, `pipe_state`, `furi_hal_rfid_tim_read_start`,
`furi_hal_rfid_tim_read_stop`, `furi_hal_rfid_pins_reset`, `furi_stream_buffer_reset`,
`furi_thread_get_current`, `furi_thread_set_signal_callback`.
> A firmware rebuild + reflash is required for these exports to take effect on-device.

The CC1101 FSK preset (`tpms_preset.h` → `<cc1101_regs.h>`), the `subghz_devices` async-RX path, and the
CLI pipe/args API all resolved against existing firmware headers unchanged.

### Board gate
`tpms_board_supported()` (first line of `tpms_bridge_app`) gates to T-Embed CC1101, Paradoxt,
ThorChimera, Thormini. Added `requires` `dialogs`, `subghz`.

### Input — D-pad vs encoder (reworked 2026-11-15)
Custom ViewPort, so the firmware's automatic encoder remap does not apply. `s_is_encoder` /
`app->is_encoder` resolved once from `furi_hal_board_get_input_layout()`.

The first port fed the encoder the upstream D-pad map unchanged, which **broke the encoder**: picking a
list row needs *Long* Up/Down, and a wheel only emits Press/Short/Release, so on T-Embed / ThorChimera no
sensor except the first could be selected from the list. "OK hold = clear list" also shared its start
with "OK held + turn", so turning later than 500 ms wiped the list.

| Action | D-pad / buttons (upstream, unchanged) | Encoder (`tpms_bridge_handle_encoder`) |
|---|---|---|
| Pick sensor row (list) | hold Up/Down | turn wheel |
| Band / modulation (list) | Up/Down | OK held + turn |
| Prev/next sensor (detail) | Up/Down | turn wheel |
| Open detail / back to list | OK | OK |
| Back / exit | Back | side key |
| Clear sensor list | hold OK | hold side key (OK Long ignored) |

Footer hints and the empty-list help follow the layout: encoder `OK+turn:band`, D-pad `hold U/D:select`.

### LF wake — not available on this hardware
Upstream wakes parked sensors with a 125 kHz CW field from the Flipper's RFID coil. **None of the
supported boards can do that**: T-Embed / Paradoxt have an RDM6300 (UART reader, decodes EM4100 itself,
no coil control) and ThorChimera / Thormini have no RFID. `furi_hal_rfid_tim_read_start/stop` are empty
stubs. So upstream's Right (wake), Left (auto-wake), the `wake` header flag and CLI `wake` silently did
nothing. `TPMS_LF_WAKE_SUPPORTED 0` (tpms_lf.h) now disables them honestly: wake keys are inert, hints say
"No LF wake: drive car", CLI `wake` prints "not supported, ignored". **Consequence:** a parked sensor stays
silent; it transmits when the wheel turns (driving), on a pressure change, or when triggered by an
external TPMS activation tool. RX/decoding itself is unaffected.

## Build / deploy
`./buildFap.sh applications_user/tpms_bridge` → all symbols resolved, 0 missing, no warnings; deployed to
`sdcard/apps/SubGhz/tpms_bridge.fap`. `firmware_api.c` recompiles clean.

## Post-review fix (2026-11-14, /code-review high)
- **Wake-pulse wrap-safety:** `tpms_session_wake_pulse()` used `while(furi_get_tick() < deadline)`, which is
  not wrap-safe — within the ~50-day tick-rollover window the deadline wraps below `now` and the 125 kHz LF
  field would fire for ~0 ms (failing to wake the sensor). Changed to an unsigned elapsed-tick test
  (`(furi_get_tick() - start) < wait_ticks`), consistent with the signed/elapsed-diff timing elsewhere.

## Notes / not done
- **No hardware test.** Local RX + decoding, the `tpms_rx` CLI stream, radio hand-off between local RX and
  a CLI session, and the 125 kHz LF wake pulse need on-device confirmation.
- Decoders are a 1:1 port of rtl_433 (credited upstream); only the Renault group was hardware-verified
  upstream, the rest against rtl_433 captures.
- The CLI bridge streams over whatever transport the firmware's CLI/VCP uses; which physical port carries
  it is the firmware's decision, not this app's.
