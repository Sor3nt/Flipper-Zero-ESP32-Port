# BW16 R4TKN controller v3

Standalone ESP32-S3 companion for the LILYGO T-Embed CC1101 Plus, targeting
R4TKN **FLIPPER_ZERO v4** on a BW16. The file remains `bw16_r4tkn.fap`; its app
name is **BW16 R4TKN**.

Implemented controls: network scanning, indexed station lists, observed clients,
station/client deauthentication, deauthentication of the complete scanned list,
and acknowledged STOP. Active operations require hold-to-confirm and have a
30-second host limit. Sending a command is never displayed as a verified result.

See [BW16-CONTROLS.md](../../BW16-CONTROLS.md) for command-by-command binary evidence,
build/install steps, protocol limitations and tests. Beacon/SSID flooding,
handshake capture, NRF jamming and other firmware variants are not supported by
this v4 UART controller. See [BW16-DEPLOYMENT.md](../../BW16-DEPLOYMENT.md)
for matching firmware and Wardriver installation.

| BW16 peripheral UART | T-Embed CC1101 Plus |
| --- | --- |
| TX1 | GPIO43 (UART RX / NRF CE) |
| RX1 | GPIO44 (UART TX / NRF CSN) |
| GND | GND |

Use 3.3 V logic and an appropriate regulated BW16 supply. Leave the built-in NRF
fitted. Boot with BW16 TX/RX disconnected, hold OK to prepare, then connect when
prompted. Every command uses both SPI locks while UART toggles CSN. The NRF is
verified powered down for the session; normal NRF and RFID use is exclusive.

Unplug BW16 TX/RX before confirming exit and before resetting or powering off
the T-Embed. If STOP is unconfirmed, power off BW16 before unplugging. The UART
connection cannot enforce physical isolation or power removal during a failure.

Version 3 needs the matching firmware's new `furi_hal_bw16_guard_send` export.
Do not install this FAP alone on the scan-only firmware. Wardriver/OUI support is
unchanged. No hardware validation or automatic flashing has been performed;
hardware smoke testing remains scanning/listing only.
