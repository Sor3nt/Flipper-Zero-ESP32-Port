# [BW16] R4TKN WiFi — companion FAP (ESP32-S3 port)

Companion app for the **RTL8720DN "BW16"** WiFi module running the **R4TKN**
firmware (https://github.com/rusyln/flipper-zero-bw16-r4tkn).

The upstream repo only ships a prebuilt ARM `.fap` (for the original Flipper) plus
the BW16 binary firmware — there is **no app source**. The ARM `.fap` cannot run on
this Xtensa ESP32-S3 port, so this app is a clean re-implementation of the same
serial protocol (recovered from the `.fap` / BW16 firmware symbols) built natively
for this firmware. It mirrors the proven `wifi_marauder_companion` plumbing.

## Wiring (115200 baud, 8N1)

| BW16 | Flipper header |
|------|----------------|
| 5V   | Pin 1 (5V)     |
| GND  | Pin 8 (GND)    |
| RX1  | Pin 13 (TX)    |
| TX1  | Pin 14 (RX)    |

On this board those TX/RX pads are the **shared expansion USART**
(`FuriHalSerialIdUsart`, GPIO43/44). The same channel is used by the WiFi Marauder
companion, the USB-UART bridge and ESP-Flasher, so only one of those may run at a
time. 5V is driven from the OTG rail: the app enables OTG on entry (only effective on
battery) and restores the previous state on exit, so it never leaves OTG on for light
sleep. `furi_hal_serial_init()` holds light sleep off while the UART is open and
releases it on exit.

## Menu → serial command

| Menu item        | Sent to BW16          | Mode        |
|------------------|-----------------------|-------------|
| Scan Networks    | `SCAN`                | one-shot    |
| Deauth All       | `DEAUTH_ALL`          | continuous  |
| Targeted Deauth  | `LIST_STATION` → pick | —           |
| → Deauth This AP | `DEAUTH_STATION <i>`  | continuous  |
| → List Clients   | `LIST_CLIENTS <i>`    | —           |
|   → pick client  | `DEAUTH_CLIENT <i> <j>` | continuous|
| Random SSID      | `RANDOM_SSID`         | continuous  |
| Beacon + Deauth  | `BEACON_DEAUTH`       | continuous  |
| NRF24 Jammer     | `NRF_START`           | continuous  |
| Deauth + NRF     | `DEAUTH_NRF_START`    | continuous  |
| Evil Portal      | `EVIL_PORTAL_START`   | continuous  |
| Deauth Sniff     | `DEAUTH_SNIFF_START`  | continuous  |

Responses parsed: `list-station: <ssid>,<ch>,<rssi>` / `LIST_STATION_DONE`,
`list-client: …` / `LIST_CLIENTS_DONE`. All other lines stream to the console view.

**Continuous (attack) modes:** the R4TKN firmware toggles these on a repeated command,
so pressing **Back** re-sends the command once to stop the attack before leaving. One
-shot modes (scan / list) need no stop. The exact feature set depends on the BW16
firmware version you flashed — older builds implement scan / deauth-all / targeted
only; the newer menu items are forwarded regardless.

## Build / install

No firmware reflash is required — every HAL symbol this app uses is already exported
in `components/flipper_application/flipper_application/firmware_api.c`. Build the FAP:

```
./buildFap.sh applications_user/bw16_r4tkn
```

The result is `build/fap/bw16_r4tkn.fap`; copy it to the SD card under `apps/GPIO/`
(the `fap_category="GPIO"` places it there automatically when the SD image is built).

## Status

Host-reviewed; the line parser was unit-tested on the host. **Not yet hardware-tested**
(no BW16 module available in this environment).
