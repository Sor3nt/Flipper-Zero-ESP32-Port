# ProtoView (protoview) — ESP32-S3 port notes

Upstream: antirez's ProtoView via https://github.com/xMasterX/all-the-plugins (`base_pack/protoview`).
A Sub-GHz digital signal **analyzer / visualizer / editor / replay** tool: shows received pulses as a
waveform, decodes known protocols (TPMS, Oregon2, KeeLoq, B4B1, …), lets you inspect fields, build and
retransmit messages, and save/load signals. Uses the normal `subghz_devices` async RX/TX path.

## What changed for this firmware

### Direct Sampling disabled (STM32-only hardware)
Upstream's "Direct Sampling" screen samples the CC1101 GDO0 pin with the **STM32 `TIM2` timer** and the
**ARM `DWT` cycle counter** — neither exists on ESP32-S3. This is a niche debug visualiser; the core
detect/decode/replay features do **not** use it (it is gated on `debug_timer_sampling`, which upstream
hard-disables via `else if(0 && …)` in view_settings.c). Changes:
- `app.c`: the one `DWT->CYCCNT` init → `0`.
- `app_subghz.c`: `protoview_timer_isr` + `raw_sampling_worker_start/stop` stubbed to no-ops (these were
  only reachable through the disabled `debug_timer_sampling` path).
- `view_direct_sampling.c`: `ds_timer_isr` removed and `direct_sampling_timer_start/stop` stubbed. The
  Direct Sampling screen still renders (shows its usage text) but captures nothing; `view_enter`/`view_exit`
  still stop and restart the normal async RX, so leaving the screen fully restores reception.

### Firmware API exports (firmware_api.c)
4 pre-existing firmware symbols exported (via `tools/add_symbol.py`); all headers were already included:
`&message_delay_10`, `&message_note_g1` (decode beep), `name_generator_make_auto` (auto .sub filenames),
`subghz_devices_get_data_gpio` (radio data pin).
> A firmware rebuild + reflash is required for these exports to take effect on-device.

### Board gate
`protoview_board_supported()` (first line of `protoview_app_entry`) gates to T-Embed CC1101, Paradoxt,
ThorChimera, Thormini. Added `requires` `storage`, `dialogs`, `subghz`.

### Input — D-pad vs encoder
Custom ViewPort with a global Left/Right = prev/next view and per-view Up/Down + OK. `s_is_encoder` resolved
once. Native encoder delivery already suits this app (wheel = Up/Down value edits: zoom, frequency/modulation,
field scroll; OK-held+turn = Left/Right view switch), so **no axis remap** — only `pv_enc_prefilter()` drops
the spurious `OK Long` ending an "OK-held + turn" gesture, which on the settings view would otherwise reset
frequency/modulation to default. D-pad behaviour unchanged.
- Encoder note: switch views with OK-held + turn; set frequency/modulation and zoom with the plain wheel.

### Fixes 2026-11-15 — did not work like upstream
- **Radio pinned to Internal.** Upstream calls `radio_device_loader_set(..., ExternalCC1101)`. On this
  firmware `cc1101_ext` is only an alias for the internal chip and always reports connected, so the app
  always ran on it: OTG 5 V boost on, `furi_hal_subghz_init()` re-run on begin, and on exit
  `subghz_devices_end()` → `furi_hal_subghz_shutdown()` on the internal radio. Now `SubGhzRadioDeviceTypeInternal`.
- **Uninitialised app state.** ESP-IDF `malloc` does not zero (Flipper's does). `app->radio_device` was
  passed to the loader as "current device" before ever being set (masked only by the bogus external path).
  `ProtoViewApp` and `ProtoViewTxRx` are now `memset` to 0 (txrx fields were set piecemeal; zeroing
  makes that independent of the allocator).
- **False decodes.** `convert_signal_to_bits()` writes only the bits the signal covers, while the decoders
  search the whole bitmap for preambles/sync. The tail was leftover heap instead of zeros, so decoders
  could lock onto garbage. The bitmap is now zeroed before conversion.
- Signal resend/TX benefits from the firmware RMT duration fix (see morse_beacon PORT_NOTES; reflash).

### Encoder key map (2026-11-15)
Panning the raw signal needs Left/Right **Repeat** (key held), which a wheel never emits, so it was
unreachable. On the encoder, **Back short on the main (raw) screen** — upstream only showed "Long press to
exit" there — toggles the wheel between **zoom** (upstream Up/Down) and **pan** (1 sample per detent); a
`PAN` badge shows the mode, Back held still exits. Build view footer reads `OK+turn +/-` on the encoder.
Everything else is native: OK held + turn = previous/next view (and ±1 on Build fields, row scroll on the
Info save sub-view), wheel = scale / frequency / modulation / field select. The ±10 Build-field step
(Left/Right Repeat) has no encoder gesture; use OK to type the value. D-pad behaviour unchanged.

## Build / deploy
`./buildFap.sh applications_user/protoview` → all symbols resolved, 0 missing, no warnings; deployed to
`sdcard/apps/SubGhz/protoview.fap`. `firmware_api.c` recompiles clean.

## Post-review fixes (2026-11-14, /code-review high)
Both are in the (already-inert) Direct Sampling view, so neither is reachable in a harmful way on this port,
but fixed for correctness:
- `usec_per_pixel` adjust computed `uint32_t change = -change` (underflow); at the minimum resolution an Up
  step wrapped huge and the clamp snapped to MAX instead of MIN. Now computed in signed `int32_t`.
- The capture bitmap is now `memset`-zeroed on view enter — with the sampler stubbed nothing fills it, so the
  screen would otherwise read uninitialised heap; it now renders blank.

## Not done
- **No hardware test.** RX waveform, protocol decode, field edit/replay (TX), save/load `.sub`, frequency/
  modulation changes, and the encoder mapping need on-device confirmation.
- **Direct Sampling is intentionally inert** (no STM32 TIM2 on ESP32-S3). A future port could reimplement it
  with an ESP32 GPTimer + GPIO read if desired.
