# Historical T-Embed feature validation

This records validation shipped with the earlier source bundle. It does not
describe the current six-app PR build. See [current review guide](T_EMBED_NATIVE_TOOLS_REVIEW.md).

The imported source comes from the existing custom applications, not a new
Wardriver implementation. Both profiles retain separate GPIO policy and share
the I2C companion. No device is automatically flashed by the build/test helpers.

## Offline checks

`python tests/run_t_embed_features.py` runs 30 groups covering existing Wardriver
parsers, uploads, storage, startup, radio lifecycle and UI; spectrum sampling and
rendering; BW16 protocol, selection, capture, transport and retained UART guards;
map JSON/HTTP/cache/GPS/input/rendering; ProtoPirate history lifetime/concurrency;
and the opt-in SD recovery code. Windows checks used Zig with assertions enabled;
they did not run ASan/UBSan. HTTP, storage and radio faults are mocked. The power
check is a source-preservation guard, not a physical shutdown test.

All 30 groups passed in the integration checkout. The functional ProtoPirate
regression distinguishes the old lifetime bug from the fixed implementation.
The UART selection/UI and I2C app groups also passed again after replacing
personal network-name examples with neutral fixtures.

## Companion builds

Both source builds succeeded with Realtek AmebaD 3.1.9 and tools 1.1.3. Their
application images match the previously distributed working bundle images
byte-for-byte:

| Companion | Bytes | SHA-256 |
| --- | ---: | --- |
| I2C 4.5, both models | 679936 | `7d502d3b0abca6928897654302549e4d5a4013de6cc2f3594bc7e67c533ad391` |
| Retained UART 4.4, non-NRF | 675840 | `ae7557bd24d722058d2d166518c84d7b87f12dff33a3924704a26172d974d4b0` |

The I2C flashing helper passed `-CheckOnly` against the generated images and the
verified working-bundle uploader. No COM port was opened.

## Host firmware and FAPs

Both ESP32-S3 firmware profiles built with ESP-IDF 5.4.1. Each profile built
BW16 I2C, its corresponding Wardriver, WDGWars Map and ProtoPirate FAPs, plus all
30 embedded ProtoPirate plugins. All four main FAPs passed checks against their
actual linked host ELF: API version, imports, supported relocations and metadata.
The WD icon also passed its byte-level check. The 1,068 linked API entries were
sorted, unique and matched the expected symbol addresses; every previous PR
export was retained. Plugin imports, metadata and relocations passed too.

| Profile | Application image bytes | SHA-256 |
| --- | ---: | --- |
| Lilygo T-embed cc1101 | 3542080 | `7d6b70b3525cc36f330af0434f76edfd32d9b442730a97c71eba32ac1d7abd61` |
| Lily T-embed cc1101 plus | 3542272 | `ed13e9d144357a8ad239ab44548b89980edc510528388d467c5c428e58735191` |

These identify the locally verified integration images; binaries are not attached
to this source PR. Build manifests record the FAP and plugin hashes separately.
Compiler warnings remain in imported code; these were successful builds, not
warning-free builds. Nothing was flashed during integration verification.

## Hardware boundary

The owner reported working BW16 scans/selected operations, map display, and
normal BW16/SD operation when connecting the module with the Plus fully powered
off and then starting it. These observations concern the earlier device builds;
they are not fresh acceptance tests of the combined PR host images.

The new host builds and the specific ProtoPirate receive fix still need hardware
acceptance. The Plus hot-plug reset/SD failure is not fixed; Deep Sleep is not a
substitute for full Power Off. The separately observed boot SPI assertion is
also unresolved. Capture format tests do not prove that a real network yielded
a complete handshake, and TX counters do not verify radio effects. No live upload
or disruptive over-the-air operation was used for this integration's validation.
