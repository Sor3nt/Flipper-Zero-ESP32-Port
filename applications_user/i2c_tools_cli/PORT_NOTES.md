# ESP32 port notes

Upstream: https://github.com/Tristus1er/flipperzero-i2c-tools-cli (GPL-3.0, commit `b53daec`,
fork of NaejEL/flipperzero-i2ctools). Pure FAP: not listed in `fam_config.py`, never built into
the firmware. Build with `buildFap.sh applications_user/i2c_tools_cli`, copy the result to
`/ext/apps/GPIO/` on the SD card.

## What differs from upstream

| Area | Upstream | This port |
|---|---|---|
| I2C HAL | `furi_hal_i2c_*` | The firmware has none (`furi_hal_i2c.h` is a stub header), so `i2c_compat.c` implements the five calls the app uses on the ESP-IDF legacy driver. Upstream sources are unchanged. |
| Bus pins | C0 = SCL, C1 = SDA | Qwiic bus, **SDA GPIO47 / SCL GPIO48** (`I2C_NUM_0`), shared with the BQ25896 (0x6B), BQ27220 (0x55) and the PN532. All three show up in a scan. |
| Sniffer pins | C0 / C1 | **SDA GPIO4 / SCL GPIO5** (free header pins). Sniffing re-initialises its pads as GPIO inputs, which would detach the I2C peripheral and starve the fuel-gauge, charger and NFC traffic if done on the Qwiic pads. |
| Writes | any address | Writes to **0x6B and 0x55 are refused** (GUI sender and `i2c write`), a stray BQ25896 register write can enter ship mode. Reads are allowed. |
| Scan | 8-bit probe of every value | Address-only probe of 0x08..0x77 only (reserved ranges are skipped); timeout floored to 20 ms because the driver lock can be held by the power service. |
| Input | acts on `InputTypeRelease`, uses Long OK / Long Back | Acts on `InputTypeShort`; `i2c_input.h` also turns an OK/Back hold into `InputTypeLong` on boards with `BOARD_INPUT_SHORT_AFTER_RELEASE` (Paradoxt does not emit Long for OK/Back). |
| CLI output | `printf` / `putchar` | `pipe_send()` on the shell pipe. Nothing in this firmware binds libc stdout to the per-thread furi stdout callback (`furi_thread_stdout_write` is not even linked into the ELF), so `printf` text never reached the terminal. |
| CLI unload | none | `i2c_cli_unregister()` waits (max 5 s) for a command that is still running, because the shell runs commands on their own thread and does not wait for them; unloading the FAP under one would jump into freed memory. |
| CLI numbers | hex | unchanged: every number is hex (`i2c read 3c 0 10` reads 16 bytes). |

### Upstream bugs fixed on the way

* `SCLcallback` (GPIO ISR) indexed `data[]` / `ack[]` with an unchecked counter: a transaction over
  128 bytes, or a bus left "started" by noise, wrote past the frame and, on the last frame, past the
  whole 33 KB table (heap corruption). The view also read up to index 254. Both are bounded now
  (host-tested: the unpatched file panics at `index 128 out of bounds`).
* Sniffer pads used no pull, so with nothing wired the edge interrupts storm on noise; they use the
  internal pull-ups now. The record table wrap clears with one `memset` instead of a 32K-iteration loop in the ISR.
* A Sender request made while the device list was empty fired later on whatever device appeared next.
* The Sender could use a stale address after a rescan shortened the list; the scanner view showed
  stale `0x00` entries at the end of the list.
* Leaving the Sniffer view without having started a capture reset its pads anyway.

## Firmware exports this FAP needs (one reflash)

`furi_hal_gpio_add_int_callback`, `furi_hal_gpio_remove_int_callback`, `furi_hal_gpio_read`,
`I_ButtonDown_7x4`, `I_ButtonLeft_4x7`, `I_ButtonRight_4x7`, `I_ButtonUp_7x4`, `I_Voltage_16x16`
(the PNGs of the same name are skipped by `compile_icons.py` because the firmware already has them),
`i2c_cmd_link_create`, `i2c_cmd_link_delete`, `i2c_master_start`, `i2c_master_write_byte`,
`i2c_master_stop`, `i2c_master_cmd_begin`. All were added with `tools/add_symbol.py`.

## Known limits

* Not tested on hardware. Host tests (zig cc, fake bus / fake GPIO / fake shell) cover the compat layer,
  the sniffer ISR callbacks, the CLI and the input normalizer; the firmware itself builds with the new exports.
* Reading BQ25896 register 0x0C clears its latched fault bits, and a scan can collide with the power
  service polling the BQ27220 (it needs bus-free time between accesses). Either costs at most one missed sample.
* The sniffer decodes from per-edge GPIO callbacks (ISR dispatch latency not measured), so edges
  may be lost on a fast bus.
* Long OK / Long Back on paradoxt is detected from press duration (600 ms). If the GUI thread
  delivers the driver's click echo more than 30 ms late, that hold also registers one stray click.
