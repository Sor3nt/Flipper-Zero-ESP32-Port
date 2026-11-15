# Music to Sub-GHz (fmf_to_sub) — ESP32-S3 port notes

Upstream: https://github.com/jamisonderek/flipper-zero-tutorials (`subghz/fmf_to_sub`). **File converter,
no radio**: reads a Flipper Music file (`.fmf` or RTTTL-style `.txt`) and writes a Flipper SubGhz RAW
`.sub` (the chosen frequency/preset as header, note tones as `RAW_Data` pulses). Played later via Read RAW.

## What changed for this firmware

### Firmware API exports (firmware_api.c)
Compiled with no structural changes. Three pre-existing firmware symbols exported (via `tools/add_symbol.py`):
`flipper_format_buffered_file_close`, `storage_file_truncate`, `view_set_custom_callback`.
> A firmware rebuild + reflash is required for these exports to take effect on-device.

### Upstream bug fixes
1. **NULL-notes crash (two places).** If the picked file could not be opened/parsed, upstream left
   `error = false` and then dereferenced a NULL `notes` string in `furi_string_size()`, crashing the app.
   Fixed: `fmf2sub_load_txt_file()` now sets `error = true` on an open failure, and `fmf2sub_save_sub_file()`
   guards `model->data.notes == NULL` → clean `Fmf2SubStateError` instead of a crash.
2. **Doubled slash in output path.** `EXT_PATH("/subghz/")` with `"%s/%s"` produced
   `/ext//subghz//Flip1.sub`; changed to `EXT_PATH("subghz")` → `/ext/subghz/Flip1.sub`.

Left as-is (noted, not changed to avoid altering note parsing): the redundant invalid-note check
`if(ch < 'A' && ch > 'G' && ...)` is always false, but the later `else` in the note switch already rejects
non-notes, so behaviour is unchanged.

### Board gate
`fmf_board_supported()` (first line of `fmf_to_sub_app`) gates to T-Embed CC1101, Paradoxt, ThorChimera,
Thormini — for consistency with the ported set (the converter is board-agnostic). Added `requires`
`storage`, `dialogs`.

### Input / icons
Standard `view_dispatcher` + submenu / variable_item_list / widget + one custom Convert `View` (Left/Right/OK).
The firmware GUI drives the standard modules on both dpad and encoder. `fap_icon_assets="assets"` →
`I_fmf_10x10` (file-browser icon) compiled with Pillow.

**Convert view on the encoder (fixed 2026-11-15).** The earlier note ("Left/Right via OK-held+turn, usable")
was wrong: upstream opens the file browser on OK *Press*, and the encoder emits an OK Press at the start of
every OK-held+turn, so changing the FlipBoard button opened the browser instead; the wheel did nothing.
Now, on `FuriHalBoardInputLayoutEncoder` only: the view is `ViewInputModeLeftRight` (wheel = previous/next
FlipBoard button) and the browser opens on OK *Short*. D-pad / buttons: unchanged (Left/Right, OK Press).

### ESP32 runtime fixes (2026-11-15) — the app did not work like upstream
- **Uninitialised model → crash.** Flipper's `malloc` returns zeroed memory; ESP-IDF's does not. Upstream
  never initialised the Convert model, so `data.notes` held garbage and the first file load (or leaving the
  Convert screen) called `furi_string_free()` on it. The model and the app struct are now `memset` to 0.
- **Abort on the last note.** The note parser reads one character past the end of the notes string (the
  `#` look-ahead and the octave-digit loop at the final note; `fmf2sub_extract_param` likewise). Flipper
  release builds return `'\0'` there; this firmware has assertions on and M*LIB's `string_get_char()`
  asserts `index < size` → abort on practically every song. All reads go through `fmf2sub_char_at()`,
  which returns `'\0'` past the end (same result Flipper produced).
- **Slow conversion.** One `storage_file_write` per ~10-byte pulse pair (thousands per song) froze the
  screen for seconds; output is now buffered and written in 4 KiB chunks (identical file contents).
- Removed the deprecated no-op `view_dispatcher_enable_queue()` (build is warning-free again).

## Build / deploy
`./buildFap.sh applications_user/fmf_to_sub` → all symbols resolved, 0 missing, no warnings; deployed to
`sdcard/apps/SubGhz/fmf_to_sub.fap`. `firmware_api.c` recompiles clean.

## Post-review fixes (2026-11-14, /code-review high)
- **TXT last-note truncation fixed:** the notes read loop `while(storage_file_read(..) && !storage_file_eof(..))`
  dropped the final byte (eof turns true right after the last byte is read), truncating the last note when the
  `.txt` had no trailing newline. `storage_file_read` already returns 0 at EOF, so the `&& !eof` was removed.
- **Dead invalid-note guard fixed:** `if(ch < 'A' && ch > 'G' && ...)` was a contradiction (always false).
  Replaced with a correct negated-range test. (The reviewer's suggested `||` form was itself wrong — it would
  have skipped valid notes; this uses `!((ch>='A'&&ch<='G')||(ch>='a'&&ch<='g')||ch=='P'||ch=='p')`.)

## Not done
- **No hardware test.** File pick, FMF/TXT parsing, `.sub` output format, and playback need on-device checks.
