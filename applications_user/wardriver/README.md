# Wardriver 1.3.2 for this ESP32-S3 firmware

Passive Wi-Fi/BLE surveying for the LILYGO T-Embed CC1101 Plus. This is an
Xtensa ESP32-S3 FAP. Install it with matching firmware using the
[deployment guide](../../BW16-DEPLOYMENT.md).

## Version 1.3.2: startup exit fix

The preceding build silently returned before showing the UI if the largest free
internal-RAM block was smaller than 48 KiB. That is not the app's allocation
requirement: its small control objects use internal RAM and its large tables,
index and worker stack can use PSRAM. A fragmented heap could therefore reject
an otherwise viable launch. This was identified after an immediate-exit report; the exact device-side
cause still needs serial-log confirmation.

Startup now attempts its actual allocations and releases partial allocations on
failure. Failures returned by allocation APIs show the failed stage and internal
RAM statistics in a dialog; USB logs also identify version 1.3.2 and memory sizes.
If even an error dialog cannot allocate, the USB log retains the reason. Firmware
helpers that assert internally on severe exhaustion remain firmware limitations.
The app's atomic control object still stays in internal RAM.

About / limits shows version 1.3.2.

Host checks exercise the actual entry point with 4/8/32 KiB largest-block values,
six injected allocation failures and normal cleanup. Windows optimized host tests
explicitly retain assertions. All current checks and all 166 linked FAP imports
are checked against the matching firmware. A device retry is still required.

## Features present in this source

- Passive 2.4 GHz Wi-Fi and BLE surveying using the ESP32's internal radios.
  It does not use BW16 as a radio backend; both apps can be installed and used
  separately. No BW15 protocol support is claimed.
- Bounded network table, RSSI/channel/BSSID or BLE address, observation counts,
  first/last-seen values and offline OUI vendor names.
- Broadcast hints and supported Remote ID fields, labelled as clues rather than
  proof of a particular product, camera, tracker or drone. Randomized MACs do
  not identify a manufacturer.
- Buffered local CSV logs, optional NMEA GPS, and separate WiGLE CSV files only
  for positioned Wi-Fi observations. Stale GPS fixes are not reused for new rows.
- The optional WiGLE upload/configuration UI is retained. It sends only after
  explicit selection and confirmation; upload tests use simulated HTTP, not the
  real service. No account or captured scan data was uploaded in this integration.

The firmware now exports the app's real radio, UART, HTTPS, storage and runtime
imports. The WLAN survey owner suspends normal station activity, rejects competing
commands and restores the previous state on exit. Bluetooth teardown uses actual
controller/host states, including an initialized but non-advertising controller.
Callbacks and queued observations are drained before radio restoration. Storage
sync reports actual flush/fsync failures instead of always claiming success.

## SD files and offline OUI database

Copy the built files onto the SD card, preserving existing logs/settings. Required destinations (relative to the card root):

| Packaged file | SD destination |
| --- | --- |
| `wardriver.fap` | `apps/Tools/wardriver.fap` |
| `bw16_r4tkn.fap` | `apps/GPIO/bw16_r4tkn.fap` |
| `mac-vendor.txt` | `apps_data/wifi/mac-vendor.txt` |

Generate the offline database with `tools/wardriver_oui.py`, or reuse an
existing compatible vendor text file. The database is not embedded in the FAP.
A 40,214-prefix IEEE MA-L snapshot was exercised by the host loader checks.

The app loads the index incrementally from `/ext/apps_data/wifi/mac-vendor.txt`.
Without that file it can survey, but cannot show OUI vendors. The loader accepts
common Sor3nt vendor text separators, bounds the file to 8 MiB and index to
65,536 entries, and uses smaller allocation fallbacks. A partial index may result
if available memory limits capacity. Display names are limited to 28 bytes.
MA-M/MA-S allocations and manufacturers behind randomized addresses are not
resolved. A storage read error invalidates the index rather than reporting it ready.

To regenerate later from an official MA-L CSV:

```powershell
python tools/wardriver_oui.py --input C:\path\oui.csv
```

Without `--input`, the tool downloads the [IEEE MA-L registry](https://standards-oui.ieee.org/oui/oui.csv).
The FAP itself does not download a vendor database. Copy the generated file from
`build_t_embed/sdcard/apps_data/wifi/mac-vendor.txt` to the same SD destination.

## GPS and NRF ownership

GPS defaults OFF with no pins assigned. Surveying and vendor lookup work without
GPS. Existing settings live at `/ext/apps_data/wardriver/config.conf`; malformed
settings fall back to GPS OFF. Logs and optional WiGLE configuration remain under
`/ext/apps_data/wardriver/`. Existing files are not erased by installing the FAP.

**GPIO43 and GPIO44 are reserved and rejected by the GPS backend**, even if an
older settings file selected them. They are NRF CE/CSN and are also used by BW16's
special guarded connection workflow. Arbitrary GPS UART traffic cannot safely
share them. A setting that says pins are isolated cannot override this restriction.

Custom GPS pins still require verified electrical isolation from board hardware.
No spare pair is assumed; USB and flash/PSRAM pins are also rejected. GPS wiring
on this particular Plus revision has not been validated. Leave GPS OFF for the
initial test. Do not attach GPS to the BW16 wiring.

## Build and tests

See the [deployment guide](../../BW16-DEPLOYMENT.md) for firmware and FAP builds.
The FAP build
now follows IDF's target-to-header mapping: ESP32-S3 intentionally shares the
C3-family Bluetooth header in IDF 5.4, while C6 has its own header. Firmware and
FAP therefore use the same controller configuration ABI.

Host checks compile the production parser, table, logger, vendor loader, radio
lifecycle and upload code with mocked hardware/storage/network boundaries:

```powershell
$env:ZIG_CC='C:\path\to\zig.exe'
$env:ZIG_GLOBAL_CACHE_DIR="$PWD\build_host_bw16\zig-cache"
python tests/host/run_wardriver_tests.py
```

Windows uses native Zig checks without ASan/UBSan; the Linux GCC runner retains
those sanitizers. Tests cover malformed input, cancellation, failure cleanup and
repeated start/stop. They do not establish RF behavior, device stability, GPS
wiring, SD-card reliability or actual WiGLE service acceptance.

## Scan-only device acceptance

1. Disconnect BW16 signal wires and leave the built-in NRF fitted.
2. Open Wardriver, keep GPS OFF, wait for READY, then Start. Vendor loading runs
   incrementally during scanning; allow it time to finish.
3. Confirm known nearby Wi-Fi/BLE observations and vendor names for public MACs.
   Randomized addresses should not be assigned a vendor from their prefix.
4. Stop, inspect the local CSV, then exit and repeat. Check that other normal
   Wi-Fi apps and the previous Bluetooth state still work afterward.
5. Test BW16 separately using its prepare/connect/disconnect sequence. No
   deauthentication, beacon flooding or upload is part of the smoke test.
