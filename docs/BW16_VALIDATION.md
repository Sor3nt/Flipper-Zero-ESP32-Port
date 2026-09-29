# BW16 controller validation

Target: the owner's T-Embed CC1101, ESP32-S3, 16 MB flash, ESP-IDF 5.4.1.
FAP version 4.0 controls a separate BW16-Kit running R4TKN FLIPPER_ZERO v4.

## Source scope

- Fixed UART2 transport, TX43/RX44, 115200 8N1; 8192-byte RX buffer.
- Eight main-menu entries expose all v4 operations and diagnostics.
  STOP is available on every active operation through short/long Back or OK.
- Firmware capability export checks the actual running UART profile.
- Personal early GPIO profile uses input pull-ups; console uses USB Serial/JTAG.
- GPS/config/NMEA, all Wardriver source, main startup, shutdown and RF code unchanged.
- No NRF mode/guard/SPI implementation added.
- Previous release bundle preserved; no physical device flashed.

## Checks completed

Windows native host checks compile the actual production C implementation
against mocked hardware/OS boundaries using Zig. These are not hardware tests.

- Protocol framing, valid 2.4/5 GHz records, commas/hidden SSIDs, malformed
  numeric/MAC data, overlong/NUL-corrupted lines, noise and timer wrap.
- All v4 wire commands, preserved AP/client index order, complete-list gating,
  duplicate/partial responses, bounded lists and invalidated stale indices.
- START/STOP acknowledgements, TX failure, malformed counters, overflow,
  20-second scan/list deadlines, 30-second operation limit and STOP timeout.
- Fixed UART route with no per-command flush/reroute; incompatible firmware
  rejection; existing UART1/2 owner left untouched; setup/teardown fault injection.
- 100 BW16/GPS swaps through both real production UART backends, including
  busy rejection, original GPS baud/pins and balanced driver/power-lock cleanup.
- Actual app entry/exit and callbacks: 20 repeated sessions, allocation/setup
  failures, short/repeated/held keys, every menu option and operation route,
  long Back requests STOP, overflow handling and uncertain-stop exit.
- Startup NUL/echo replay, quiet interval, incomplete reply, fragmented scan,
  zero-byte vs incomplete-response diagnostics and true millisecond deadlines.
- 24 screens rendered using native fonts, width assertions and visual review.
- Production early pin initialization and firmware capability tested with
  external-UART profile on/off, USB console on/off, and another board.
- Full Wardriver host suite: radio/STA/Bluetooth restoration, GPS, startup,
  413 parser checks and 50,000 malformed-input iterations, config/OUI,
  GPS-only CSV logging, combined HTTPS uploads and UI/credential handling.
- RF suite: 10,000 waterfall ring wraps, 300 actual worker sweeps and
  200 compositor frames with framebuffer guards.
- Shutdown guard: three preserved startup/shutdown files match 8c04a47.
- Custom-file protection: 93 custom files present; GPS/config/NMEA match baseline.

## Build and package

- Firmware and both FAPs built successfully with ESP-IDF 5.4.1 / Xtensa GCC 14.2.0.
- Firmware partition fit: 43% free in the existing 0x5f0000 application partition.
- Firmware and bootloader ESP image checksums and validation hashes are valid.
- Actual linked ELF: 1052 API entries sorted, unique, correct hashes and addresses.
- All 48 BW16 and 167 Wardriver imports resolve; FAP metadata and relocations match the loader.
- Wardriver FAP is byte-for-byte identical to the previous working release.
- Previous RF/WDGWars recovery firmware retains SHA256 3c0de286d47675efa289e97e4f1a238f449861be8a6caa059165a2c4d297a8be.
- Final source diff is whitespace-clean; GPS, Wardriver, RF, shutdown and partition sources are unchanged.

| Artifact | Bytes | SHA256 |
| --- | ---: | --- |
| furi_esp32.bin | 3539184 | 1257f83eee557c30c2c749b404dfb14fecb21a77bc75812a88715c1d43ed83f7 |
| bootloader/bootloader.bin | 20704 | 1d7098389a31d792f2f6b1a44686f980da482cc8c8bb6f1d3aab8650f2e4f0c6 |
| fap/bw16_r4tkn.fap | 47476 | d1e8b1fc86f8f9d35c34d061c9297d3172c5172a7f1a890d78ef8210ab25d88c |
| fap/wardriver.fap | 182148 | 1eb3f80577b5e920d3e87fa361164a1c5ad237c050187ce0e0cba0c6344bc4d5 |

## Hardware acceptance

Still required on the physical units: real BW16 communication and scans,
each supported control on an authorized test network, STOP acknowledgement,
boot/reset with BW16 connected, switching back to GPS, power stability,
USB/battery shutdown/wake and prolonged sessions.

The protocol has no version query, transaction IDs or checksum. Reset BW16
before reopening after an abandoned/timed-out scan or protocol fault.
An unacknowledged STOP does not establish that the module stopped.
