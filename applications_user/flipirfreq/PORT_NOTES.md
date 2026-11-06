# FlipIRFreq - ESP32 port notes

Upstream: https://github.com/jsammarco/FlipIRFreq (commit c1e72d3, MIT). Pure FAP (`apps/Infrared/flipirfreq.fap`).

## Output pin: follows Infrared > Settings
The app contains no GPIO number and no `BOARD_*` macro, so one binary works on every board. The infrared HAL
chooses the pad for every transmission from the user's Infrared setting (internal LED, or the external pin where the
board has one: Paradoxt GPIO4, T-Embed GPIO43) and does the pin leasing itself:

- Paradoxt: `furi_hal_gpio_reserve()` on the pad for the duration of the send; refused if another feature owns it.
- T-Embed (GPIO43 is the NRF24 CE / UART1 TX pad): the shared-pins lease, the NRF24 power-down and the pad
  save/restore of the HAL, and a refusal while a GPS / BW16 module listens on it.

The upstream "IR Output: Auto / Internal / External" row is now read-only (`INT LED` / `EXT IO4`) and shows what the
HAL will use. Change it in Infrared > Settings. A transmission that the HAL refuses ("PIN BUSY", "NO IR", "FAILED")
ends at once with that message instead of showing LIVE.

## Both signal types go through the HAL's RMT transmitter
- Carrier: `furi_hal_infrared_async_tx_start(freq, duty)`.
- Pulse (10.0 - 500.0 Hz): `furi_hal_infrared_async_tx_start_plain()` (no carrier; a mark drives the pin high for exactly
  its duration). The old GPIO + software-timer version had 1 ms tick resolution and drove the pin directly, bypassing the
  pad lease (on T-Embed that means pulsing the NRF24 CE line).
- The signal is sent in packets of about one second (one RMT transaction each, a tiny gap between two packets).
  Pulse packets always end after a space.

## Race-free start and stop
- `furi_hal_infrared_claim_tx()` is atomic: nobody (CLI, JS, an IR app) can start between the check and our start.
  `furi_hal_infrared_async_tx_start*()` returns false only if the peripheral is busy; then nothing was started and
  nothing is waited for.
- STOP uses `furi_hal_infrared_async_tx_abort()`: the signal is cut within about 20 ms instead of after the packet in
  flight. A finished burst uses `_stop()` (it only waits for the HAL task).
- A hardware error while sending (RMT failure) calls the error callback and shows `TX ERROR`.
- On exit the app clears its TX callbacks (`furi_hal_infrared_async_tx_reset_callbacks()`): they point into this FAP,
  and a later user that does not claim the peripheral must never get a call into freed memory.
- Late "finished" events of an earlier transmission can no longer stop the next one (generation counter).

## Light sleep
Three layers: the loader holds insomnia for the whole app, the RMT driver holds its own PM lock while it transmits, and
the app takes `furi_hal_power_light_sleep_inhibit_enter()` after a successful start and returns it in one place
(`flipirfreq_release_hal`) on every way out (stop, abort, error, refused start, exit).

## Firmware pieces this FAP needs
`furi_hal_infrared_{claim_tx,release_claim,async_tx_start,async_tx_start_plain,async_tx_stop,async_tx_abort,
async_tx_set_signal_sent_isr_callback,async_tx_set_error_callback,async_tx_reset_callbacks,is_available,
get_tx_output,get_ext_tx_pin,get_last_tx_error}`, `furi_hal_power_light_sleep_inhibit_{enter,exit}`,
`saved_struct_save`.

## Not verified on hardware
The packet-boundary gap (carrier / pulse train), the abort latency and the external pad behaviour on a scope.
