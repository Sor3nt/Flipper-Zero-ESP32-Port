# Flipper Zero ESP32 Port

A port of the [Flipper Zero](https://flipperzero.one/) firmware to ESP32-based development boards. This project brings the Flipper Zero UI, services, and application framework to affordable ESP32 hardware — no Flipper Zero required.

## Discord
Join the [Flipper Zero meets ESP32 - Discord](https://discord.gg/5DnAqFXaBC) for support and announcements.

## Supported Boards
![img](pic1.jpg)

| Board                                   | MCU | Display | Input | SubGHz | NFC | IR | SD Card |
|-----------------------------------------|-----|---------|-------|--------|-----|-----|---------|
| **LilyGo T-Embed CC1101**               | ESP32-S3 (Xtensa LX7) | ST7789 320x170 | Rotary encoder + button | CC1101 | PN532 (I2C) | TX + RX | SPI |
| **Waveshare ESP32-C6-LCD-1.9** | ESP32-C6 (RISC-V) | ST7789V2 320x172 | CST816S touch | — | — | — | SPI |

![img](pic2.jpg)

## How to Flash

The easiest way to get started is the **web flasher** — no toolchain required, just a Chrome/Edge browser and a USB cable:

**[Flash via Browser](https://sor3nt.github.io/interface.html)**

Connect your board, click flash, done.

## Features

**SD Card** (highly recommended)
- Stores SubGHz captures, IR remotes, NFC dumps, Bad USB scripts, and settings
- Required for most apps to save/load data

**Sub-GHz** (T-Embed CC1101)
- Read, save, and transmit Sub-GHz signals
- Frequency analyzer
- Includes [ProtoPirate](applications_user/protopirate/) for protocol analysis

**Bluetooth**
- Remote control via [Flipper Mobile App](https://docs.flipper.net/mobile-app) (screen streaming, file manager, RPC)
- BLE Spam (8 attack types: FastPair, SwiftPair, EasySetup, etc.)
- BLE Walk (GATT service explorer)
- BLE Clone (dev)
- Bad USB over BLE HID (keyboard/mouse/consumer)

**WiFi**
- Connect to WiFi networks (password prompt for protected networks)
- Saved passwords on SD card (`/wifi/<SSID>.txt`) — entered passwords are saved automatically after successful connect
- Network scanner
- Deauther
- Handshake capture
- Packet sniffer
- Port scanner

**NFC** (T-Embed with PN532, needs testing)
- 15 protocols ported (NTAG, Mifare Classic/Ultralight, Felica, ISO14443, etc.)

**LF-RFID** (RDM6300)
- Read and emulate low-frequency RFID tags

**Infrared** (T-Embed)
- TX and RX via RMT peripheral
- 8 protocols: NEC, RC5, RC6, Samsung, SIRC, RCA, Pioneer, Kaseikyo
- Built-in remote database (TV, AC, audio, fans, projectors, LEDs)

**ESP-NOW**
- Peer-to-peer communication between ESP32 devices

**GPIO**
- GPIO control and USB-UART bridge

**General**
- Clock app
- Animated Dolphin desktop
- Archive (file browser)
- Settings (BT, backlight, etc.)

## Building

### Prerequisites

- **[ESP-IDF v5.4.1](https://docs.espressif.com/projects/esp-idf/en/v5.4.1/esp32s3/get-started/)** (exact version required)
- ESP-IDF export script sourced (default: `~/esp/esp-idf/export.sh`)

> **Important:** This project must be built with ESP-IDF **v5.4.1**. Newer versions (v5.5+) ship with a different toolchain (e.g. `xtensa-esp-elf 14.2.0`) that introduces linker errors. If you see `undefined reference` errors during linking, you are likely using the wrong ESP-IDF version.

### Build & Flash (T-Embed CC1101)

```bash
# Build and flash (auto-detects /dev/cu.usbmodem*)
./build.sh

# Build only
./build.sh --build-only

# Build, flash, and open serial monitor
./build.sh --monitor

# Specify serial port
./build.sh --port /dev/cu.usbmodem101
```

## Porting Approach

This port preserves the original Flipper Zero architecture as closely as possible:

- **Furi OS** runs on FreeRTOS with the same thread/mutex/event API
- **Services** (GUI, Input, Storage, Loader, Desktop, BT) use the same message-queue and record-system patterns
- **HAL** maps STM32 peripherals to ESP-IDF drivers (SPI→`esp_lcd`, I2C→CST816S/PN532, RMT→IR, Bluedroid→BLE)
- **Display** renders the original 128x64 mono framebuffer, then scales 2x to RGB565 for the color LCD
- **Applications** compile with minimal changes (`#include` path adjustments, no-op stubs for missing hardware)
- **`malloc` is redefined to `calloc`** — the STM32 heap starts zeroed, ESP32 does not
