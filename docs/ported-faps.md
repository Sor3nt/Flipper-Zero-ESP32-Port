# Ported FAPs (from the previous firmware)

Apps in `applications_user/`: `flipirfreq`, `i2c_tools_cli`, `pc_monitor_usb`, `mouse_click_recorder`, `fmtx`,
`subghz_raw_edit`, `wifi_marauder_companion` (appid `esp32_wifi_marauder`). Prebuilt only, no source in this tree:
`sdcard/apps/SubGhz/pocsag_pager.fap` and `spectrum_analyzer.fap` (stock Flipper apps, they use the Sub-GHz device API;
the firmware's built-in RF Spectrum is a different, separate feature and is untouched).

Supported boards: **LilyGo T-Embed CC1101, Paradoxt, ThorChimera, Thormini** (all ESP32-S3). The new FAPs refuse to start
elsewhere (`furi_hal_board_supports_ported_apps()`, opt-in per board header with `BOARD_PORTED_APPS_SUPPORTED`).
A FAP never contains a GPIO number or a `BOARD_*` macro: `buildFap.sh` bakes the T-Embed header into every FAP, so pins,
bus, UART and policy are asked from the firmware at run time.

## Where each app goes on the SD card
| FAP | SD path | needs |
|---|---|---|
| flipirfreq | `apps/Infrared/` | board with IR |
| i2c_tools_cli | `apps/GPIO/` | external I2C bus (all four boards) |
| pc_monitor_usb, mouse_click_recorder | `apps/USB/` | USB-OTG PHY |
| fmtx, subghz_raw_edit | `apps/SubGhz/` | fmtx: board CC1101; MP3 files in `apps_assets/fmtx/` (kept in `applications_user/fmtx/assets/`) |
| esp32_wifi_marauder | `apps/GPIO/esp32/` | UART header: T-Embed (external-UART profile) or Paradoxt |

## Firmware APIs the apps use (all exported in `firmware_api.c`)
- **Infrared**: `furi_hal_infrared_claim_tx / release_claim / async_tx_start / _start_plain / _stop / _abort /
  _set_error_callback / _reset_callbacks / get_last_tx_error / is_available`. The claim is atomic and belongs to one task.
  The output pin always follows Infrared > Settings.
- **USB**: `furi_hal_usb_lease_acquire / release`, `set_config_for_owner`, `composite_install_for_owner`,
  `furi_hal_usb_hid_release_for_owner` (HID alone leaves the TinyUSB stack and the PHY mux on OTG; this puts the PHY back
  on USB-Serial-JTAG), `cli_vcp_disable / enable`.
- **UART**: `furi_hal_usb_uart_bridge_get_pins` (UART number + pads, only where the pads may be taken),
  `furi_hal_uart_shared_acquire / release`, `furi_hal_shared_pins_*`.
- **I2C**: `furi_hal_i2c_*` (stock 8-bit API), `furi_hal_i2c_get_access / get_owner / external_get_pins / get_sniffer_pins`.
- **Board**: `furi_hal_board_supports_ported_apps`, `furi_hal_board_get_name`.

## UART header per board
| Board | UART / pads | rule |
|---|---|---|
| T-Embed | UART1, TX GPIO43, RX GPIO44 | the pads are the NRF24 CE/CSN lines: usable only in the external-UART profile (`CONFIG_T_EMBED_EXTERNAL_UART`), which declares the NRF24 absent; the pad state is saved and restored |
| Paradoxt | UART1, TX GPIO43, RX GPIO44 | dedicated pads, UART0 console is detached from them; parked as pulled-up inputs on release |
| ThorChimera, Thormini | none | the UART apps say "No UART header on this board" |

Every UART user takes: `furi_hal_uart_shared_acquire` -> `furi_hal_shared_pins_acquire` -> driver check
(UART1/UART2/own) -> `furi_hal_shared_pins_save` -> light-sleep veto -> driver, and gives back in this order: driver,
pad snapshot, UART lease, pin lease, veto. A release that fails keeps what is still owned and is retried.

## I2C addresses on the external bus
| Address | Device | Boards | Apps may |
|---|---|---|---|
| 0x24 | PN532 NFC | all four (shares the Qwiic/NFC bus) | nothing (exclusive) |
| 0x3C | SSD1306 OLED | Thormini | nothing (exclusive) |
| 0x42 | BW16 I2C bridge | T-Embed with `CONFIG_T_EMBED_BW16_I2C` | nothing (exclusive) |
| 0x55 | BQ27220 fuel gauge | where present | read only |
| 0x6B | BQ25896 charger | where present | read only (a write can enter ship mode) |
The table is in `components/furi_hal/furi_hal_i2c.c`, with `_Static_assert`s that no two drivers share an address.
Bus pins: T-Embed SDA8/SCL18, Paradoxt SDA47/SCL48, ThorChimera and Thormini SDA8/SCL9. The sniffer needs two free pads:
only Paradoxt has them (GPIO5, GPIO11); they are leased with `furi_hal_gpio_reserve`.
The legacy I2C driver (`driver/i2c.h`) is the only one the firmware uses: never mix it with `driver/i2c_master.h` (ESP-IDF
aborts at start-up when both are linked).

## Light sleep
Held by three layers: the loader's insomnia for the whole app, the driver's own PM lock while it transmits (RMT) / is installed
(USB), and an explicit `furi_hal_power_light_sleep_inhibit_enter/exit` pair around hardware sessions (UART: light sleep gates
the UART clock; USB: it gates the PHY clock and the host may not re-enumerate, see the ESP-IDF USB-Serial-JTAG guide).

## Known limits / not verified on hardware
- Nothing here was run on a device. Everything was compiled; the FAPs were linked against the firmware API table
  (`tools/check_fap_symbols.py`, all symbols resolve).
- IR: carrier and pulse trains are sent in packets of about one second; the output has a tiny gap between two packets.
- Marauder: the ESP32 module needs its own 3V3 (the OTG switch of this firmware is only a flag).
- Building FAPs on Windows needs Pillow for the icons (`pip install pillow`) and a toolchain on PATH; `buildFap.sh` now
  normalises the backslash paths its Python helper prints.
