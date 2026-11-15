# Agentic Remote USB (claupper) — ESP32 port notes

Upstream: https://github.com/Wet-wr-Labs/claupper (MIT, appid `claude_remote_usb`,
`main` branch = the USB build). A one-handed USB HID keyboard remote for AI coding
agents (Claude Code): 5-button D-pad sends keystrokes, plus macros, an offline
manual and a quiz. Single file, `claude_remote.c` (~3300 lines).

## What is verbatim vs changed
The app is almost entirely firmware-agnostic (ViewPort + canvas + logic): the
splash/tour/home/remote/manual/quiz/settings/macros UI, `char_to_hid()`, the
single/double-press button mapping, macro loading from SD, settings load/save,
and the HID key senders all port **verbatim**. The BLE code is behind
`#ifdef HID_TRANSPORT_BLE` and is not compiled (the manifest sets
`cdefines=["HID_TRANSPORT_USB"]`). Only these changed:

1. **USB transport** → the mandatory lease pattern. Upstream did
   `furi_hal_usb_get_config()` + `furi_hal_usb_unlock()` + `furi_hal_usb_set_config(&usb_hid)`
   at startup and `furi_hal_usb_set_config(prev)` at exit. Replaced with
   `cremote_usb_acquire()` = `furi_hal_usb_lease_acquire` →
   `furi_hal_usb_set_config_for_owner(&usb_hid)` → light-sleep veto, and
   `cremote_usb_release()` = `set_config_for_owner(NULL)` →
   `furi_hal_usb_hid_release_for_owner` → `furi_hal_usb_lease_release` → drop veto,
   retried 5×100 ms at exit. The lease is taken just before the main loop; a
   refused lease shows a "USB is busy" dialog and the app exits cleanly (no reset).
   Two `bool` fields (`usb_lease`, `sleep_inhibited`) were added to the state.
2. **Board gate** added in `claude_remote_usb_app` (`furi_hal_board_supports_ported_apps()`).
3. **Icons include dropped.** The source `#include <claude_remote_usb_icons.h>` but
   draws no icons (`canvas_draw_icon`/`I_*` count = 0), so the include was removed
   and `fap_icon_assets` dropped — the FAP needs no icon pipeline.

## Firmware exports added (for FAPs, in firmware_api.c)
`furi_hal_hid_consumer_key_press`, `furi_hal_hid_consumer_key_release` (consumer
control, for the dictation key 0x00CF), and the notification globals the app
references: `&sequence_reset_rgb`, `&message_blue_0`, `&message_green_0`,
`&message_display_backlight_enforce_auto`,
`&message_force_display_brightness_setting_1f`. All additive; the API table stays
hash-sorted and the firmware still links.

## Controls (USB build)
Single: Left=1, Up=2, Right=3, OK=Enter, Down=dictation (Win+H on Windows OS
setting, else consumer 0x00CF). Double-press: Left=Ctrl+U, Up=Page Up, Right=Up
Arrow. Macros load from `apps_data/claude_remote_usb/macros.txt` (defaults written
if missing); `{update}` expands per the OS setting. Offline manual (7 categories)
and a 24-card quiz are in the menus. Settings (`settings.cfg`): haptics, LED, OS
(mac/win/linux), tour, double-click speed.

## Board support
Supported boards only: T-Embed, Paradoxt, ThorChimera, Thormini. USB HID only
(the BLE variant is a separate upstream branch and is not built here).

## Encoder fixes (2026-11-15)
- **Scrambled keys on Home/Remote.** Those screens run in portrait (`ViewPortOrientationVertical`)
  and the ViewPort rotates arrow keys to match -- right for a D-pad that turns with the device,
  wrong for a wheel. On the encoder the wheel sent "3"/"1" and opened BLE-promo/Settings, and
  OK+turn sent "2"/dictation. `input_callback()` now undoes the rotation for encoder events
  (`cremote_unrotate_key`), so: turn back = Up (2 / Macros), turn forward = Down (dictation /
  Manual), OK+turn back = Left (1 / Settings), OK+turn forward = Right (3). The orientation is
  tracked by a wrapper around this file's `view_port_set_orientation()` calls because
  `view_port_get_orientation()` is not exported to FAPs (exporting it would make the app fail to
  load on older firmware).
- **Backspace on the encoder** (upstream: hold Left, which a wheel cannot do): a plain OK hold on
  the Remote screen sends Backspace; an OK Long within 600 ms of an OK+turn detent is ignored. The
  help overlay line reads `o  Backspace` on encoder boards.
- Not reachable on the encoder (two keys held at once): the Right+Down hotkey overlay and the
  Left+Down macro shortcut. Macros remain on Home (turn back). D-pad unchanged.
