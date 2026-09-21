# BW16 R4TKN

Companion app for an AI-Thinker **BW16 (RTL8720DN)** running **R4TKN** dual-band
deauther firmware, driven over UART. It's a port of the R4TKN Flipper Zero app
(@r4tkn) — the Flipper original speaks `furi_hal_serial`, which is a no-op
stub on this ESP32 port, so the serial layer here is reimplemented on the
ESP-IDF UART driver.

## Wiring (default: T-Embed CC1101)

| Board pin | BW16 |
|-----------|------|
| IO43 — UART TX | RX |
| IO44 — UART RX | TX |
| GND | GND |

115200 baud, 8N1, on `UART_NUM_1`. IO43/IO44 are the T-Embed's onboard
RDM6300/Grove serial pins (see `BOARD_PIN_RFID_TX`/`BOARD_PIN_RFID_RX` in
`board_lilygo_t_embed_cc1101.h`) — 125kHz RFID reading won't work while the
BW16 is wired there. These pins don't exist on the ESP32-C6 Waveshare
boards. Edit `BW16_TX_PIN` / `BW16_RX_PIN` in `bw16_r4tkn.c` to use
different pins or target another board.

## Features (Core v1)

- **Scan Networks** → list of APs, scroll with Up/Down.
- **Deauth All** — broadcast deauth.
- **Beacon Spam** — random-SSID beacon flood.
- **Targeted deauth** — OK on a scanned AP.
- **Stop** — OK/Back while running.

## Wire protocol (recovered from the R4TKN .fap)

TX (newline-terminated): `SCAN`, `DEAUTH_ALL`, `DEAUTH_STATION <idx>`,
`RANDOM_SSID`, `STOP`. RX: one line per network after `SCAN`, ended by
`SCAN_DONE`. (The full firmware also supports `DEAUTH_CLIENT <ap> <cl>`,
`BEACON_DEAUTH`, `EVIL_PORTAL_START`, `NRF_START`, `DEAUTH_SNIFF_START`,
`LIST_CLIENTS <idx>` — not wired into this v1 UI yet.)

## To confirm on real hardware

The BW16 firmware is closed-source, so two details were inferred and should be
checked with the BW16 connected (watch the serial log, `FURI_LOG` tag `BW16R4TKN`):

1. **Targeted-deauth index base** — `DEAUTH_STATION` is sent the 0-based list
   position. If the BW16 expects 1-based, adjust in `handle_key` (ScreenApList/OK).
2. **Scan-result line format** — each line between `SCAN` and `SCAN_DONE` is shown
   verbatim as an AP entry. If the firmware sends a structured line (index/SSID/
   channel/RSSI), parse it in `handle_line` for a nicer display.
