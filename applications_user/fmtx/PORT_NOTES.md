# FM TX - ESP32 port notes

Upstream: https://github.com/yo3gnd/fmtx (commit 7bdfafc, GPL-3.0). Pure FAP (`apps/SubGhz/fmtx.fap`).
MP3 files go to `/ext/apps_assets/fmtx/` (the bundled `0-chiptune.mp3`, `1-monkeys.mp3`, `2-weasel.mp3`).

## What works / what was dropped
- **MP3 -> FM** (decode, 8 kHz resample, sigma-delta PDM, CC1101 FSK deviation through the async-TX RMT path): kept.
- **USB audio source: dropped.** It enumerated the Flipper as a 48 kHz USB speaker through the STM32 USB stack
  (`usb.h`, custom `FuriHalUsbInterface`). This port's USB is a fixed TinyUSB composite (HID+CDC+MSC) without the
  audio class and shares its PHY with the flash/console port, so the Source setting and the USB screen are gone and an
  old stored "USB" choice falls back to MP3.

## What was changed for this firmware
- **Radio**: goes through the Sub-GHz device API on the board's CC1101 (`cc1101_int`). The old firmware also had an
  external CC1101 module on header pins chosen in Sub-GHz > Radio Settings; **this firmware has none**: its `cc1101_ext`
  device is only a second name for the same board radio (`is_connect()` always answers yes, `begin()`/`end()` re-init and
  shut down the chip in use). The External / Auto choice is therefore gone: the Settings row shows `Board CC1101`, a stored
  "External" choice loads as the board radio, and `rfexternal()` is always false. The radio HAL refuses TX outside the
  allowed bands (`furi_hal_subghz_is_tx_allowed`, also used for the VFO's "TX allowed / RX only"; the region database here
  is the wide-open "00" region). The device registry (`subghz_devices_init()` is a furi_check when already initialised)
  is opened in `rfopen()` and closed in the idempotent `rfclose()` on every path.
- `__DMB()` (Cortex-M barrier) -> `__sync_synchronize()`. The radio state (4096-sample ring) that the RMT refill interrupt
  reads is allocated in internal RAM when possible.
- The PDM generator can no longer repeat one bit for ever: the HAL merges equal bits inside the refill interrupt, so
  a saturated (full-scale DC) input would have kept that interrupt spinning. A forced flip after 2048 equal decisions
  (43 ms, never reached by normal audio) fixes that.
- The host tests of `fmtx_rf.c` of the previous firmware modelled its radio HAL and are not part of this tree.
- Buttons: acts on `Short` and on Press/Repeat/Release for the 2 s Left/Right track-change hold; both work with this board's
  driver (one Short per click, Repeat while an arrow is held).

## Firmware exports added
`subghz_devices_{flush_tx,is_async_complete_tx}`, `furi_hal_subghz_{is_frequency_valid,is_tx_allowed}`,
`furi_hal_power_insomnia_{enter,exit}`, `canvas_draw_bitmap`, `dialog_ex_reset`, `submenu_get_selected_item`,
`storage_file_{eof,get_error}`, `saved_struct_save`.

## Not verified on hardware
Real-time MP3 decode speed from PSRAM-resident code, RMT refill timing at ~48 kHz and the audio quality on a receiver.
