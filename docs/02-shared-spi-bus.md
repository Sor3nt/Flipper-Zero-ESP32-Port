[← Back to index](README.md)

# Shared SPI Bus (paradoxt): LCD + SD + CC1101 + NRF24 all on SPI2

paradoxt has no RF band switch, no AW9523 — CC1101, NRF24, the SD card and the
ST7789 LCD all sit on the same SPI bus behind one global recursive mutex. This
is by design (board economics), and most of the stack is already careful
about it. The failure modes when new code *isn't* careful:

- **Never hold the SPI bus lock across anything that can block for more than
  a few ms** — a `vTaskDelay`/`furi_delay_ms`, a modal dialog, a long script
  step. A real bug: NRF24 MouseJacker wrapped an *entire* Ducky script step
  (which can contain a multi-second `DELAY` or a long `STRING` typed at
  10 ms/char) in one `nrf24_hw_acquire()`/`release()` pair. Because that
  mutex is also the global LCD/SD lock, the **screen and input froze for the
  whole script step** — this looked exactly like a hard hang requiring a
  reboot. The fix pushed the acquire/release down into the actual
  register-write primitives only.
- **A blocking HAL probe/attach + a modal dialog inside a `VariableItemList`
  change callback will wedge the GUI.** The input-dispatch thread is not the
  place for a ~1 s probe/attach cycle or `dialog_message_show`. Defer to
  `on_event` via a custom event instead (the pattern already used elsewhere
  in the Sub-GHz scenes).
- When releasing the bus mid-operation to stay responsive, make sure whatever
  invariant the bus mutex was also protecting (e.g. "which device's CS is
  currently routed," a `current_handle`-style field) is still safe to
  read/write across the gap — it usually is here because that state is
  itself guarded by the same mutex, but verify this explicitly for any new
  lock-splitting, don't assume it.
- External CC1101/NRF24 modules (header-pin breakouts) reserve their header
  GPIOs only for the session they're attached, and cross-check against each
  other before claiming a pin, to avoid a lock-ordering deadlock between the
  two HAL modules. Follow this pattern for any new external-radio feature —
  don't manage header pins with a bare `gpio_config` call.
