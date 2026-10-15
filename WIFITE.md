# WIFITE 0.8 — T-Embed Native Tools experimental build

Firmware: **2.0.0-tembed-tools**, ESP-IDF **5.4.1**, target **ESP32-S3**,
board profile **lilygo_t_embed_cc1101**, 16 MB flash, 320 × 170 display.
This build includes the local video player and existing T-Embed applications. WIFITE runs on the T-Embed itself and needs no Raspberry Pi, Kali host,
BW16, or external Wi-Fi adapter.

**Status: compiled and host-tested, not tested on a physical T-Embed or AP.**
The native radio exchange is experimental. Successful compilation and synthetic
protocol tests do not establish that the ESP32 Wi-Fi driver will complete
authentication, association, and WPS exchanges with real routers. This package
is ready for hardware validation, not a claim of universal Wifite/Reaver parity.

## Changes in 0.8

- Fixes identical WPS requests being rejected when the AP uses a new EAP
  identifier. The last complete WSC response is resent with that identifier;
  its authenticated body, DH keys and nonces remain unchanged.
- Keeps the complete response separate from temporary fragment acknowledgments.
  This also covers a repeated complete message delivered in fragments.
- Changed M1 payloads are still treated as an incomplete/restarted session.
  They are not mistaken for retransmissions or accepted using old keys. Logs
  show the received identifier and expected message when an exchange stops.
- Pixie logs explicitly distinguish capture preparation from an offline search
  that has actually started with a valid M3. Online testing reports which PIN
  remains unverified after three consecutive inconclusive attempts.

The new host test first reproduced the v0.7 failure, then verified the actual
exchange worker with independent Python peer transcripts and real Mbed TLS
crypto: capture, M1/M3/M5 retransmissions, M1–M7 known-PIN verification,
fragment ACK isolation, changed-payload rejection, TX failure and a bounded
duplicate stream. This is not physical RF validation. The screenshot alone
cannot establish whether that AP repeated identical bytes or restarted with
different data. Both firmware and matching FAP are included; firmware flashing
is required for the protocol-handler correction.

## Changes in 0.7 (previous release)

- Fixes Auto ending after an inconclusive first test of 12345670. It now tries
  a fresh Pixie capture after either a PIN rejection or an unverified exchange.
  The latter remains unverified; a transport timeout is not a rejected PIN.
- Releases and reacquires the radio/WPS context between Auto stages, then
  checks the selected router's current advertisement before continuing.
- If Pixie cannot obtain a valid M3 capture, it explicitly reports that the
  offline search did not run and proceeds to the bounded online PIN stage.
  A completed Pixie search with no match also proceeds to that stage.
- Online testing still retries the same PIN after inconclusive exchanges and
  stops after three consecutive inconclusive attempts without skipping PINs.
  Confirmed locks, cancellation, unavailable targets and initialization errors
  still stop the job. A verified first PIN still ends Auto successfully.

Both firmware and the matching FAP must be updated. The workflow is verified
with host regression tests; the underlying association timeout is not confirmed
fixed on physical hardware. This release adds no new upstream port.

## Changes in 0.6 (previous release)

- The action screen now has exactly two choices: **Auto test** and **Test one
  PIN**. Every Auto run starts with **12345670**, including resumed runs. A
  verified success stops immediately. A conclusive rejection proceeds to the
  standard Pixie search, then online PIN search if that search finds no match.
  An inconclusive first-PIN exchange stops with its diagnostic, rather than
  silently treating a transport failure as an incorrect PIN. Manual PIN entry
  also starts at 12345670 and remains editable.
- Scan opens a live router table with separate **WPS**, **LOCK**, **CLI**, channel
  and signal columns. YES means WPS advertised, -- means not observed. LOCK is
  the AP's PIN-lock advertisement; CLEAR means its flag was explicitly zero;
  ? means unknown. CLI is the number of distinct clients observed in traffic,
  not a complete association list. Zero does not prove no clients are connected.
- All discovered networks can appear (up to 64). Client traffic updates an AP's
  count immediately, including after its final beacon. A beacon without a WPS
  IE does not erase WPS support seen in another packet during the same scan.
- Fresh status checks discard frames captured before the check or on another
  channel. They require distinct packet sequence numbers over at least 250 ms.
  An advertised PIN lock triggers up to 30 seconds of **passive** waiting. Only
  repeated explicit zero flags clear an observed lock; an absent flag cannot.
  The actual AP Setup Locked attribute (0x1057) and its source are logged.
- WSC NACK logs show the configuration error and distinguish first/second PIN
  rejection from a setup-locked response. Confirmed WSC setup locks still stop.
  This does not disable or bypass the router's lock.

The screenshot does not establish that the AP flag was false. The old parser
already used the correct lock attribute, verified against ESP-IDF and Reaver.
The corrected behavior is fresh confirmation, bounded waiting, clearer evidence
and refreshed UI status, rather than immediately terminating on one packet.
The user reported that v0.5 verified their known PIN successfully; v0.6 has not
been independently tested on a physical device. Both firmware and FAP must be
updated because the target/event structure now includes WPS presence.

## Changes in 0.5

Replaces the direct private-callback bridge with ESP-IDF 5.4.1's actual WPS
initialization, task, selected-BSSID scan, association and teardown flow.
A source adapter directs that flow to the selected router and delivers EAPOL
to the existing external-registrar engine. Uses SDK-built registrar probe and
association IEs; corrects connection retry configuration before applying it.
The external worker owns EAP timeouts; the enrollee's two-second message timer
is not armed for these sessions. Cancellation stops discovery before teardown.

Logs show driver scan count, matching router observations, queued connections,
WPS readiness and EAP RX/TX. This distinguishes failure to find the router from
failure to associate and failure to exchange EAP. The WIFITE icon, cyan WPS
labels, native Pixie modes, known-PIN verification and bounded PIN search remain.

The source adapter checks the SHA-256 of upstream esp_wps.c and generates a
modified translation unit inside the build directory. It does not rewrite that
file in the SDK installation. See tools/wifite_sdk_adapter.py and
cmake/wifite_sdk_adapter.cmake. The project's pre-existing raw-TX library patch
for other firmware tools is unchanged.

This is an experimental build with host verification, not a complete Kali
Wifite port. The v0.4 known-PIN failure is real; v0.5 still needs a physical
router/device test. Do not present compilation or mocked tests as radio success.

## Changes in 0.4

- Turns passive sniffing off before every connection and checks that it is off.
  Restores passive RX before the next beacon/lock check, including after an
  offline Pixie search. Applies the WPS mode before the passwordless station
  configuration to match the SDK's initialization order.
- Logs connection events, channel/signal, named disconnect reasons, WPS readiness
  and EAP receive/transmit counts. A connection failure is explicitly distinguished
  from PIN verification. The screenshot's reason 2 is AUTH_EXPIRE; it does not
  establish an incorrect PIN or a successful online exchange.
- Fixes the launcher's cached missing-icon result when a replacement FAP has
  the same file size. Shows the Wi-Fi/W icon in the application header too.
- Marks discovered WPS routers with cyan [WPS+] log lines and WPS badges in the
  selection list. Amber badges mean an advertised lock; U means unlocked and
  ? means unknown. The scan lists WPS-advertising routers only.
- Stops on a session-matching WSC setup-locked response. Existing lock and PIN
  retry limits remain in place; transport errors do not advance the PIN.

These are code fixes and diagnostics, not confirmation that every router will
complete an exchange. Version 0.4 still requires physical validation.

## Changes in 0.3

Replaces the raw authentication/association path that timed out in 0.2 with
ESP-IDF driver-managed WPS association and EAPOL transport. Logs report actual
EAP Identity and WPS M1–M7 processing, named timeout stages, receive counters
and driver disconnect reasons. Adds a custom WIFITE launcher icon.
The driver bridge is pinned to ESP-IDF 5.4.1. Both firmware and FAP must be rebuilt.
This new transport has compiled but has not been verified on hardware.

## Install

1. Extract the release ZIP. Use its `FLASH.ps1` or `flash.sh` to flash all four
   supplied images. The new FAP requires this firmware's new native API; copying
   the FAP onto pi4 alone will not work.
2. Merge the contents of `SD/` into the SD card root. The new file belongs at
   `apps/WiFi/wifite.fap`. Retain your existing SD files and data.
3. Open **Apps → WiFi → WIFITE**. The displayed name is **WIFITE**.

Windows, with the actual port substituted for COM4:

```powershell
py -m pip install esptool==4.12.0
powershell -ExecutionPolicy Bypass -File .\FLASH.ps1 -Port COM4
```

Linux:

```sh
python3 -m pip install esptool==4.12.0
bash flash.sh /dev/ttyACM0
```

The flash scripts retain their existing behavior. They write these offsets
without erasing NVS. Do not write the application at address zero.

| Image in firmware/ | Offset |
| --- | --- |
| bootloader.bin | 0x0 |
| partition-table.bin | 0x8000 |
| furi_esp32.bin | 0x10000 |
| ota_data_initial.bin | 0xbf0000 |

Active SDK configuration and partition layout match the supplied pi4 build.
The existing bundled apps, ProtoPirate plugins, local video demo, converter and
companion files are retained. Companion files are unrelated to WIFITE.

## Workflow and controls

Choose **Scan networks**. The live router table updates during scanning.
After it finishes, select a WPS-advertising AP you are authorized to test.
Hold OK on a row for details. Networks without observed WPS show details only.
The Home menu retains logs, font size, SD export and About controls.

| Action | Behavior |
| --- | --- |
| Auto test | Try 12345670 first; stop on success. After rejection or an inconclusive exchange, reinitialize the radio and try Pixie capture. Run the offline search only with valid M3 data. If capture fails or the search finds no match, start/resume bounded online PIN testing. Verify any recovered Pixie PIN in a fresh session. |
| Test one PIN | Enter eight digits (prefilled with 12345670), then hold OK to verify that one PIN. |

Confirmed locks, cancellation, unavailable targets and initialization errors
stop the sequence. In the online PIN stage, three consecutive inconclusive
exchanges stop without advancing past the unverified PIN.
Neither WPS support nor a nonzero client count guarantees a vulnerable router.
WPS communicates directly with the AP; an associated client is not required.

Rotate to select. Press OK to open. During a job, logs automatically follow new
lines. The live table highlights WPS advertisements in cyan and PIN locks in amber. Rotate to scroll through the last 256 wrapped lines; press OK to return
to live output. Back requests stop, waits for resource cleanup, then another
Back returns to the menu. The font setting applies to the next job.

**Save last log to SD** exports that retained terminal history to
`apps_data/wifite/log-<tick>-<suffix>.txt`. Results appear in the same terminal.
Saved logs may contain the recovered PIN and network key. Nonprintable bytes are
rendered as `?`; this is not a binary credential export.

## Search scope

The two-choice UI uses the standard Pixie search. The full-search native API
remains compiled for compatibility, but has no separate action in this UI.

This is a native implementation of the requested **WPS workflow**, inspired by
Wifite. It is not the Kali Wifite Python application or the Linux Reaver binary.
No shell commands or simulated attack results are used.

Pixie uses fresh captured public keys, nonces, E-Hashes and a derived AuthKey.
It checks both E-Hashes before reporting a recovered PIN. The PRNG routines
derive from Pixiewps; attribution and licensing are in
`components/wifi/wifite/UPSTREAM.md` and `COPYING.pixiewps`.

| Family | Standard | Full |
| --- | --- | --- |
| Zero secret nonces and reused public nonce | Yes | Yes |
| 1: Ralink LFSR reconstruction | Yes | Yes |
| 2: eCos simple | 25-bit remaining-state search | Same |
| 3: RTL/glibc | AP public-nonce seed within ±1 day of device time | Entire 32-bit public-nonce seed space |
| 4: eCos simplest | All 256 low-byte streams, covering the observable 32-bit seed streams | Same |
| 5: eCos Knuth | No | Entire 32-bit seed space |

RTL secret-nonce timing uses ±600 seconds for E-S1 and offsets 0–9 seconds for
E-S2. Correct device time matters for standard RTL search. Full searches may
take hours or days on an ESP32; runtime has not been measured. Searches are
cooperatively cancellable. Full offline searches do not have SD resume.
Offline PIN checking includes the empty PIN and non-checksummed numeric second
halves. Online brute force covers the standard 10,000 first halves plus 1,000
checksum-bearing second halves once the first half is established. Vendor PIN
dictionaries, nonstandard online PIN spaces and every Linux CLI option are not
implemented.

Brute force advances only on a selected-AP WSC NACK with matching session nonces
and the Device Password Authentication Failure code at the expected stage.
Timeouts, EAP failure and transport errors are inconclusive and retry the same
PIN. Three inconclusive attempts stop the job. Advertised lockouts between attempts enter a bounded passive wait; session-matching WSC setup-locked
responses stop it. Sniffing is disabled during an active connection, so beacon
lock updates are checked again before the next attempt.
There is a two-second inter-attempt delay; no lockout bypass is attempted.
A passive lock wait lasts at most 30 seconds per status check.
If Auto capture fails without a lock or cancellation, it moves to the bounded
online PIN stage and reports that no offline search ran. Confirmed AP locks
and cancellation stop the sequence.

Checkpoints are saved by BSSID under `apps_data/wifite/bf-<BSSID>.txt`, every ten
attempts in the first phase, each second-phase advance and on a normal stop.
Resume validates their bounds. Abrupt power loss can lose the latest checkpoint;
an invalid file is ignored. Delete that AP's checkpoint to restart from zero.
PIN/AP configuration changes may invalidate saved progress.

## Protocol and platform limits

- The internal ESP32-S3 2.4 GHz radio is used. No 5 GHz support is added.
- WSC/EAP fragmentation is reassembled up to 1,400 bytes. Oversized messages and
  malformed fragments are rejected. The driver handles outgoing 802.11 framing.
- Hidden SSIDs and an M1 MAC address differing from the selected BSSID are
  rejected. APs requiring unsupported registration behavior may fail.
- Capture ends at authenticated M3. Verification reads authenticated/decrypted
  M7 settings. It does not send M8 or reconfigure the AP.
- Each exchange is bounded to 45 seconds, with up to 20 seconds for association
  and step retransmissions. Driver TX
  acceptance means queued, not proof that a router received or acknowledged it.
- The app temporarily owns Wi-Fi and uses the existing firmware radio lease to
  restore prior Wi-Fi/Bluetooth state on exit. Hardware recovery behavior still
  needs validation.
- Scan capacity is 64 APs and 128 observed AP/client pairs. Client counts are
  passive observations, not a definitive association table.
- WPA handshake cracking, PMKID, WEP and the other Kali Wifite attack families
  are outside this app's WPS scope. `NO MATCH` does not establish that an AP is
  secure, and WPS being advertised does not establish vulnerability.

## Validation and first hardware check

`VALIDATION.txt` in the release records actual build and host checks. Native
tests use independent Python cryptographic fixtures, GCC AddressSanitizer and
UndefinedBehaviorSanitizer. They exercise WPS M1–M7, DH/KDF/HMAC/AES, all five
PRNG families, integrity rejection, split-PIN progression, selected-AP framing,
fragment retransmission and malformed-input parsing. They do not execute the
ESP32 worker, exercise the physical display, or transmit Wi-Fi packets.

For the first hardware check, scan a controlled AP and use **Verify one supplied
PIN** with its known PIN. The log should show actual authentication, association,
EAP Identity, M1/M3/M5/M7 and a validated result. If it stops at TX, association
or EAP, save the terminal log: that is a radio/protocol compatibility failure,
not evidence of an incorrect PIN. Check that Back stops cleanly and normal Wi-Fi
returns. Only after that exchange works can the attack paths be meaningfully
validated on the hardware. No physical success is claimed in this release.

## Build the matching source

The source ZIP contains the complete modified project based on the supplied
`merged-firmware-source.zip`. Start with `BUILD-WIFITE.md` for source-only builds. Use an ESP-IDF 5.4.1 checkout with
submodules and its installed ESP32-S3 tools. Tested versions: Xtensa GCC
14.2.0_20241119, idf-component-manager 2.2.2, CMake 4.4.4 and Ninja 1.13.2.

```sh
. /path/to/esp-idf/export.sh
python -m pip install idf-component-manager==2.2.2
cd Flipper-Zero-ESP32-Port
idf.py -B build_wifite -D SDKCONFIG=sdkconfig.wifite \
  -D FLIPPER_BOARD=lilygo_t_embed_cc1101 build
FAP_FW_BUILD_DIR=build_wifite FAP_APP_VERSION_OVERRIDE=8 \
  bash buildFap.sh applications_user/wifite
```

The existing build patches the ESP-IDF Wi-Fi library's raw TX sanity check.
That existing step remains for other firmware tools. WIFITE 0.3 uses
driver-managed association and Ethernet EAPOL rather than raw frame injection;
it does not add a new binary patch.
The SDK is a separate build dependency, not bundled in the source ZIP.

Host tests require GCC, Python `cryptography`, and the Mbed TLS crypto library
from that same SDK built for the host:

```sh
cmake -S /path/to/esp-idf/components/mbedtls/mbedtls -B host-mbedtls \
  -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF -DMBEDTLS_FATAL_WARNINGS=OFF
cmake --build host-mbedtls -j4
MBEDTLS_ROOT=/path/to/esp-idf/components/mbedtls/mbedtls \
MBEDTLS_LIB="$PWD/host-mbedtls/library/libmbedcrypto.a" \
python3 tests/wifite/test_core.py
```

Connection-lifecycle regression test (mocked driver; no RF):

```sh
python3 tests/wifite/test_link.py
```

This test covers sniffer separation, WPS/config ordering, repeated sessions,
failure propagation and callback ownership. Address/undefined-behavior checks
are enabled; LeakSanitizer is disabled for container compatibility.

Generated SDK adapter regression tests (no RF):

```sh
IDF_PATH=/path/to/esp-idf python3 tests/wifite/test_sdk_adapter.py
```

Worker orchestration and scan/status regressions (actual functions, mocked I/O):

```sh
MBEDTLS_ROOT=/path/to/esp-idf/components/mbedtls/mbedtls \
MBEDTLS_LIB=/path/to/host-mbedtls/library/libmbedcrypto.a \
python3 tests/wifite/test_workflow.py
```

These cover the default-PIN-first ordering and success short-circuit, automatic
fallback, error/lock/cancel stops, stale and duplicate status packets, temporary
locks clearing, persistent locks, unknown flags, and late client traffic.
