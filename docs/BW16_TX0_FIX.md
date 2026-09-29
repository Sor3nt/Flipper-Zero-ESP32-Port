# BW16 R4TKN v4 TX0 fix

This is a BW16 firmware update for the existing T-Embed BW16 R4TKN FAP.
Keep your current T-Embed firmware and FAP. No SD-card copy is needed.

## Flash

1. Disconnect all four BW16 wires from the T-Embed. Connect the BW16 USB to the PC.
2. Hold BURN, press and release RST, then release BURN.
3. Run this in PowerShell, substituting the BW16 USB port for COM4:

```powershell
powershell -ExecutionPolicy Bypass -File "D:\Flipper-Zero-ESP32-Port\build_t_embed\BW16-R4TKN-v4-TX0-Fix\FLASH_BW16.ps1" -Port COM4
```

The bundled official AmebaD uploader starts at 115200, then transfers at 460800
baud by default. This changes only USB flashing speed; runtime T-Embed UART stays
at 115200. Override with `-BaudRate 230400` or `-BaudRate 921600` if needed. Wait for
`SUCCESS: BW16 boot images and application checksums verified.`
Press RST afterward. This command is for the BW16 USB port, not the T-Embed port.
The public web flasher's FLIPPER_ZERO v4 option installs the unmodified version;
use this local package to install the TX0 fix.

## Connect and test

Unplug BW16 USB, then connect these four wires:

| T-Embed | BW16 |
|---|---|
| 3.3V | 3V3 |
| GND | GND |
| TX / GPIO43 | RX1 / PB2 |
| RX / GPIO44 (yellow wire) | TX0 / PA7_LOG_TX0 |

Keep GPS disconnected while using BW16. Start the existing **BW16 R4TKN** app,
open its UART session and run Scan. After the board is ready, the app should show
network records and finish when SCAN_DONE arrives. Do not use the old TX1 wiring
shown in the original app's wiring help with this TX0 image.

## What changed

Only 10 bytes across three existing code locations changed in the exact original
R4TKN v4 application image. Serial1 commands still arrive through RX1; protocol
replies now use TX0. Arduino debug/echo output is suppressed, along with the
specific SDK 'WIFI is already running' print that was joining protocol records.
The command handlers, radio routines, scan/list format and features are unchanged.
The original app and T-Embed firmware are unchanged.

This is a binary patch of the shipped v4 image, not a rebuild of unavailable
R4TKN sketch source. The patch script rejects any other image by SHA256.
The vendor image headers and lengths are unchanged. Its own packaging sources
show no embedded application checksum; the official uploader checks transferred
flash data. It writes the original KM0/KM4 boot images at flash offsets 0 and
0x4000, and the application at 0x6000, erasing only the affected sectors. It does
not perform a full-chip erase or modify efuses.

## Validation and limits

- Actual ARM Thumb code executed in Unicorn: original scan/list protocol output
  equals patched output, now on TX0; all byte values route correctly.
- Debug output is suppressed; wifi_on's return value and mutex lock/unlock remain.
- Empty scans, 50 results, maximum SSID length, duplicate names and both bands pass.
- Image hashes, section headers, allowed changes and calling conventions pass.
- Captured emulated replies are replayed through the real FAP parser/controller.

No physical BW16 was flashed or tested by the build process. The hardware test
above is still required; the emulator does not simulate radio or electrical behavior.
`PATCH_MANIFEST.json` contains exact hashes and patch locations; `validation/`
contains the results and emulated UART captures (synthetic networks).

## Restore original v4

Repeat the BW16 USB/BURN/RST procedure, adding `-RestoreOriginal`:

```powershell
powershell -ExecutionPolicy Bypass -File "D:\Flipper-Zero-ESP32-Port\build_t_embed\BW16-R4TKN-v4-TX0-Fix\FLASH_BW16.ps1" -Port COM4 -RestoreOriginal
```

For original v4, move the yellow return wire back to TX1/PB1. Restoring original
v4 restores the original behavior; it does not fix the earlier TX1 reception issue.
`-CheckOnly` verifies package hashes without opening hardware (also supports
`-RestoreOriginal -CheckOnly`).

## Provenance

R4TKN v4 archive from [rusyln/flipper-zero-bw16-r4tkn](https://github.com/rusyln/flipper-zero-bw16-r4tkn),
commit `9325761a5b8ff5b827f4faa4bbdc4afd3ee91a69`.
Original application SHA256: `995e34da08942e6edd9eae9f22bc55e9b0ffd193ac195e5c8f3cbbc2b25a8e2a`.
Patched SHA256: `a232eb5c42c5b8707e272bb8faa235d2e98e466dccb5b3c42baaec823fba84cd`.
Official uploader and source from [Ameba-AIoT/ameba-arduino-d](https://github.com/Ameba-AIoT/ameba-arduino-d/tree/6626360b6f6af698bfed794ca9e396b14a44ca3c/Ameba_misc/Autoflash_patch).
Upstream notices are retained in `source/`. This private compatibility patch is
not an upstream R4TKN release.

## Upload retry after a partial transfer

The first hardware upload reached at least 648 of 672 application packets and
failed before checksum verification. The old tool reports the same generic
message for a non-ACK or timeout, so the exact response is unknown. That point
is approximately 60 seconds into data transfer at the previous forced 115200
setting. The wrapper now uses supported 460800-baud transfers, reducing the
wire time by four. A loader time limit is a hypothesis, not a confirmed cause.

After a failure, disconnect/reconnect BW16 USB, enter BURN/RST download mode again,
and rerun FLASH_BW16.ps1 with the BW16 port. The complete image is rewritten; it
does not resume an uncertain partial transfer. Success still requires all three
flash checksums to match. The firmware file itself is unchanged.
