# BW16 R4TKN and Wardriver for T-Embed CC1101 Plus

This integration provides two standalone ESP32-S3 FAPs and their required firmware
API exports. BW16 R4TKN controls the public v4 UART operations described in
[BW16-CONTROLS.md](BW16-CONTROLS.md). Wardriver surveys
Wi-Fi/BLE using the ESP32's own radios, with local CSV logs and offline OUI names.
Wardriver does not use BW16 or BW15 as a radio backend.

The BW16 work builds on the companion-app proposal in
[PR #114](https://github.com/Sor3nt/Flipper-Zero-ESP32-Port/pull/114) by
@Adam-neeeds-help and the R4TKN firmware by @rusyln. The unrelated external-IR
change is not included. The controller supports scanning, station/client lists,
confirmed station/client/all-AP deauthentication, and acknowledged STOP. Active
operations require hold-to-confirm; the host requests STOP after 30 seconds.
Missing start/stop acknowledgements are reported, not treated as success.

## Build matching firmware and apps

Use ESP-IDF 5.4.1, the ESP32-S3 toolchain, Python with Pillow and pyelftools, and
Bash for `buildFap.sh`. Start in a separate checkout when using the normal board
build helpers: they can regenerate `sdkconfig`.

In an activated ESP-IDF Bash environment:

```sh
bash build.sh --board t_embed --build-only
bash buildFap.sh applications_user/bw16_r4tkn
bash buildFap.sh applications_user/wardriver
python tests/bw16/check_abi.py
python tests/bw16/check_abi.py build_t_embed/fap/wardriver.fap
```

For Windows firmware builds, set `ESP_IDF_DIR` to the installed IDF directory and
run `python winbuild.py build --board t_embed`. Run the FAP commands in Git Bash
with the same IDF and compiler environment. Existing configured checkouts may
instead use `idf.py -B build_t_embed -DFLIPPER_BOARD=lilygo_t_embed_cc1101 reconfigure build`
with `FLIPPER_BOARD=lilygo_t_embed_cc1101` in the environment. Ensure the primary
console is USB Serial/JTAG and the secondary console is disabled.

Build outputs are `build_t_embed/furi_esp32.bin`,
`build_t_embed/fap/bw16_r4tkn.fap` and `build_t_embed/fap/wardriver.fap`.
Use the firmware and apps together: upstream firmware lacks the new imports.
The ABI checker verifies the linked export hashes/addresses and every FAP import.

## Manual flashing and SD files

Disconnect BW16 TX/RX signal wires before flashing the T-Embed. From an activated
ESP-IDF environment, use the generated partition layout:

```powershell
Set-Location build_t_embed
python -m esptool --chip esp32s3 --port COMx --baud 460800 --before default_reset --after hard_reset write_flash '@flash_args'
```

Replace `COMx` with the device's port. The generated `flash_args` supplies the
offsets for the bootloader, partition table, application and OTA metadata; keep
its referenced files together. Do not substitute offsets from another build.

| Built/generated file | SD-card destination |
| --- | --- |
| `build_t_embed/fap/bw16_r4tkn.fap` | `apps/GPIO/bw16_r4tkn.fap` |
| `build_t_embed/fap/wardriver.fap` | `apps/Tools/wardriver.fap` |
| `build_t_embed/sdcard/apps_data/wifi/mac-vendor.txt` | `apps_data/wifi/mac-vendor.txt` |

Generate the vendor file with `python tools/wardriver_oui.py`, or supply a local
IEEE MA-L CSV using `--input path/to/oui.csv`. The app works without it but cannot
show vendor names. Preserve existing logs/settings under `apps_data/wardriver`.

## BW16 wiring and the fitted NRF

| BW16 peripheral UART | T-Embed CC1101 Plus |
| --- | --- |
| TX1 | GPIO43 (UART RX / NRF CE) |
| RX1 | GPIO44 (UART TX / NRF CSN) |
| GND | GND |

Use 3.3 V logic, 115200 baud, 8N1 and an appropriate separate regulated BW16 supply.
Exact BW16 pad numbers depend on the board. The protocol was checked against the
R4TKN v4 binary described in the [BW16 README](applications_user/bw16_r4tkn/README.md).
Five Ghost and BW15 compatibility is unverified.

The built-in NRF remains fitted. Preparation verifies NRF power-down and acquires
a cooperative pin lease. The reversed UART mapping keeps CSN HIGH during BW16
replies; both the firmware SPI mutex and IDF bus lock stop shared SPI clocks while
commands toggle CSN. A TX failure forces CSN HIGH before SPI is released.

BW16 TX/RX must be disconnected at boot/reset, during preparation and before
leaving the app. Software cannot isolate the BW16's output during reset. The
normal NRF app and BW16 cannot operate simultaneously. Legacy background apps
that bypass pin ownership are not covered. This has host tests, not electrical
validation on the Plus hardware.

## Host checks and scan-only hardware acceptance

```sh
python tests/bw16/run.py
python tests/bw16/test_ui.py
python tests/host/run_wardriver_tests.py
```

On Windows use `python tests/bw16/run.py --zig C:/path/to/zig.exe` and set `ZIG_CC`
to that executable for UI and Wardriver tests. Windows uses native mocks without
ASan/UBSan; Linux GCC checks retain sanitizers. Covered areas include framing,
malformed records, indexed lists, START/STOP deadlines, actual controller UI,
UART errors, GPIO restoration, NRF guard behavior, radio
lifecycle, startup allocation failures, CSV/NMEA/OUI parsing and storage errors.
Upload checks use mocked HTTP only.

Hardware acceptance remains outstanding:

1. Boot with BW16 signal wires disconnected; verify the display, encoder and SD.
2. Open BW16 R4TKN, hold OK to prepare, and connect only after the ready prompt.
3. Scan and confirm known networks plus a received `SCAN_DONE`; with no module,
   verify the timeout. Check Station list and Observe clients on a known AP.
   No deauthentication or beacon flooding is part of hardware testing.
4. Back, unplug both signal wires, then hold OK to exit. Repeat and check normal
   display, SD, CC1101 reception and NRF receive operation.
5. Open Wardriver with GPS OFF, start surveying, verify observations and offline
   vendors, then stop and inspect the local CSV. Exit and reopen. GPS on GPIO43/44
   is rejected; alternative GPS wiring requires separate board verification.

See the [Wardriver README](applications_user/wardriver/README.md) for feature
limits and the startup fix. Compilation and host tests do not establish physical
radio behavior or device stability.
