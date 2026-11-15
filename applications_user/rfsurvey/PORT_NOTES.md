# RF Survey (rfsurvey) — ESP32-S3 port notes

Upstream: https://github.com/tbelbek/flipper-rf-survey (MIT), `rf_survey.c` + `rf_logic.h`.
Passive Sub-GHz RX survey: sweep 300–928 MHz, RSSI bins, waterfall / numeric / cadence / hunt
pages, async-RX capture of a located signal to a Flipper RAW `.sub`, CSV logging. **RX only, never TX.**

## What changed for this firmware

### Firmware API exports (components/flipper_application/flipper_application/firmware_api.c)
Added 16 already-existing firmware symbols to the FAP API table (via `tools/add_symbol.py`) plus two
includes (`furi_hal_region.h`, `cc1101_configs.h`). None are new code — purely exposing existing
functions to external apps. These also serve morse_beacon:
`furi_hal_region_get/set/get_name`, `furi_hal_subghz_set_frequency/set_path/start_async_rx/stop_async_rx`,
`furi_hal_power_get_pct`, `flipper_format_write_int32`, `gui_remove_framebuffer_callback`,
`&sequence_set_vibro_on`, `&sequence_reset_vibro`, and the 4 `subghz_device_cc1101_preset_*_async_regs`.
The app's radio layer is **unchanged** — the internal radio keeps using `furi_hal_subghz_set_frequency`
+ `furi_hal_subghz_set_path`; the external radio keeps using the `subghz_devices` ("cc1101_ext") path.
> A firmware rebuild + reflash is required for these exports to take effect on-device.

### Board gate
`rfsurvey_board_supported()` (first line of `rf_survey_app`) gates to the supported ESP32-S3 boards
(T-Embed CC1101, Paradoxt, ThorChimera, Thormini) via `furi_hal_board_supports_ported_apps()`, else a
"Board not supported" dialog. Added `requires` `dialogs`, `subghz`.

### Input — D-pad vs encoder
The spectrum/range/capture screens are custom ViewPorts, so the firmware's automatic encoder remap
does not cover them. `s_is_encoder` is resolved once (`furi_hal_board_get_input_layout()`).
- **D-pad / button boards: behaviour is byte-for-byte unchanged** (every helper is a no-op).
- **Encoder boards (T-Embed, ThorChimera)** — `enc_prefilter()` drops the spurious `OK Long` that ends
  an "OK-held + turn" gesture (it would otherwise toggle recording / confirm).
  **Spectrum view, reworked 2026-11-15.** The first mapping (wheel = page flip) left the Bars-page band
  cursor unreachable, so OK on Bars always zoomed the same band, and Freeze was unreachable too. Now:

  | Action | D-pad (upstream) | Encoder |
  |---|---|---|
  | Bars band cursor | Left/Right | turn wheel |
  | Time window / Numeric row / parked tune | Up/Down | turn wheel |
  | Page flip | hold Left/Right (short off Bars) | OK held + turn |
  | Zoom / park / capture | OK | OK |
  | CSV logging | hold OK | hold OK |
  | Freeze traces | hold Up | hold side key |
  | Clear peaks | hold Down | config page 2 "Clear peaks" row |

  The waterfall hint reads `OK+turn:Pg` and the Help page lists the encoder keys on encoder boards.
  **Capture view:** native axes (wheel = tune, OK-held+turn = modulation); the RSSI gate toggle
  (upstream: hold Left/Right, which a wheel never emits) is on **hold side key**.
  **Range editor:** native axes (wheel = digit value, OK-held+turn = digit cursor), unchanged.

### Radio pinned to Internal (2026-11-15) — the main functional bug
This firmware has no external CC1101: `cc1101_ext` is registered only as an alias that drives the
**internal** chip (`applications/drivers/subghz/cc1101_ext/cc1101_ext.c`), and its `is_connect()` always
returns `true`. Upstream's start-up auto-detect therefore always chose **External**, and that path powered
the BQ25896 OTG 5 V boost for nothing, re-ran `furi_hal_subghz_init()` on every sweep/capture start
(`subghz_devices_begin`) and `furi_hal_subghz_shutdown()` on the internal radio on every stop
(`subghz_devices_end`). `RFS_HAS_EXT_RADIO 0` now pins the radio to Internal: no probe, no OTG, the
Radio row offers only "Internal", and a saved `Radio: 1` from an older build is ignored.

### Cleanup
`boot_trace_to()` neutered to a no-op (upstream wrote developer trace files `w0..w2`, `t_heap*`, `t_boot`,
`t_timer`, `t_fb*` to SD on every call). Body kept under `#if 0` so the ~10 call sites still compile.
The `rf_lab.h`/`rf_tx.h` TX code stays compiled-out via the upstream `__has_include` guard (RX-only build).

## Build / deploy
```
source <esp-idf>/… ; IDF_PATH + xtensa bin on PATH   # see subghz-fap-conventions
./buildFap.sh applications_user/rfsurvey              # -> build_t_embed/fap/rfsurvey.fap
cp build_t_embed/fap/rfsurvey.fap sdcard/apps/SubGhz/
```
Build result: **all 159 API symbols resolved, 0 missing**, no warnings. `firmware_api.c` recompiles clean.

## Post-review fix (2026-11-14, /code-review high)
- **CRITICAL heap overflow fixed** in the RSSI-gated capture prerun flush (`rf_survey.c`, the
  `ind == 512` block inside the `flush_pre` loop). Upstream wrote `!(werr = !flipper_format_write_int32(...))`
  and `break`-ed on a **successful** write, skipping the `ind = 0` reset — so `ind` stayed 512 and the next
  `buf[ind++] = d` wrote one past the `malloc(512*int32)` buffer, then grew unbounded (heap corruption /
  crash). Replaced with the same pattern the live-edge path uses: record errors in `werr` (the capture
  `while` loop exits on `werr`) and always reset `ind`. Reachable whenever capture runs with the gate on.

## Not done
- **No hardware test.** Logic/compile/symbol-resolution verified only. RSSI sweep, async capture, region
  override, OTG power for the external module, and the encoder mapping need on-device confirmation.
