# T-Embed Native Tools — maintainer review guide

This is a consolidation of the previously prepared native ESP32 firmware and FAP work, rebased by applying its source changes to SOR3NT's current `main`, commit `0e367fb5c7c7a0e1b3b3467eff412c06a7661b86`. Upstream has no `dev` branch, so this proposal targets `main`. The proposed build identity is **T-Embed Native Tools**, version `2.0.0-tembed-tools`.

This is the native ESP32/Furi port. The Bruce JavaScript interpreter port is a separate project and is not bundled here. Bruce PR #2961 was the review reference for that separate port, not a specification imposed on this repository.

## Review order and overlapping pull requests

1. Review firmware integration: TLS slots, radio ownership, shared pins, storage handoff, API exports and board profiles.
2. Review app changes and their host tests using the inventory below.
3. Build firmware and matching FAPs together, then run the hardware acceptance checks.

This consolidation overlaps SOR3NT PR #118 (BW16/Wardriver/map/profiles) and #119 (ProtoPirate 3.6.3). It includes their prepared work plus the later app revisions. Those PRs are not automatically closed or merged by this proposal; maintainers should choose one merge path and avoid applying overlapping patches twice. A squash merge is suitable for this browser-uploaded source consolidation. Only ProtoPirate, WIFITE, BW16, Wardriver, WDGWars Map and RTL433 are included. No prebuilt firmware images, personal captures, SD credentials or generated build directories are part of the proposed source change.

## Included app inventory

| App / source directory | Revision | Changes and integration notes |
| --- | --- | --- |
| `applications_user/protopirate` | 3.6.3 local port | Ported protocol sources and 34 matching FAL plugins; emulation compiled in by default; receive/history stability; lazy SD saves; rotary and Back navigation; configuration-view cleanup. Protocol labels report implemented capabilities and are not proof of physical interoperability. |
| `applications_user/rtl433` | 1.1.6 | Complete catalog (377 protocol entries, 333 enabled by default); native OOK/FSK capture; original two-profile AUTO plus optional ALL/FIXED; structured history, JSON/CSV and RAW save/replay/offline decode. Keeps delayed FSK packets and coalesces partial snapshots to prevent full-packet queue starvation. Worker stack is 60 KiB. |
| `applications_user/wardriver` and `wardriver_plus_i2c` | 1.5.2 | Passive Wi-Fi/BLE survey, NMEA GPS, WiGLE export and WDG upload workflow; bounded queues, radio handoff and two hardware pin profiles. |
| `applications_user/wdgwars_map` | 1.3 | Native RGB565 world/street/territory map, GPS/manual position, pan/zoom and four offline areas; bounded network and cache handling. |
| `applications_user/bw16_r4tkn_i2c` | 4.5 | Native BW16 companion control, sequenced/CRC-checked I2C transport, bounded capture queues and PCAP export. Shared I2C ownership is respected. Companion source is included. |
| `applications_user/bw16_r4tkn` | 4.4 legacy | Preserves the prepared UART variant; it is separate from the recommended I2C profile. |
| `applications_user/wifite` | 0.8.1 experimental | Native ESP32 WPS workflow and firmware-resident bridge. Its SDK adapter is pinned to ESP-IDF 5.4.1; this is not a claim of complete desktop Wifite parity or verified success against every AP. |

Existing upstream applications remain present. Upstream licenses and copyright/font attribution are retained; product/build branding has been changed without rewriting third-party attribution.

## Firmware changes that matter to reviewers

- **TLS compatibility:** Furi thread identity moves to FreeRTOS TLS slot 1, leaving slot 0 to ESP-IDF pthreads. Configurations require at least two slots. The host regression reproduces the previous collision and checks independent identity, values and destructors.
- **Radio lifecycle:** bounded Wi-Fi/BLE shutdown/start handoff and ownership checks support repeated app switching. These changes require on-device coexistence testing, particularly with shared CC1101/LCD/SD SPI wiring.
- **Shared pins:** NRF/RFID/external-UART ownership is explicit. The Plus profile reserves GPIO43/44 for NRF; the non-NRF GPS profile uses those pins for UART. I2C uses SDA8/SCL18. Select the profile that matches the actual wiring.
- **Storage:** USB/SD handoff and optional recovery paths are explicit. Recovery is opt-in; this does not assert that every card or hot-plug scenario is fixed.
- **FAP compatibility:** firmware exports are extended for the included applications and must stay hash-sorted. New FAPs must be distributed with the matching firmware/API; an old firmware image may fail to load them.
- **Display/input:** direct RGB565 drawing and cleanup, shared viewport behavior, rotary navigation and map display are included.
- **NFC/EMV:** this consolidation does not change NFC/EMV protocol implementation. Shared platform changes can still affect peripherals; actual card-read regression testing remains required. The separate Bruce EMV changes are not claimed here.

## Build and install from source

Use ESP-IDF **v5.4.1** (`4c2820d377d1375e787bcef612f0c32c1427d183`), including its submodules. Activate `export.sh`, then install `idf-component-manager==2.2.2`, `Pillow` and `pyelftools` in the IDF environment. Keep the checked-in dependency lockfile; do not silently update component versions during this review.

```sh
python -m pip install idf-component-manager==2.2.2 Pillow pyelftools
bash BUILD-NATIVE-TOOLS.sh
```

The script builds the T-Embed firmware into `build_native`, builds six matching FAPs and checks their imported symbols against the firmware API. It never flashes a device. The ProtoPirate output is `proto_pirate.fap`, with its matching `proto_pirate_assets/plugins` FAL files; preserve that directory structure on SD.

For explicitly selected wiring profiles:

```sh
python tools/build_t_embed_profiles.py --profile cc1101 --jobs 2
python tools/build_t_embed_profiles.py --profile cc1101-plus --jobs 2
```

The profile builder compiles the corresponding Wardriver variant, includes all six app builds, checks their linked ABI and records artifact hashes. `--faps-only` requires an already-built matching firmware. Use the generated build's `flash_args`/`flasher_args.json` and actual board profile when flashing; no single image is asserted to fit every supported board. Do not mix historical `.fap` or `.fal` binaries with this build.

The BW16 companion is a separate device/build. See `BW16-DEPLOYMENT.md`, `T-EMBED-BW16-SETUP.md` and the companion source for its requirements. Large SD theme/media packs are not firmware source dependencies.

## Verification recorded for this consolidation

The following host checks were rerun on the consolidated source, rather than inferred from an older release:

- All **28 suites** in `tests/run_t_embed_features.py` pass.
- RTL433 host suite passes, including delayed FSK and partial-backlog fixtures using actual decoder code, field/JSON/CSV output and RAW replay checks.
- ProtoPirate 3.6 / 3.6.2 / 3.6.3 tests pass, including 1,000 synthetic Chrysler frames and repeated configuration exits.
- Furi/pthread TLS regression passes against the actual ESP-IDF 5.4.1 pthread TLS source.

Host runs use ASan/UBSan where supported. LeakSanitizer is unavailable in the current execution environment, so these runs use `ASAN_OPTIONS=detect_leaks=0`; this is a test-runtime limitation, not evidence of a leak-free physical device. Compiler warnings found in recovered host tests/map code were fixed without disabling `-Werror`.

The T-Embed CC1101 firmware and six matching FAPs are verified using `BUILD-NATIVE-TOOLS.sh`, including linked ABI checks. The 34 ProtoPirate embedded plugins also receive metadata/import/relocation checks. Hardware acceptance remains separate from compilation and host fixtures.

Useful commands:

```sh
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 python3 tests/run_t_embed_features.py
ASAN_OPTIONS=detect_leaks=0 bash applications_user/rtl433/tests/run.sh
ASAN_OPTIONS=detect_leaks=0 python3 tests/protopirate363/run.py
python3 tests/thread_tls/test_pthread_coexistence.py
```

## Hardware acceptance and known limits

No physical board was attached while preparing this consolidation. Previous user reports on the Bruce port do not establish SOR3NT hardware validation. A successful build/host test cannot certify production readiness for all users or all RF protocols.

Before releasing, verify on the intended T-Embed CC1101 board: cold boot; SD mount and USB handoff; repeated launch/exit; CC1101 receive/transmit with known fixtures; existing NRF operation; Wi-Fi/BLE coexistence; GPS and BW16 wiring; NFC/EMV reads; RFID; display/audio; sleep/wake and power-off. Check both standalone operation and relevant shared-bus combinations.

The previously reported BW16 hot-plug reset/SD-mount failure is not claimed fixed. Attach or detach the companion only with full power removed, including USB. A reported boot-time SPI assertion also needs an on-device trace before it can be considered resolved. Protocol emulation being enabled does not mean every rolling-code variant or physical receiver has been verified. Do not label unresolved protocols or hardware checks as passing.

## Provenance

The consolidation is recovered from the user's published native-tools source bundle in `salikisazazil/Flipper-firmware-Flashable-files` at commit `6a28fca1096ac1fd5968908d3866fbc5c2ca69d2`, overlaid with the published RTL433 1.1.6 source. Source/license notices remain alongside the vendored code. The upstream README, unrelated assets and dependency lockfile are retained. Obsolete ProtoPirate protocol/keystore locations are removed only where replaced by the port's current source/assets layout.
