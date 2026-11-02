[← Back to index](README.md)

# Adding a FAP (External App) — the Export-Table Trap

Several Flipper-catalog apps were ported here as pure FAPs (not built into
the firmware) under `applications_user/`. The recurring trap:

- **A FAP can only call symbols already present in `firmware_api.c`'s export
  table.** If the upstream app needs a stock Flipper HAL function this port
  doesn't export yet (very common — this port has gaps like `furi_hal_i2c`,
  `furi_hal_serial`, `furi_hal_pwm` that needed writing from scratch), you
  must add the export via `tools/add_symbol.py` **and rebuild/reflash the
  whole firmware once** — rebuilding only the FAP is not enough, and an old
  firmware will show the app as "Update Firmware… (MissingImports)" rather
  than a crash.
- Most ported hardware (upstream STM32 timers/PWM/UART register pokes) has no
  equivalent on ESP32 and needs a real HAL translation layer, not a stub —
  see `furi_hal_pwm.c` (LEDC-backed, written for the Signal
  Generator/servo-tester FAPs) and `furi_hal_serial.c` (UART1-backed, written
  for the Marauder companion) as the reference implementations.
  **Coordinate before editing these shared HAL files** — more than one FAP
  port has extended `furi_hal_pwm.c` in the same period; treat additions to
  it as append-only unless you've checked for a parallel in-flight change.
- `tools/fam/compile_icons.py` silently **skips** (produces an empty icon
  file for) any icon name that already exists in firmware
  `components/assets/assets_icons.h` — buildFap then reports it as a
  "missing API symbol." The fix is exporting the existing `I_*` symbol, not
  renaming/duplicating the PNG.
- FAP loader note: FAP code+data loads into **PSRAM**, with a 16 KB stack —
  this is heavier on RAM than a built-in app running XIP from flash. If a
  feature is a serious RAM/perf concern, "built-in vs. FAP" is a real design
  choice (ESP Flasher was deliberately kept built-in for this reason), not
  just packaging.
- The ELF loader only supports a specific, bounded set of Xtensa relocation
  types (ABI max 66). A `.fap` reporting relocation types *above* that (e.g.
  68, 140) is not a firmware bug to chase — it means the `.fap` file itself
  is stale, corrupted, or built with a mismatched toolchain; rebuild it from
  source and recopy it to the SD card first before assuming the loader is
  broken.
- New FAPs on paradoxt must follow the input-echo rules in
  [04 — paradoxt input quirks](04-input-quirks.md) and the power/light-sleep
  balance rules in [03 — power management](03-power-management.md)
  (insomnia/inhibit enter↔exit balanced, OTG/USB/BT/PWM released on every
  exit path, including early-failure paths).
