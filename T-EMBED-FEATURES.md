# T-Embed feature set

This change brings the existing custom applications into the PR. Wardriver is
extended from its existing implementation; the map is a separate application.
The source-built BW16 companion is R4TKN-v4-protocol compatible, not an official
R4TKN release or a rebuild of unavailable upstream source.

## Hardware profiles

| Name | Profile | GPS UART43/44 | BW16 |
| --- | --- | --- | --- |
| **Lilygo T-embed cc1101** | `cc1101` | Available with NRF absent | I2C SDA8/SCL18 |
| **Lily T-embed cc1101 plus** | `cc1101-plus` | Reserved for fitted NRF | I2C SDA8/SCL18 |

Both use the same BW16 I2C 4.5 image and BW16 FAP. Wardriver has a separate Plus
variant that rejects saved GPS configurations on NRF and I2C pins. The host images
are different. Firmware capability checks prevent the UART app from using the
Plus header. Earlier guarded-UART ABI exports remain for compatibility.

**Connect BW16 while the host is fully powered off, with host USB removed. Deep
Sleep is insufficient.** Start with BW16 already connected. The owner confirmed
this works on the Plus; connecting it while running caused resets and SD mount
failures. See [complete setup and flashing instructions](T-EMBED-BW16-SETUP.md).

## BW16 controller

- Scans both supported Wi-Fi bands, displays indexed AP results, and provides
  cached station lists and passive client observation.
- Multiple target selection, grouping of identical SSIDs across discovered
  bands, explicit pairing of differently named Wi-Fi entries, and loading/status
  indicators during scans and interactions.
- Selected AP/client operations and selected-group operation are retained.
  The companion schedules selected channels locally instead of waiting for a
  new host command between each channel. BW16 has one radio: channel visits are
  sequential, including across 2.4 and 5 GHz. It cannot transmit on both at once.
- Attempt counters and acknowledgements are reported separately from effects
  observed over the air. A growing counter does not verify a disconnection.
- I2C framing includes session IDs, sequence checks, CRC32, bounded buffers,
  retry handling, stop handling and a link-loss timeout. It borrows the existing
  power/NFC I2C driver without reinstalling or resetting that shared bus.
- The earlier UART controller is retained for the non-NRF profile and the
  corresponding UART companion; UART and I2C wiring/images are not interchangeable.

### Handshake capture and PCAP files

The app includes **Capture handshake** (passive) and a distinct **Capture + deauth**
option. The latter arms capture before its bounded reconnect burst. Passive
capture sends no deauthentication frames. Both use the selected BSSID/channel;
capture does not follow clients to another band or capture multiple channels
simultaneously.

The SD card receives unique files such as
`apps_data/bw16/captures/capture_0001.pcap`. Existing captures are not overwritten.
The file is classic PCAP with raw IEEE 802.11 link type 105, holding the first
matching beacon and matching EAPOL frames, including partial exchanges/retries.
Sessions are bounded to 120 seconds or 256 packets and can be stopped from the
app. Storage preflight and stream checks reject invalid/truncated records.

**A saved PCAP is not proof of a complete handshake.** Zero EAPOL packets means
none was captured; partial exchanges remain partial. Inspect the exported file
in a packet analyzer to determine its contents. No password recovery is performed
by these apps. Implementation details are in the [capture guide](docs/BW16_4_4_CAPTURE.md);
that older guide's UART wiring applies only to its UART variant.

## Existing Wardriver

The existing UI, Wi-Fi/BLE scanning, NMEA GPS, SD logging, vendor database and
notable/surveillance-device heuristics remain. Detection labels are hints, not
proof of a device's identity. Wardriver uses the ESP32 radios independently of
BW16 and does not scan 5 GHz with the ESP32-S3.

The retained updates include GPS-positioned WiGLE-format CSV, fix freshness
checks, loss/recovery handling, bounded tables/queues, incremental vendor lookup,
and the unified Uploads screen for WiGLE and WDGWars. Upload requires credentials
and user confirmation, stops the survey first, and reports each service's result.
WiGLE and WDGWars credentials are separate and are never committed with firmware.
See [WDGWars upload](docs/WDGWARS_UPLOAD.md) and the
[Wardriver README](applications_user/wardriver/README.md).

## Standalone WDGWars Map

- Native color map with its own WD icon; no browser, Leaflet or JavaScript runtime.
- World land overview at zoom 0, pan/zoom, scale bar, GPS marker/status where a
  usable GPS is configured, and manual positioning where it is not.
- Authenticated WDGWars cells, gang ownership names/colors, returned territory
  outlines, nearby territory details and account information.
- Public OpenStreetMap vector context via Overpass, with separate service
  results and retry/backoff. Useful partial street data is retained when a dense
  query reaches the bounded path/vertex limits; an incomplete download is reported.
- Online loading and four SD offline areas, including cache validation, backup
  recovery and download-age reporting. Previously downloaded geometry remains
  usable offline. The world outline is built in; detailed streets/territories
  require a successful download. An API key alone does not provide offline data.
- WDGWars credentials reuse `apps_data/wardriver/wdgwars.conf`; no duplicate key
  store. The map does not start Wardriver or run a background survey.

Returned hulls are drawn directly. The API cell anchor convention is unverified,
so cell markers do not invent boundary polygons. Cached age is the local download
age, not a guaranteed server generation timestamp. Streets and territories have
independent coverage and limits. See [map details](docs/WDGWARS_MAP.md) and
[source attribution](docs/WDGWARS_BASEMAP_SOURCES.md).

## RF display and ProtoPirate

RF Waterfall and the Sub-GHz spectrum view retain the native color waterfall,
centered frequency display and signal history. See
[spectrum documentation](docs/SPECTRUM_ANALYZER.md).

ProtoPirate keeps its decoder/plugins and gains the receive-history lifetime
and concurrency fix: incoming records no longer free a reader borrowed by the
UI, and list updates run on the UI event loop. The same replacement FAP works
with both profiles. Its host regression passes; hardware confirmation of this
specific fix is still pending. See [RX stability](applications_user/protopirate/RX-STABILITY.md).

## Stability boundaries

The owner reported working scans, selected operations, map display, and BW16/SD
operation after a true power-off/connect/start sequence. The newly integrated PR
build must still be checked on both physical models; compilation and host mocks
do not establish hardware acceptance of a different firmware image.

SD recovery code is opt-in (`CONFIG_T_EMBED_PLUS_SD_RECOVERY`) and disabled in the
two normal profiles. It includes bounded retries and rejection of a false tiny
SD-card identity; it does **not** solve hot-plug resets or simulate removal of SD
power. The earlier delayed-start BW16 experiment is excluded. The separately
observed boot-time SPI assertion remains unresolved. Do not market these as fixed
by the ProtoPirate change or by I2C.

## Build from this PR

Use an activated ESP-IDF **5.4.1** environment with the ESP32-S3 toolchain, Ninja,
Python `pyelftools` and `Pillow`, and Bash (Git Bash on Windows). The profile helper
copies its config into an isolated build folder, builds firmware and the four
apps, and verifies app imports against the linked firmware. It never flashes.

```text
python tools/build_t_embed_profiles.py --profile cc1101
python tools/build_t_embed_profiles.py --profile cc1101-plus
```

Outputs are `build_cc1101/` and `build_cc1101_plus/`, with their FAPs under `fap/`.
Install `wardriver.fap` and `wdgwars_map.fap` in SD `apps/Tools/`,
`bw16_r4tkn_i2c.fap` in `apps/GPIO/`, and `proto_pirate.fap` in `apps/SubGhz/`.
For a fresh ProtoPirate installation, also merge the generated
`proto_pirate_assets/` into SD `apps_assets/proto_pirate/`; preserve existing
compatible protocol assets when replacing only its main FAP.

To flash a source build in the activated IDF environment, with BW16 unplugged
at the Lilygo connector, use the corresponding build directory and actual port:

```text
idf.py -B build_cc1101 -p COM4 flash
idf.py -B build_cc1101_plus -p COM20 flash
```

Build the common module image using Arduino CLI with installed
`realtek:AmebaD` **3.1.9** and `ameba_d_tools` **1.1.3**:

```text
python tools/build_bw16_plus_i2c.py
```

The module output is `build_t_embed_plus_i2c/bw16/`; this build tool never opens
hardware. For the new output, use the verified uploader from your working BW16
bundle (it is not the SDK uploader, which has a different command line):

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\flash_bw16_i2c.ps1 -Uploader "C:\path\to\working-bundle\BW16\upload_image_tool_windows.exe" -CheckOnly
powershell -ExecutionPolicy Bypass -File .\tools\flash_bw16_i2c.ps1 -Uploader "C:\path\to\working-bundle\BW16\upload_image_tool_windows.exe" -Port COM3
```

The script verifies the uploader and all four image hashes before opening a port.
The setup guide explains USB isolation, BURN/RST upload mode and the required
successful checksum result. Binary bundles, upload executables and personal keys
are not committed to the PR.

The retained UART-only companion can be built separately with
`python tools/build_bw16_native.py`, producing `build_t_embed/bw16_native/`.
It is for the non-NRF UART wiring described in the 4.4 guide, not the I2C cable.
Build its optional host FAP with `FAP_BOARD=lilygo_t_embed_cc1101` and
`FAP_FW_BUILD_DIR=build_cc1101` set when invoking
`bash buildFap.sh applications_user/bw16_r4tkn`. It is not installed by the I2C
profile builder. Do not substitute that companion image for I2C 4.5.

Run the offline production-code regressions with:

```text
python tests/run_t_embed_features.py
```

On Windows set `ZIG_CC` to an installed `zig.exe`; native assertions remain
enabled but ASan/UBSan are not used there. Linux uses GCC with sanitizers.

See [integration validation](docs/T_EMBED_FEATURE_VALIDATION.md) for the checked
builds, test coverage and remaining hardware acceptance.
