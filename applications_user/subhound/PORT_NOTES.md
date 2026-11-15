# SubHound (subhound) — ESP32-S3 port notes

Upstream: https://github.com/maxwalks/subhound (`flipper-app`). **Offline** Sub-GHz capture classifier:
opens a `.sub` file, parses it, extracts ~30 features, classifies into one of 17 device categories, shows
a report, and writes `<capture>.report.txt` + `<capture>.bra` sidecars. **No radio, no region, no power** —
pure file analysis + GUI.

## What changed for this firmware

### Firmware API exports (firmware_api.c)
The app compiled with **no source changes to the analyzer**. Two pre-existing firmware symbols needed
exposing (via `tools/add_symbol.py`): `roundf` (added an `extern float roundf(float);` next to the other
math externs, since newlib's math.h under `-std=gnu17` doesn't declare it here) and `widget_add_line_element`.
> A firmware rebuild + reflash is required for these exports to take effect on-device.

### Board gate
`subhound_board_supported()` (first line of `subhound_app`) gates to T-Embed CC1101, Paradoxt, ThorChimera,
Thormini — for **consistency** with every other ported FAP (docs/ported-faps.md). Subhound is itself
board-agnostic; the gate only keeps launcher behaviour uniform across the ported set. `requires` already
had `gui`, `storage`, `dialogs` (the gate dialog).

### Input
Submenu / text_box / loading / file browser need no encoder work. **Exception: the summary widget.**
Its Left "Back" / Right "Full" buttons put the widget in `ViewInputModeLeftRight`, so on the rotary
encoder turning the wheel fired Back/Full, and the body only scrolled with OK-held+turn. On
`FuriHalBoardInputLayoutEncoder`, the summary now shows only Center "Menu" and is forced to
`ViewInputModeUpDown`: the wheel scrolls, OK opens the menu (which has "Full report"), and the Back key
returns to the file picker. D-pad and button boards keep the upstream three-button layout unchanged.

### Icons
`fap_icon_assets="assets"` → `sub1_10px.png` compiles to `I_sub1_10px` (used as the file-browser icon via the
generated `subhound_icons.h`). This needed Pillow in the build Python (installed in the build env).

### Bug fixes over upstream (2026-11-15)
Upstream (`ea0976a`, the catalog-pinned commit) has these bugs on stock Flipper too; they are not port regressions.
- **Blank screen after Back on an error.** When a parse failed, the error TextBox sent Back to
  `SubhoundViewSummary`. That widget was never built for the current file, so the user got a blank
  screen (or the *previous* file's summary) and had to press Back a second time to reach the file picker.
  Now the error TextBox's Back returns to the picker directly (`s_textbox_is_error`), and the
  summary/sections are cleared on error.
- **Unclear error for non-BinRAW files.** Only BinRAW captures (`Bit_RAW`/`Data_RAW`) can be analysed.
  RAW recordings (`Protocol: RAW`, no `TE`) failed with "Missing TE" and decoded keys (Princeton, ...)
  with "No Data_RAW". The parser now records `Protocol:` and explains how to capture BinRAW
  (Sub-GHz > Read > Config > Bin RAW: ON).

## Build / deploy
`./buildFap.sh applications_user/subhound` → **all 88 symbols resolved, 0 missing**, no warnings (9 C files);
deployed to `sdcard/apps/SubGhz/subhound.fap`. `firmware_api.c` recompiles clean.

## Not done
- **No hardware test.** File-browser pick, `.sub` parsing, the classifier output, and sidecar writing need
  on-device confirmation. (All logic is deterministic file processing, so risk is low.)
