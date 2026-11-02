# FlipIRFreq - ESP32 port notes

Upstream: https://github.com/jsammarco/FlipIRFreq (commit c1e72d3, MIT). Pure FAP (`apps/Infrared/flipirfreq.fap`).

## What was changed for this firmware
- **Pins**: upstream used `gpio_infrared_tx` / `gpio_ext_pa7`. `gpio_ext_pa7` is the CC1101 MOSI line on this
  port. The IR LED (`BOARD_PIN_IR_TX`, GPIO2) and the external IR output (`BOARD_PIN_IR_TX_EXT`, GPIO4) now come
  from the board header the FAP is built for (same pins the infrared HAL uses for `Internal` / `ExtPA7`).
- **IR HAL state machine**: TX must start with `furi_hal_infrared_claim_tx()`; callbacks are set between claim and
  `async_tx_start()`, which returns false (then `release_claim`). Upstream cleared the callbacks again after the stop:
  here that trips a `furi_check` (the HAL already dropped them). Removed.
- **Burst length bug**: the HAL fires the "signal sent" callback after *every* packet (every `Done`). Upstream used
  `Done` for each 25 ms chunk, so the burst would have ended after ~25 ms and the carrier would have had a gap every
  25 ms (one RMT transaction per packet). Chunks are now grouped into ~1 s packets (`Ok`... `Done`) and the burst is
  reported finished only after `LastDone` (host-tested for every burst length 1..5000 ms).
- **Pulse mode**: claims the IR peripheral while it drives the pin, reserves the external pin
  (`furi_hal_gpio_reserve`) so no other feature takes it, holds a light-sleep inhibit (1 ms ticks would stretch), and
  restores the idle state the HAL expects (LED pin = output low, external pin = input + pull-down).
- A TX error callback ends the transmission ("TX ERROR") instead of leaving the screen on LIVE; late "finished"
  events of an earlier transmission can no longer stop the next one (generation counter).
- Buttons: acts on `Short`/`Repeat`, which this board's driver sends exactly once per click - no change needed.

## Firmware exports added
`furi_hal_infrared_{claim_tx,release_claim,is_busy,async_tx_stop,async_tx_set_signal_sent_isr_callback,async_tx_set_error_callback}`,
`furi_hal_gpio_{reserve,unreserve}`, `furi_hal_power_light_sleep_inhibit_{enter,exit}`, `saved_struct_save`.
