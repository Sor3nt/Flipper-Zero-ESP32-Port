# Better Mouse — ESP32 port notes

Upstream: https://github.com/Timon-sys/Bettermouse (GPL-3.0). A USB HID mouse
remote with a real motion engine: smooth acceleration ramp, true diagonals,
sub-pixel accumulators, scroll mode, drag lock and optional "ice" momentum.

## What is verbatim vs changed
- **`bm_settings.c/.h`** — verbatim. Settings via `saved_struct` (magic 0xB3, v3,
  with a v2 migration), range tables for the VariableItemList.
- **`bm_mouse.c` motion engine** — verbatim: the fixed-rate motion thread
  (absolute deadlines + measured dt), acceleration (`min_speed`→`max_speed` over
  `accel_ms`, quadratic ease), `0.707` diagonal scaling, float sub-pixel
  accumulators, scroll, drag lock, ice/momentum decay, and input taken off the
  input-service pubsub (`RECORD_INPUT_EVENTS`) so releases are never late. All
  HID goes through the motion thread: `furi_hal_hid_mouse_move/press/release/
  scroll`, `furi_hal_hid_is_connected`.
- **`bm_mouse.c` draw callback** — REWRITTEN with canvas primitives. The upstream
  draws with app-bundled icons (`bettermouse_icons.h` from `assets/`); to keep
  the FAP self-contained (no icon pipeline) the screen is redrawn with the same
  information (title, USB status, mode, DRAG, speed bar, a D-pad of direction
  arrows, L/R button boxes, the hold-Back-for-menu hint). Behaviour is identical.
- **`bettermouse.c` USB handling** — CHANGED to the mandatory lease pattern. The
  upstream does `furi_hal_usb_get_config()` + `furi_hal_usb_unlock()` +
  `furi_check(furi_hal_usb_set_config(&usb_hid, NULL))`. On this firmware that
  `unlock()` would rip USB from a running RPC/qFlipper session, and the
  `furi_check` would reset the device. Replaced with:
  `furi_hal_usb_lease_acquire` → `furi_hal_usb_set_config_for_owner(&usb_hid)` →
  light-sleep veto; released in reverse (`set_config_for_owner(NULL)` →
  `furi_hal_usb_hid_release_for_owner` → `furi_hal_usb_lease_release` → drop veto),
  retried 5×100 ms at exit. A refused lease shows a "USB is busy" dialog and exits
  cleanly instead of resetting. Board gate added
  (`furi_hal_board_supports_ported_apps()`).

No firmware change is needed for this app — it uses only the stock USB HID lease
API that `mouse_click_recorder` already relies on.

## Controls
Opens straight into the mouse. Arrows move; OK = left click; OK hold = drag lock
(press OK to drop); Back tap = right click; Back hold + OK toggles scroll mode;
hold Back ~0.8 s (with a "MENU..." warning at 1/4) drops to the menu, Back from
the menu leaves. Settings (VariableItemList) and a Controls help page are in the
menu; settings are saved on leaving the settings screen and on exit.

## Board support
Supported boards only: T-Embed, Paradoxt, ThorChimera, Thormini.

## Note on the GUI input service
The motion thread runs at `FuriThreadPriorityHigh` and reads keys straight off
the input pubsub, so a redraw never delays a release. Light sleep is inhibited
for the whole session (USB + the high-rate thread need the clock). The mouse
view's own input callback returns true only to stop the dispatcher treating Back
as "go back"; all key handling is via the pubsub.

## Encoder key map (2026-11-15)
The first port ran the upstream held-key engine on the encoder too, which did not work: a wheel
detent is an instant Press/Release, so the motion thread almost never saw it (cursor barely moved),
and OK-held+turn (the only Left/Right) starts with an OK Press and ends with an OK Release, so
upstream's click-on-Release fired a **spurious left click after every horizontal move** (and an OK
hold over 500 ms latched drag lock). `bm_mouse_input_event_encoder()` now handles the encoder:

| Action | Encoder |
|---|---|
| Move up/down | turn wheel (6 px/detent, x2 / x4 when spun fast) |
| Move left/right | OK held + turn |
| Left click / drop drag | OK click (only OK *Short* clicks) |
| Drag lock | OK hold without turning |
| Right click | side key click |
| Scroll mode on/off | hold side key, then click OK (upstream Back hold + OK) |
| Scroll | turn wheel in scroll mode (1 notch per detent unit) |
| Menu | hold side key (~1.3 s: the side key reports its press at 0.5 s) |

Impulses are running totals written only by the input thread and consumed by the motion thread
(no shared read-modify-write); they are added after the idle accumulator reset. invert_y /
invert_scroll apply. The Controls page shows the encoder text on encoder boards. D-pad unchanged.
