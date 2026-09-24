# BW16 R4TKN v3 controller

The BW16 R4TKN companion for the T-Embed CC1101 Plus requires firmware
built from the same revision. See [BW16-DEPLOYMENT.md](BW16-DEPLOYMENT.md)
for building both the controller and Wardriver, and installing the OUI data.

## Implemented R4TKN v4 commands

The target is **FLIPPER_ZERO v4** from
[rusyln/flipper-zero-bw16-r4tkn](https://github.com/rusyln/flipper-zero-bw16-r4tkn/tree/9325761a5b8ff5b827f4faa4bbdc4afd3ee91a69).
The standalone TFT firmware and newer commercial variants use unverified protocols.

| App operation | v4 wire command | Evidence / completion |
| --- | --- | --- |
| Scan networks | `SCAN` | Validated AP records followed by `SCAN_DONE` |
| Station list | `DEAUTH_STATION` without arguments | `LIST_STATION_START`, prefixed AP records, `LIST_STATION_DONE`; lists only |
| Observe clients | `LIST_CLIENTS <AP index>` | Five-second passive observation; `list-client:<12 hex digits>` and `LIST_CLIENTS_DONE` |
| Deauth station | `DEAUTH_STATION <AP index>` | `TARGETED_START`, `TX:<count>`, `TARGETED_DONE` after STOP |
| Deauth selected client | `DEAUTH_CLIENT <AP index> <client index>` | Same targeted start/counter/stop markers |
| Deauth all scanned APs | `DEAUTH_ALL` | `DEAUTH_ALL_START`, `CYCLES: <count>`, `DEAUTH_ALL_STOPPED` |
| Stop an active operation | `STOP` | The matching stopped/done marker must be received |

Commands are LF-terminated. Indices are zero-based and refer to the BW16's own
arrays. The app preserves record order, including duplicate records, and never
sorts or deduplicates those lists. Scan results are for viewing; selecting actions
first requests the explicitly framed station list. The app requires that completed
list before client lookup or deauthentication, and invalidates old client indices
after a new station/scan request. Overflowed lists remain visibly partial; deauth
all is unavailable when some APs could not be displayed.

Client observation identifies peers whose data-frame addresses match the selected
AP according to v4's callback. This is not proof of association, device ownership,
or identity. No handshake payload is captured or saved by this command.

**Beacon flooding, random-SSID spam, handshake capture and NRF jamming have no
verified command handlers in this v4 bridge.** The upstream README describes those
as planned/in-progress or separately available. Adding menu entries cannot provide
missing BW16 firmware functionality, so they are not implemented here.

## Protocol evidence

The inspected `RTL8720DN-BW16/v4.zip` contains `application.axf`, a map/disassembly
and `km0_km4_image2.bin`. The latter's SHA-256 is
`995e34da08942e6edd9eae9f22bc55e9b0ffd193ac195e5c8f3cbbc2b25a8e2a`.
No complete bridge source was available. Thumb disassembly identifies the UART
dispatch in `loop` at `0x0e0008a8`, station listing at `0x0e000230`, targeted AP
operation at `0x0e000528`, and all-AP operation at `0x0e000664`.
The client-list handler starts near `0x0e0009ec`, enables promiscuous observation
for 5000 ms, then prints client MACs and a completion marker. Its callback at
`0x0e0003e0` builds the index order used by the client command.
The targeted client handler near `0x0e000b40` parses two indices, emits
`TARGETED_START`, polls for `STOP`, reports `TX:` attempts and emits `TARGETED_DONE`.
Library symbols mentioning beacons or EAPOL alone are not exposed UART features.

This protocol provides no version handshake, checksums or transaction identifiers.
The app requires serial quiet between requests, rejects malformed records, and
requires a BW16 reset/reopen after protocol loss or timeout. Compatibility with
the actual module and its firmware image still needs hardware confirmation.

## Operation and NRF ownership

Wiring remains **BW16 TX1 to GPIO43, BW16 RX1 to GPIO44, GND to GND**, 3.3 V UART
logic at 115200 8N1. Power BW16 through its appropriate regulated input. Boot with
its TX/RX disconnected, open the app, hold OK to prepare, then connect when the
menu says to connect. NRF stays fitted and is verified powered down.

The [LILYGO pin map](https://github.com/Xinyuan-LilyGO/T-Embed-CC1101/blob/master/docs/pinmap_cn.md)
and [board definitions](components/furi_hal/boards/board_lilygo_t_embed_cc1101.h)
assign NRF CE=43, CSN=44, SCK=11, MOSI=9 and MISO=10; the
[Plus model](https://lilygo.cc/products/t-embed-cc1101-plus) includes the NRF.
This reverses PR #114's UART wiring: incoming BW16 replies now toggle CE instead
of selecting the NRF through CSN during display/SD/CC1101 traffic.

Per the [Nordic nRF24L01 specification](https://devzone.nordicsemi.com/cfs-file/__key/support-attachments/beef5d1b77644c448dabff31668f3a47-aad1a46f307945a7b0204fd969e86bdf/content.pdf),
PWR_UP=0 disables the radio independently of CE, while SPI remains active.
Preparation probes CONFIG/SETUP_AW and verifies writable CONFIG with PWR_UP=0;
an absent or unresponsive NRF fails setup. Both power-down and the controlled
CSN/SPI-lock arrangement are required. On exit, the saved GPIO state is restored
after unplug confirmation; NRF remains powered down until its app initializes it.
Host tests cover this sequence, but electrical behavior and NRF clones remain
unverified. Qwiic GPIO8/18 serve onboard I2C devices and are not assumed spare.

The new `furi_hal_bw16_guard_send` export routes every command, including STOP,
through the same firmware SPI mutex and IDF hardware bus lock. It bounds each
ASCII line to 48 bytes and rejects embedded control characters. Short writes and
TX timeouts disconnect the UART output and park NRF CSN HIGH before unlocking.
The old `furi_hal_bw16_guard_scan` export remains compatible for the scan-only FAP.

Deauthentication requires a separate confirmation screen and a long OK press.
The screen names the selected AP/client or all scanned APs. Each operation has a
30-second host limit, and OK/Back requests STOP. Repeated keys cannot start a new
operation while one is pending or active. START and STOP acknowledgements have
five-second deadlines. Device TX/cycle counters report attempts, not demonstrated
network disruption. The controller does not claim an outcome from command delivery.

RX loss, malformed counters or input overflow during an active operation request
STOP and invalidate the session. If STOP is not acknowledged or TX fails, the app
shows that the BW16 may still transmit and requires powering BW16 off, then
unplugging TX/RX before confirming exit. Physical power removal cannot be enforced
by this UART-only connection. The BW16 signal wires still must be disconnected
before T-Embed reset/power-off; software cannot isolate outputs during boot.

## Installation and validation

Build outputs in this worktree:

- Matching firmware: `build_t_embed/furi_esp32.bin` and generated flash files.
- New controller: `build_t_embed/fap/bw16_r4tkn.fap`, displayed as **BW16 R4TKN**.
- Copy the FAP to `apps/GPIO/bw16_r4tkn.fap` on SD.

This FAP needs the new guarded-send export, so replacing only the FAP on old
firmware is insufficient. Flash the matching firmware manually with the generated
`build_t_embed/flash_args`. Existing Wardriver and offline OUI data remain usable;
their source and existing exports are unchanged by the controls extension.

Keep BW16 TX/RX disconnected while flashing. Close any serial monitor, connect
the T-Embed by USB, then run PowerShell from `build_t_embed` in an activated
ESP-IDF environment. Replace `COMx` with the T-Embed's actual port shown in
Windows Device Manager:

```powershell
python -m esptool --chip esp32s3 --port COMx --baud 460800 --before default_reset --after hard_reset write_flash '@flash_args'
```

The activated ESP-IDF Python environment supplies esptool. If automatic connection fails,
hold BOOT, tap RESET, release BOOT, then retry with the resulting USB port. The
generated layout uses DIO, 80 MHz and 16 MB flash:

| Offset | File |
| --- | --- |
| `0x0` | `bootloader/bootloader.bin` |
| `0x8000` | `partition_table/partition-table.bin` |
| `0x10000` | `furi_esp32.bin` |
| `0xbf0000` | `ota_data_initial.bin` |

Do not flash the app binary at address zero. Copy the built FAPs and generated
OUI database to the SD-card root at these paths:

- `apps/GPIO/bw16_r4tkn.fap`
- `apps/Tools/wardriver.fap`
- `apps_data/wifi/mac-vendor.txt` (offline OUI database)

The BW16 module separately needs the matching **FLIPPER_ZERO v4** firmware;
the T-Embed build does not produce a BW16 image. The module's physical TX1
and RX1 pads must be identified from the actual BW16 board's labels/documentation.

Host checks exercise indexed lists, explicit start/stop acknowledgements, stale
list rejection, bounded counters, timer wraparound, UART loss, allocation failures,
real UI keys/callbacks, confirmed-unplug cleanup and native-font rendering.
Run `python tests/bw16/run.py` and `python tests/bw16/test_ui.py`; on Windows set
`ZIG_CC` and pass `--zig` to the first command. `tests/bw16/check_abi.py` checks the
FAP against the actual linked firmware. Hardware acceptance is outstanding.

All hardware smoke testing remains scan-only: prepare/connect, scan, list stations,
observe clients, then disconnect/exit and verify normal display/SD/NRF operation.
Deauthentication controls are exercised only against host mocks during development.
No device is flashed automatically.
