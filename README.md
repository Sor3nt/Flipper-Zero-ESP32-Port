> WARNING: I do not take responsibility if you damage your board or property. This guide is for educational purposes only — proceed at your own risk.

# Flipper Zero ESP32 Port

A port of the [Flipper Zero](https://flipperzero.one/) firmware to ESP32-based development boards. This project brings the Flipper Zero UI, services, and application framework to affordable ESP32 hardware — no Flipper Zero required.

## Discord

Join the [Flipper Zero meets ESP32 - Discord](https://discord.gg/5DnAqFXaBC) for support and announcements.

## Supported Boards

![img](pic1.jpg)

| Board | MCU | Display | Input | SubGHz | NFC | IR | SD Card |
|---|---|---|---|---|---|---|---|
| **LilyGo T-Embed CC1101** | ESP32-S3 (Xtensa LX7) | ST7789 320×170 | Rotary encoder + button | CC1101 | PN532 (I2C) | RMT TX + RX | SPI |
| **Waveshare ESP32-C6-LCD-1.9** | ESP32-C6 (RISC-V) | ST7789V2 320×172 | CST816S touch | — | — | — | SPI |

![img](pic2.jpg)

## How to Flash

The easiest way is the **web flasher** — no toolchain required, just a Chrome/Edge browser and a USB cable:

**[Flash via Browser](https://sor3nt.github.io/interface.html)**

Connect your board, click flash, done. After flashing, copy the contents of [`sdcard/`](sdcard/) onto a FAT32 SD card and insert it — most apps need files there to function.

## Apps

### 📡 Wireless / RF

#### Sub-GHz
External CC1101 receiver/transmitter for 433–868 MHz signals.
- Receive & decode
- Read RAW: capture unknown waveforms to `.sub` files for later analysis
- Frequency analyzer with sweep & live RSSI
- Hopper: scan all preset bands during receive
- Transmit saved files; manual signal creation (frequency, modulation, protocol, key/serial/counter)
- Brute force / sub-brute attack with manufacturer dictionary
- Playlist for sequential transmit
- **Limitation:** AES-encrypted manufacturer keystores (`keeloq_mfcodes`, `nice_flor_s`, `alutech_at_4n`) are not decryptable on this port — only the plain-text `keeloq_mfcodes_user` works for Keeloq decoding.

#### Sub-GHz Remote
Multi-button remote layouts that batch saved `.sub` files. Map Up/Down/Left/Right/OK to individual transmit signals; switch between persistent remote profiles.

#### WiFi
Full WiFi pentest toolkit.
- **Scanner** — SSID, BSSID, channel, RSSI, auth mode
- **Connect** — auto-detect WPA/WPA2/WPA3, password input or saved password lookup (`/ext/wifi/<ssid>.txt`)
- **Deauther** — SSID-mode (single AP) or Channel-mode (all on channel)
- **Sniffer** — capture packets to PCAP
- **Handshake capture** — record EAPOL 4-way handshakes, optionally with deauth trigger
- **AirSnitch** — auto-bruteforce target with password list
- **Beacon Spam** — Funny SSIDs / Rickroll / Random / Custom
- **Network Scan / Port Scan** — host discovery + 19 common-port probe on the connected network
- **Web Crawler** — domain-based web crawler
- **Evil Portal** — captive portal with credential harvesting
  - Built-in templates: Google login, Router firmware update
  - Custom templates from `/ext/wifi/evil_portal/login_template/*.html` and `/ext/wifi/evil_portal/router_template/*.html` (filename = template name in dropdown)
  - Marker substitution: `%ERROR%`, `%SSID_OPTIONS%` (live AP scan)
  - Router-style verify flow: dropdown of real SSIDs, live WLAN re-auth check, captured-credentials screen on success, retry with error banner on fail
  - Pause/Resume of the AP from the run screen
  - Captured creds saved to `/ext/wifi/evil_portal/<ssid>_creds.csv`

#### Bluetooth
- **BLE Spam** — Apple Continuity (Pair/Action/NotYourDevice), Google FastPair (455+ models), Microsoft SwiftPair, Samsung Buds & Watch, Xiaomi QuickConnect
- **BLE Walk** — passive scanner with GATT service/characteristic inspection
- **BLE Clone** *(dev)* — replicate active BLE advertisements
- **FindMy** — emulate Apple AirTag, Samsung SmartTag, Tile beacons (clone or generate keypairs)
- **HID** *(see below)* — keyboard/mouse/media remote over BLE
- **Bad USB** — via USB or BLE

#### Infrared
RMT-based TX + RX.
- Learn signals (auto-decoded or raw)
- Browse, edit, and send saved remotes
- Universal remotes: TV, AC, audio, projectors, fans, LED controllers (databases on SD)
- Brute force category-based databases
- Configurable IR pin and 5 V GPIO power
- Protocols: NEC, NEC42, Samsung32, RC5/RC5X, RC6, SIRC 12/15/20, Kaseikyo, RCA, Pioneer

### 🪪 NFC

#### NFC *(PN532 over I2C)*
- Read, save, emulate, write NFC cards/tags
- Manual card generation (custom UID/ATQA/SAK)
- Mifare Classic dictionary attack (system + user dictionaries)
- Mifare Ultralight-C dictionary unlock
- ISO15693 SLIX unlock with manual or stored DEF key
- FeliCa system info, MIFARE DESFire app inspection, EMV transaction history
- 14 supported protocols: ISO14443-3A/3B/4A/4B, ISO15693-3, FeliCa, MIFARE Classic/Ultralight/Plus/DESFire, SLIX, ST25TB, NTAG4xx, Type-4
- 30+ supported card auto-parsers (Charlie Card, Clipper, EMV, Gallagher, HID, Opal, Skylanders, Troika, …)


#### TagTinker *(FAP)*
Infrared ESL (Electronic Shelf Label) research toolkit. Transmits custom images/text to graphics tags via IR. RLE streaming, Android companion app for image editing, monochrome + accent-color support.

### ⌨️ HID / USB

#### Bad USB
HID payload runner for Ducky-script (`.txt`) files from `/ext/badusb/`.
- 16+ Ducky commands (DELAY, STRING, REPEAT, HOLD/RELEASE, MEDIA keys, mouse, ALT-CHAR/ALT-STRING, SYSRQ)
- Layouts under `/ext/badusb/assets/layouts/*.kl` (~30 included)
- Configurable USB VID/PID + device name
- BLE bonding with custom MAC and PIN-verify pairing
- Mouse movement, scroll, button emulation; per-character typing delay
- **Transport:** USB OTG (TinyUSB) on T-Embed, BLE on Waveshare

### 🛠 System / Tools

#### Archive
SD-card file browser with tabs per media type: Favorites, Sub-GHz, NFC, LF-RFID, Infrared, iButton, Bad USB, U2F, Apps, Internal, Browser. Pin/unpin favorites; copy, paste, rename, delete, create folder.

#### JS Runner
mJS-based JavaScript runtime for user scripts in `/ext/apps/Scripts/*.js`.
- **Available modules:** `gui` (loading/menu/dialogs/text+byte input/popup/file picker/widget), `notification`, `math`, `storage`, `event_loop`, `subghz`, `infrared`, `badusb`, `blebeacon`
- **Excluded on this port** *(need HAL porting)*: `js_serial`, `js_gpio`, `js_i2c`, `js_spi`

### 🎮 Games

#### Doom
Full DOOM port. Place `doom1.wad` at `/ext/apps_data/doom/doom1.wad`. Encoder turns; click fires (short) / walks forward (long). Side-button uses doors/switches (short) / opens menu (long).

#### Snake
Classic snake game.

### ⚙ Settings & General
Bluetooth, backlight, clock, dolphin/passport, expansion port, input, notification, power, storage, system info, factory reset. Animated dolphin desktop on idle. File-pack manifest at `/ext/Manifest` (qFlipper-style asset list — its presence suppresses the "No DB" boot animation).

## SD Card Layout

| Path | Used by |
|---|---|
| `/ext/Manifest` | Desktop (presence check) |
| `/ext/dolphin/` + `manifest.txt` | Idle animations |
| `/ext/apps_assets/nfc/plugins/` | NFC protocol plugins (.fal) |
| `/ext/apps_data/nfc/plugins/` | NFC card-parser plugins (.fal) |
| `/ext/apps_data/js_app/plugins/` | JS module bindings (.fal) |
| `/ext/apps_data/doom/doom1.wad` | Doom |
| `/ext/badusb/` | Bad USB scripts + `assets/layouts/*.kl` |
| `/ext/infrared/assets/` | Universal remote DBs (`tv.ir`, `ac.ir`, `audio.ir`, `projectors.ir`, `fans.ir`, `leds.ir`) |
| `/ext/lfrfid/assets/iso3166.lfrfid` | LF-RFID country code lookup |
| `/ext/nfc/assets/` | MIFARE & EMV dictionaries |
| `/ext/subghz/assets/` | SubGHz keystores + `dangerous_settings` |
| `/ext/u2f/assets/` | U2F cert + key |
| `/ext/wifi/<ssid>.txt` | Saved WiFi passwords |
| `/ext/wifi/evil_portal/login_template/` | Custom captive-portal templates (no verify) |
| `/ext/wifi/evil_portal/router_template/` | Custom captive-portal templates (with WLAN verify) |

A complete starter kit is in [`sdcard.zip`](sdcard.zip) — extract it onto a FAT32 SD.

## Building

### Prerequisites

- **[ESP-IDF v5.4.1](https://docs.espressif.com/projects/esp-idf/en/v5.4.1/esp32s3/get-started/)** (exact version required)
- ESP-IDF export script sourced (default: `~/esp/esp-idf/export.sh`)

### Build & Flash (Linux / macOS)

```bash
# T-Embed (auto-detects /dev/cu.usbmodem*)
./buildAndFlash_T-Embed.sh

# Build only
./buildAndFlash_T-Embed.sh --build-only

# Waveshare ESP32-C6
./buildAndFlash_waveshare_c6_1.47.sh
./buildAndFlash_waveshare_c6_1.9.sh
```

### Build & Flash (Windows)

Use `winbuild.py` — a single CLI that wraps build, flash and serial-monitor steps for `cmd.exe` / PowerShell. Requires Python 3 and ESP-IDF v5.4.1 installed at `C:\Espressif\frameworks\esp-idf-v5.4.1` (or override via `ESP_IDF_DIR`).

```bat
:: One-time: install ESP-IDF python env
python winbuild.py setup

:: Verify the toolchain activates
python winbuild.py check

:: Build T-Embed CC1101 (default board)
python winbuild.py build

:: Build Waveshare ESP32-C6
python winbuild.py build --board waveshare_c6

:: Flash (port defaults to %ESPPORT% or COM14)
python winbuild.py flash --port COM14

:: Stream serial output for N seconds
python winbuild.py monitor --duration 30

:: Build + flash + monitor in one go
python winbuild.py all --port COM14
```

Boards: `t_embed` (default), `esp32s3`, `waveshare_c6`. Override defaults with `ESP_IDF_DIR` and `ESPPORT` env vars. `monitor --reset` only works on USB-UART bridges, not on the ESP32-S3 native USB-Serial/JTAG — use `flash` or `all` to capture boot logs.

### Build a FAP

```bash
# Firmware must be built first (Linux/macOS)
./buildFap.sh applications/main/my_app
```

## Porting Approach

This port preserves the original Flipper Zero architecture as closely as possible:

- **Furi OS** runs on FreeRTOS with the same thread/mutex/event/record API
- **Services** (GUI, Input, Storage, Loader, Desktop, BT) use the same message-queue and record-system patterns
- **HAL** maps STM32 peripherals to ESP-IDF drivers (SPI → `esp_lcd`, I2C → CST816S/PN532, RMT → IR, Bluedroid → BLE, TinyUSB → USB-HID)
- **Display** renders the original 128×64 mono framebuffer, then 2× upscales to RGB565 for the color LCD
- **Applications** compile with minimal changes (`#include` path adjustments, no-op stubs for missing hardware like 1-Wire)
- **`malloc` is redefined to `calloc`** — STM32 heap starts zeroed, ESP32 does not
- **Crypto** is stubbed (no Flipper-Enclave key) — affects encrypted SubGHz keystores; everything else uses real mbedtls
