[← Back to index](README.md)

# paradoxt Input Quirks — Read Before Writing Any New View/FAP

paradoxt has a real 4-way D-pad, but this firmware's GUI still carries input
adaptations written for the LilyGo T-Embed's 1-axis rotary encoder, and the
board's own input driver has its own quirk on top:

- **Every click emits `Press`, `Release`, then a synthetic echoed
  `Press/Short/Release`.** Any view/app logic written as "act on Press, stop
  on Release" will fire twice per physical click.
- **OK and Back never emit `InputTypeLong`.** Code that waits for a
  long-press on those keys (menus, confirmation holds, etc.) will never see
  it unless you derive "held" from Press→Release timing yourself.
- For new FAPs/apps, don't hand-roll a fix per app. Use
  `components/input/input_canonical.h` (header-only) — it converts the
  paradoxt stream back into stock Flipper semantics (one `Short` per tap,
  `Long` for OK/Back held ≥600 ms, `Repeat` for held arrows). Several ported
  FAPs ship their own narrow variant of the same pattern (`servo_input.h`,
  `i2c_input.h`, `raw_edit_keys.h`) when they needed extra behavior (e.g.
  "open menu while OK is still held") — copy one of those if
  `input_canonical.h` doesn't fit, don't invent a fourth echo-filtering
  scheme.
- Stock Flipper GUI modules that only react to `Short`/`Long`/`Repeat`
  (Submenu, TextBox, VariableItemList, number/text input, etc.) are
  naturally immune to the echo and need no normalizer.
- `BOARD_INPUT_DPAD` in `board_paradoxt.h` gates the GUI-level encoder
  adaptations (arrow rotation/transposition in `view_dispatcher.c`,
  `view_holder.c`, `text_input.c`, `byte_input.c`, `variable_item_list.c`)
  off for a real D-pad. **Do not "fix" apparent D-pad direction bugs by
  swapping `BOARD_PIN_BTN_*` pins** — that was tried once, made things worse,
  and was reverted. The pins are correct; the bug (if one reappears) is
  almost always in one of those GUI adaptation layers, not the GPIO mapping.
- `view_port.c` screen-orientation rotation (Vertical/Horizontal) is
  **stock Flipper behavior**, unrelated to the encoder adaptations above —
  don't gate that behind `BOARD_INPUT_DPAD`.
