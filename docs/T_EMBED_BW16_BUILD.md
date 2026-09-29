# T-Embed BW16 R4TKN 4.0 build

Adds a BW16-Kit / FLIPPER_ZERO v4 controller to the current working custom
T-Embed firmware. Existing RF Waterfall, Spectrum Analyzer, Wardriver 1.5.2,
GPS-only logging and combined WiGLE/WDGWars upload remain present.

## Install

1. Extract the ZIP. In its folder run:
   `powershell -ExecutionPolicy Bypass -File .\FLASH.ps1 -Port COM4`
   Replace COM4 with the T-Embed port. This flashes only the T-Embed.
2. Copy `SD/apps/GPIO/bw16_r4tkn.fap` to `apps/GPIO/bw16_r4tkn.fap` on SD.
   The included `SD/apps/Tools/wardriver.fap` is rebuilt from unchanged
   Wardriver source and may replace the existing file.
3. Leave all SD settings, credentials, logs, assets and favorites in place.
4. Connect BW16 using the table below. Open Apps -> GPIO -> BW16 R4TKN.
   Start with BW16 idle/reset and press OK to connect, then Scan networks.

| T-Embed header | BW16-Kit |
| --- | --- |
| 3.3 V | 3V3 |
| GND | GND |
| TX / GPIO43 | PB2_RX1 |
| RX / GPIO44 | PB1_TX1 |

BW16 USB is disconnected while powered by the T-Embed. GPS and BW16 are swapped,
never connected together. Exit the current app before swapping modules.
The owner's stable 3.3 V supply and NRF-free header are the intended setup.

The FAP provides every UART operation implemented in FLIPPER_ZERO v4:
scan, indexed station list, client observation, station/client/all-scanned-AP
controls, and STOP. It also includes UART diagnostics and wiring information.
See BW16-APP.md for the complete command map, operation confirmations and
recovery procedures. Features advertised as upcoming in the upstream project
cannot be enabled through this v4 UART interface.

## Firmware scope

GPIO43/44 use the dedicated external-UART profile and the firmware console uses
USB Serial/JTAG. The controller uses UART2 at 115200, 8N1, and releases it on
exit. GPS continues to use its saved UART/pins/baud without configuration edits.
There are no NRF checks, mode menus or guarded SPI transmissions in the app.

The existing shutdown implementation, main startup code, radio applications,
Wardriver/config/NMEA sources and partition layout are preserved. The narrowly
scoped early pin initialization change prevents NRF output states on the UART
header. No eFuses are written and no BW16 image is flashed.

Install the firmware together with the new FAP: the FAP requires the new
external-UART capability export. Flash regions remain 0x0 (bootloader), 0x8000
(partitions), 0x10000 (application), 0xbf0000 (initial OTA data). No erase-all.
The earlier RF/WDGWars release remains available as a separate recovery bundle.

## Rebuild and checks

The source checkout's sdkconfig enables CONFIG_T_EMBED_EXTERNAL_UART and selects
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG with no UART console. With the existing
Windows ESP-IDF 5.4.1 environment:

```powershell
python tools/build_bw16.py
$env:ZIG_CC = 'path/to/zig.exe'
python tests/bw16/run.py
python tests/host/run_wardriver_tests.py
python tests/host/test_shutdown.py
python tools/package_bw16.py
```

The build script builds firmware and both FAPs and validates their linked ABI.
It never flashes hardware. Set ESP_IDF_DIR if the IDF installation differs.
`--incremental` skips explicit reconfigure after the initial build.

BUILD_INFO.json and SHA256SUMS.txt identify the exact delivered artifacts,
source hashes and validation status. `FLASH.ps1 -Port COM4 -VerifyOnly`
checks checksums without opening a serial port.

Hardware acceptance remains required: actual BW16 reply/scan, each v4 operation
on an authorized test network, reliable STOP, unplug/reopen, boot with BW16
attached, swap back to GPS, USB/battery wake/shutdown and prolonged use.
The build process does not claim those physical tests or flash a device.
