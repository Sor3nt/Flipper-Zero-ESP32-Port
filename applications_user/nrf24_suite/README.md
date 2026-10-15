# nrf24tool

![License](https://img.shields.io/badge/license-MIT-green) ![Status](https://img.shields.io/badge/status-active-brightgreen)

A complete tool for the **Flipper Zero** device, designed to interface with the NRF24L01+ transceiver. It offers functionalities such as transmitting, receiving, packet sniffing, and an experimental "mouse jacker" mode for controlling Unifying receivers.

## Table of Contents
- [Overview](#overview)
- [Features](#features)
- [Installation](#installation)
- [Usage](#usage)
- [Configuration](#configuration)
- [Contributing](#contributing)
- [License](#license)
- [Authors](#authors)

## Overview
**nrf24tool** is a dedicated tool for the Flipper Zero, providing various operational modes to work with NRF24L01+ modules. It supports custom packet transmission, data reception, passive sniffing, and a "mouse jacker" mode for sending keyboard commands through Unifying receivers.

This project utilizes an enhanced version of the **libnrf24** library, which has been improved and expanded to support additional NRF24L01+ functionalities, optimized specifically for the Flipper Zero.

### Credits
- **Sniffer and Mouse Jacker**: Code for these tools and the `libnrf24` library is based on [mothball187's flipperzero-nrf24 repository](https://github.com/mothball187/flipperzero-nrf24). Special thanks to the author for his foundational work !
- **Mouse Jacker**: Also incorporates code from the [Flipper Zero BadUSB app](https://github.com/flipperdevices/flipperzero-firmware/tree/dev/applications/main/bad_usb)

## Features
- **Transmit (TX)**: Send custom data packets and log events to a file. It can also use a text file as input to send multiple packets.
- **Receive (RX)**: Capture incoming data packets with optional event logging.
- **Sniffing Mode**: Monitor network traffic passively and automatically detect Unifying receiver addresses, saving them to a file for use with the Mouse Jacker tool.
- **Mouse Jacker Mode**: Control Unifying-compatible devices to send keyboard commands remotely, featuring the same functionalities as the **BadUsb** tool it’s inspired by, including full keyboard layout support.

### Additional TX and RX Mode Capabilities

- **TX Mode Data Format**: The TX tool can send data in hexadecimal format, ASCII format, or directly from a file, providing flexibility in how data packets are transmitted.
- **RX Mode Display Options**: The RX tool allows for real-time viewing of incoming data in either hexadecimal (binary) or ASCII format. The display format can be toggled by pressing the "OK" button on the Flipper Zero device.

## Installation

> **⚠ WARNING:** This program requires Flipper Zero firmware version **1.1.2** or higher. It will not function correctly on earlier versions.

1. **Clone the repository**:
   ```bash
   git clone https://github.com/OuinOuin74/nrf24tool.git
   cd nrf24tool
   ```

2. **Compile with UFBT for Flipper Zero**:
   Use the **ufbt** tool to compile the project for the Flipper Zero. For more details on installation and usage, visit the [Flipper Zero ufbt repository](https://github.com/flipperdevices/flipperzero-ufbt).

   ```bash
   ufbt build
   ```

## Usage

This tool runs directly on the Flipper Zero, and all functionalities (TX, RX, Sniffing, Mouse Jacker) are accessible through the device’s interface. Configuration is available on-screen, allowing for mode selection, parameter adjustment, and enabling event logging. 

## Settings Overview

Each mode includes a set of configurable settings, allowing for customized operation. Below is a list of settings available for each mode:

### RX Mode Settings (`RxSettingIndex`)
- **RX_SETTING_CHANNEL**: Channel to listen on.
- **RX_SETTING_DATA_RATE**: Data rate for communication (e.g., 250kbps, 1Mbps, 2Mbps).
- **RX_SETTING_ADDR_WIDTH**: Address width for the receiver address.
- **RX_SETTING_CRC**: Cyclic Redundancy Check (CRC) mode for data integrity.
- **RX_SETTING_RPD**: Received Power Detector to assess signal strength.
- **RX_SETTING_ACK_PAY**: Enable or disable acknowledgement payloads.
- **RX_SETTING_P0_PAYLOAD**: Payload size for Pipe 0.
- **RX_SETTING_P0_AUTO_ACK**: Enable or disable auto-acknowledgment for Pipe 0.
- **RX_SETTING_P0_ADDR**: Address for Pipe 0.
- **RX_SETTING_P1_PAYLOAD**: Payload size for Pipe 1.
- **RX_SETTING_P1_AUTO_ACK**: Enable or disable auto-acknowledgment for Pipe 1.
- **RX_SETTING_P1_ADDR**: Address for Pipe 1.
- **RX_SETTING_P2_PAYLOAD**: Payload size for Pipe 2.
- **RX_SETTING_P2_AUTO_ACK**: Enable or disable auto-acknowledgment for Pipe 2.
- **RX_SETTING_P2_ADDR**: Address for Pipe 2.
- **RX_SETTING_P3_PAYLOAD**: Payload size for Pipe 3.
- **RX_SETTING_P3_AUTO_ACK**: Enable or disable auto-acknowledgment for Pipe 3.
- **RX_SETTING_P3_ADDR**: Address for Pipe 3.
- **RX_SETTING_P4_PAYLOAD**: Payload size for Pipe 4.
- **RX_SETTING_P4_AUTO_ACK**: Enable or disable auto-acknowledgment for Pipe 4.
- **RX_SETTING_P4_ADDR**: Address for Pipe 4.
- **RX_SETTING_P5_PAYLOAD**: Payload size for Pipe 5.
- **RX_SETTING_P5_AUTO_ACK**: Enable or disable auto-acknowledgment for Pipe 5.
- **RX_SETTING_P5_ADDR**: Address for Pipe 5.
- **RX_SETTING_LOGGING**: Enable or disable event logging.

### TX Mode Settings (`TxSettingIndex`)
- **TX_SETTING_CHANNEL**: Transmission channel.
- **TX_SETTING_DATA_RATE**: Data rate for transmission (e.g., 250kbps, 1Mbps, 2Mbps).
- **TX_SETTING_ADDR_WIDTH**: Address width for transmission.
- **TX_SETTING_TX_ADDR**: Transmitter address.
- **TX_SETTING_PAYLOAD_SIZE**: Size of the payload to be sent.
- **TX_SETTING_FROM_FILE**: Enable or disable loading packets from a file.
- **TX_SETTING_AUTO_ACK**: Enable or disable auto-acknowledgment.
- **TX_SETTING_SEND_COUNT**: Number of packets to send.
- **TX_SETTING_TX_INTERVAL**: Interval between packet transmissions.
- **TX_SETTING_ACK_PAY**: Enable or disable acknowledgment payloads.
- **TX_SETTING_CRC**: Cyclic Redundancy Check (CRC) for data integrity.
- **TX_SETTING_TX_POWER**: Transmission power level.
- **TX_SETTING_ARC**: Automatic retransmission count.
- **TX_SETTING_ARD**: Automatic retransmission delay.
- **TX_SETTING_LOGGING**: Enable or disable logging of transmitted events.

### Sniffer Mode Settings (`SniffSettingIndex`)
- **SNIFF_SETTING_MIN_CHANNEL**: Minimum channel to scan.
- **SNIFF_SETTING_MAX_CHANNEL**: Maximum channel to scan.
- **SNIFF_SETTING_SCAN_TIME**: Time spent scanning each channel.
- **SNIFF_SETTING_DATA_RATE**: Data rate for sniffing mode.
- **SNIFF_SETTING_RPD**: Received Power Detector to assess signal strength.

### Mouse Jacker Mode Settings (`BadmouseSettingIndex`)
- **BADMOUSE_SETTING_ADDR_INDEX**: Index for the address of target Unifying receiver.
- **BADMOUSE_SETTING_KB_LAYOUT**: Keyboard layout setting for compatibility.
- **BADMOUSE_SETTING_DATA_RATE**: Data rate for communication with the Unifying receiver.
- **BADMOUSE_SETTING_TX_POWER**: Transmission power level.
- **BADMOUSE_SETTING_TX_RETRY**: Number of retries for packet transmission.
- **BADMOUSE_SETTING_KEY_DELAY**: Delay between key presses when sending commands.

## Configuration
Modify `settings.conf` directly or adjust configurations via the Flipper Zero interface.

## Contributing

Contributions are welcome! To contribute:
1. Fork this repository.
2. Create a new branch for your feature.
3. Commit and push your changes, then open a pull request.

## License
This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## Authors
- **OuinOuin74** - *Creator and main developer* - [GitHub Profile](https://github.com/OuinOuin74)
