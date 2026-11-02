# Signal Generator (ESP32 port, paradoxt)

Upstream: https://github.com/flipperdevices/flipperzero-good-faps/tree/dev/signal_generator
(commit `55328b4`, GPL-3.0, see `LICENSE`). Built as an external FAP, not part of the firmware.
Install to the SD card as `/ext/apps/GPIO/signal_generator.fap`.

## What differs from upstream

Upstream drives STM32 timers (TIM1/LPTIM2) and the MCO clock output. None of that exists on the
ESP32, so both modes run on the ESP32 LEDC peripheral through `furi_hal_pwm` (new in this port,
`components/furi_hal/furi_hal_pwm.c`):

- **Pins**: the board's external header, `BOARD_PIN_EXT_GPIO_1..5` = GPIO4, 5, 11, 12, 42 on
  paradoxt. Never `gpio_ext_pa4/pa7`: on this port those are the CC1101 SPI pins.
  `signal_gen_pins.h` reads the pin numbers from the board header, so the build fails on boards
  without the header pins.
- **PWM Generator**: 5 Hz .. 1 MHz, duty 0..100 %. Below 5 Hz the 80 MHz APB clock cannot be
  divided far enough. Frequency error is under 0.2 % (fractional LEDC divider).
- **Clock Generator**: 50 % square wave, presets 32.768 kHz .. 40 MHz divided by 1/2/4/8/16. All
  presets are exact except 32.768 kHz (about 0.06 % off); upstream used the LSE crystal for that one.
- **busy**: shown instead of the pin name when the pin is held by someone else (e.g. Infrared
  Settings > External uses GPIO4 while transmitting) or is driven as an output by another user.
- Changing only the duty keeps the output running without a glitch; changing the frequency restarts
  the timer (LEDC resets its counter on every reconfiguration).
- At most two pins run at once (LEDC timers 2/3; timer 0 is the backlight, 1 the buzzer). The app
  runs one at a time. Light sleep is held off while a pin is driven. A stopped pin returns to input
  with pull-down.

Needs a firmware that exports `furi_hal_pwm_*` and `I_SmallArrowUp/Down_3x5`.
The signal starts as soon as a mode is opened (same as upstream): check what is wired to the pin first.
