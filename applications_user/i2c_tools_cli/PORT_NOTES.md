# I2C Tools CLI - ESP32 port notes

Upstream: https://github.com/Tristus1er/flipperzero-i2c-tools-cli (GPL-3.0, commit b53daec, fork of NaejEL/flipperzero-i2ctools).
Pure FAP (`apps/GPIO/i2c_tools_cli.fap`).

## What the firmware provides now
The bus is `components/furi_hal/furi_hal_i2c.c`: the stock `furi_hal_i2c_*` API on the legacy ESP-IDF driver, the bus pins and port of
the board (T-Embed 8/18, Paradoxt 47/48, ThorChimera and Thormini 8/9), a bus lock, and the **address policy** (see
`docs/ported-faps.md`): power ICs read-only, NFC / BW16 / OLED not touchable, scan always allowed and labelled with the owner.
The FAP has no GPIO number, no board macro and installs/reconfigures no driver. `i2c_compat.c` is only a thin shim.

## Sniffer
Two free header pads chosen by the firmware (`furi_hal_i2c_get_sniffer_pins`; only Paradoxt has them: GPIO5 and GPIO11, never
the external IR pad GPIO4 the old version used). Both are leased with `furi_hal_gpio_reserve` and configured with the owner
variant of the init call, so the IR HAL and the sniffer can never use a pad at the same time; if a pad is leased or is an output,
the view says so instead of starting. Fixes of upstream bugs: `data[]/ack[]` writes are bounded (a long transaction ran
past the 128-byte frame), a full table stops the capture instead of wiping 33 KB inside the ISR, a capture starts empty.

## Sender / CLI
The sender and `i2c write/read` ask the policy first and say who owns an address ("belongs to the firmware (BQ25896 charger)").
`i2c write` rejects a bad or surplus byte instead of writing a truncated payload and printing "ok".
Not verified on hardware: a scan can collide with the power service polling the BQ chips (costs at most one missed sample).
