# BW16 R4TKN controller 4.4 for T-Embed

Native ESP32-S3 FAP for the owner's BW16-Kit (RTL8720DN). The T-Embed supplies
the display and controls; BW16 performs the radio operations. Version 4.4 adds a separate Capture + deauth option: it arms the selected
AP capture and sends one brief three-frame deauthentication burst. Passive
beacon/EAPOL capture and 4.2 selected-radio scheduling remain available. Legacy R4TKN **FLIPPER_ZERO v4** commands remain supported.

The companion image is built from the source in
[`firmware/bw16_controller`](../../firmware/bw16_controller/) and implements the
v4 UART protocol plus native selection. It is not an upstream R4TKN v4 release
or an exact rebuild of the unavailable upstream application source.

## Installation and wiring

On the already working T-Embed setup, replace `apps/GPIO/bw16_r4tkn.fap`
on the SD card with the 4.4 FAP, and flash the companion 4.4 controller image to
BW16 for passive capture and native multiple-radio operation. Follow the
[4.4 capture and installation guide](../../docs/BW16_4_4_CAPTURE.md)
for the supplied uploader, verification and recovery files. Flash BW16 with its
USB cable connected to the PC and its T-Embed wires disconnected.

The T-Embed system firmware does not need flashing. Open Apps -> GPIO ->
BW16 R4TKN after reconnecting the cable below. The existing external-UART
firmware profile remains required. The FAP does not flash BW16 itself.
With older BW16 firmware, selecting multiple radios shows **BW16 update needed**;
legacy single station/client, scan/list and all-cached operations remain available.

| T-Embed UART header | BW16-Kit |
| --- | --- |
| 3.3 V | 3V3 |
| GND | GND |
| TX / GPIO43 (white) | PB2_RX1 |
| RX / GPIO44 (yellow) | PA7_LOG_TX0 |

This is the owner's tested wiring with the R4TKN v4 TX0-fix image. The new
controller uses the same pins: commands enter PB2/RX1 and replies leave PA7/TX0.
Color labels refer to this cable. The original
[TX0-fix details](../../docs/BW16_TX0_FIX.md) remain available for recovery.
In this four-wire configuration BW16 USB is disconnected. The owner confirms
stable 3.3 V power. Physical operation of the new source-built image still needs
hardware validation.

GPS and BW16 are never connected simultaneously. Exit Wardriver before swapping
to BW16; exit BW16 before swapping back. GPS configuration, NMEA parsing,
logging, uploads and saved SD files are unchanged.

This personal firmware reserves GPIO43/44 for external UART. It contains no NRF
probe, guard, mode selector or SPI operation in the BW16 path. It is intended
for the owner's electrically NRF-free header.

## Controls and UART feature map

| FAP option | UART command | Result |
| --- | --- | --- |
| Scan networks | SCAN, then DEAUTH_STATION without arguments | Fresh scan followed by the cached indexed list for selection |
| Select networks | None with a valid list | Toggle multiple network groups; identical SSIDs include all discovered bands |
| Pair Wi-Fi names | None with a valid list | Explicitly join differently named entries for this scan |
| Deauth selected | DEAUTH_SELECTED mask; DEAUTH_STATION AP for a single radio | BW16 schedules only the selected indexed radios for up to 30 seconds total |
| Station list | DEAUTH_STATION without arguments | Cached indexed AP list; v4 scans first if its cache is empty |
| Capture handshake | CAPTURE AP SESSION; STOP | Passive fixed-channel beacon/EAPOL PCAP to a unique SD file, up to 120 seconds or 256 packets |
| Capture + deauth | CAPTURE_DEAUTH AP SESSION; STOP | Arms capture, then attempts one three-frame deauth burst on the selected AP; same PCAP and limits |
| Observe clients | LIST_CLIENTS AP | Five-second passive observation of the chosen AP's peers |
| Deauth station | DEAUTH_STATION AP | Chosen station operation |
| Deauth client | DEAUTH_CLIENT AP CLIENT | Chosen client operation |
| Deauth ALL scanned | DEAUTH_ALL | All entries in the BW16 scan cache |
| OK / Back on an active operation | STOP | Wait for the matching stopped/done response |
| UART details | None | Received bytes/lines, first bytes, last command/reply, elapsed time |
| Firmware / wiring | None | Native-selection support, UART settings and limitations |

Rotate to select; short OK chooses an entry. Select a station/client before
starting its operation. The confirmation screen names the selection; hold OK
to start. Short presses and repeat events cannot trigger confirmation.
Operations request STOP after 30 seconds, or immediately on OK/Back. STOP must
be acknowledged before the app reports that the device stopped. Counts are
reported transmission attempts/cycles, not proof of an effect on a network.

Network/client order is preserved exactly because UART commands use array indices.
No sorting/deduplication changes those indices. Lists are bounded to 50 APs and
32 clients. Partial lists show a plus marker; all-AP operation requires the full
list. The grouped flow fetches an indexed list after each fresh scan. Legacy
station/client/all operations request a fresh indexed list. Replacing the list
clears selections and manual links so old array indices cannot be reused. Client observations do not prove association or identity.

## Multiple networks and both bands

1. Choose **Scan networks** and wait for scanning and indexing to finish.
2. Rotate through **Select networks**, then short OK to mark/unmark groups.
   A group with exactly the same raw SSID includes all of its discovered radios,
   including both 2.4 GHz and 5 GHz. Hidden entries stay separate.
3. For different SSIDs, choose **Pair Wi-Fi names**, select the first name with
   OK, rotate to its other-band name, then OK again. This joins those groups for
   the current scan. Return to Select networks and mark the linked group.
   Manual links and marks reset on a rescan/list replacement or app close.
4. Hold OK in Select networks, or choose **Deauth selected**, to review every
   selected radio. Rotate to inspect its SSID, BSSID, band and channel.
5. Hold OK on the review screen to start. OK or Back requests STOP for the
   entire batch. The app waits for acknowledgement before declaring a stop.

A 5 GHz name such as `LAB_0305_5G` does not prove which differently named
2.4 GHz entry belongs to it. Pair the two entries explicitly; the app does not
infer names by stripping suffixes. Only radios present in the returned indexed
list can be selected. Pairing preserves individual marks; mark the joined group
to include all members. `[x]` means every member is selected, `[-]` means some,
and `[ ]` means none.

For two or more selected radios, the FAP sends the complete selection once as a
13-digit hexadecimal mask. BW16 handles one channel at a time and visits **one
selected entry per 100 ms visit**, with a bounded burst during that visit. A full
rotation through N entries takes approximately **N * 100 ms plus radio processing
time**. Each entry therefore waits for the other selected entries before its next
visit. The native command never expands a subset into `DEAUTH_ALL`.

`SELECTED_START` confirms the operation, `TX:` reports aggregate transmission
attempts, and `SELECTED_DONE` confirms its end. A single STOP request ends the
whole set. Both BW16 and the FAP enforce a 30-second limit; start/stop errors end
the session. The displayed band/channel follows the latest periodic
`SELECTED_AP` report, so it does not show every 100 ms change in real time.
One selected radio uses the compatible `DEAUTH_STATION` operation. Rotation does
not guarantee that a client cannot reconnect or switch bands.

An animated loading indicator is shown during connection/capability checking,
scan/index retrieval, client observation and start/stop acknowledgement.

## Passive capture

Choose **Capture handshake**, select one indexed AP, review its BSSID/channel and
hold OK to start. Reconnect your own client manually while the capture waits.
The operation stays on that AP's channel for up to 120 seconds or 256 saved
packets. It does not trigger deauthentication or hop through groups.

The app creates a unique file such as
`/ext/apps_data/bw16/captures/capture_0001.pcap` before sending CAPTURE.
**EAPOL: N** counts EAPOL frames successfully written and synced to SD; it does
not assert a complete handshake. The file contains the selected AP's beacon
and EAPOL frames. Right opens the full path. Back/OK requests STOP; the file
stays open while queued packets drain, then closes after DONE or a fault.
Failed writes, syncs, or closes preserve the partial file and never say Saved.
Timestamps use the T-Embed RTC sampled just before CAPTURE plus BW16's software
relative arrival time; these are approximate, not hardware radio timestamps.

Capture requires `BW16_PCAP:1` before the normal `BW16_CAPS:1` response.
Older images retain all their existing options and show a capture update prompt.
See the [4.3 guide](../../docs/BW16_4_3_CAPTURE.md) for format and hardware limits.

## Capture with one deauthentication burst

**Capture + deauth** is a separate menu option. Select one indexed AP and review
its BSSID and channel, then hold OK at the active-capture confirmation. SD file
creation and header sync must succeed before the app sends
`CAPTURE_DEAUTH AP SESSION`. BW16 arms RX first, reports START, then attempts
one burst of three frames (reason codes 1, 4 and 16). The burst does not repeat.
The capture remains on that AP and uses the same 120-second/256-packet bounds,
STOP drain, unique filenames, EAPOL counters and partial-file handling.

**TX attempts** displays BW16's reported count from 0 to 3, independently of
the number of EAPOL frames written. It does not verify a client disconnect or a
complete handshake. If a report is missing, the display says pending or unknown;
a failed SD preflight says capture not started. Right shows the saved path and
drop count. The original **Capture handshake** option stays passive.

The extra mode requires `BW16_PCAP_DEAUTH:1` during capability negotiation,
before the final `BW16_CAPS:1`. A 4.3 companion retains passive capture and
shows an update prompt for Capture + deauth. See the
[4.4 guide](../../docs/BW16_4_4_CAPTURE.md) for installation and limits.

Beacon flooding, random-SSID spam and NRF operations are not exposed by this
controller protocol.

## Connection and recovery

Start with BW16 idle/reset, then press OK on the connection screen. The app opens
UART2 at 115200, 8N1, no flow control; sends space + LF to finish a stale command;
and consumes startup text until 500 ms of quiet with a complete line boundary.
It then sends the read-only `BW16_CAPS` query. An exact `BW16_CAPS:1` response
enables native selection. If an older image does not reply within one second,
the app opens the menu with legacy support and no connection fault. A late or
unrecognized reply cannot enable native selection. A completed valid scan/list
establishes the indexed network snapshot used for operations.

The receive buffer is 8192 bytes. TX43/RX44 stay routed for the session.
The input buffer is flushed once before synchronization, never during a reply.
UART read/parse and controls run in the app task; the GUI task only draws a
snapshot and queues input. Scan/list deadlines use monotonic milliseconds,
independent of the RTOS tick rate.

Back exits from the main menu and releases the UART. After a timeout, corrupt
response, or cancelling a pending scan/list, reset BW16 before opening a new
session. These UART operations have no transaction identifiers or checksum,
so delayed responses cannot be safely attributed across abandoned requests.
The new capability query identifies native-selection support only.

If STOP is unconfirmed, the app says so and asks you to power BW16 off before
holding OK to exit. Four wires provide no independent reset/power switch.
Resource-cleanup failure retains the power lock and permits retry; an existing
GPS/RFID UART driver is never deleted or reconfigured by BW16.

## Firmware integration

`CONFIG_T_EMBED_EXTERNAL_UART=y` parks GPIO43/44 as pulled-up inputs during
application early initialization. The primary ESP-IDF console is USB Serial/JTAG,
so normal firmware/bootloader logs do not share the BW16 command wire.
No eFuses, ROM settings or hardware boot straps are changed. Booting/resetting
with BW16 attached remains part of hardware acceptance.

The FAP imports `furi_hal_external_uart_available`, which checks the running
firmware's board/profile/console selection. This makes the matching firmware a
real dependency, rather than relying on the FAP's compile-time configuration.
Existing firmware API entries and Wardriver compatibility are preserved.

## Protocol evidence

The original v4 compatibility reference is
[rusyln/flipper-zero-bw16-r4tkn](https://github.com/rusyln/flipper-zero-bw16-r4tkn),
commit `9325761a5b8ff5b827f4faa4bbdc4afd3ee91a69`, archive
`RTL8720DN-BW16/v4.zip`. Image `km0_km4_image2.bin` SHA256:
`995e34da08942e6edd9eae9f22bc55e9b0ffd193ac195e5c8f3cbbc2b25a8e2a`.

The archive supplies an AXF/map/disassembly rather than complete bridge source.
Inspection of the UART dispatcher (`loop`, 0x0e0008a8), station listing
(0x0e000230), targeted operation (0x0e000528), all-AP operation (0x0e000664)
and client handlers establishes the command map. Radio-library strings alone
are not evidence of exposed commands. The local prior controller supplied the
protocol/parser tests; the UART integration and connection workflow were replaced.

Version 4.2's native protocol and radio adapter are provided as source under
[`firmware/bw16_controller`](../../firmware/bw16_controller/). The
[4.2 update guide](../../docs/BW16_4_2_NATIVE_UPDATE.md) records the source comparison,
SDK version, build provenance, installation and recovery procedure. The upstream
archive hash above describes the original compatibility reference, not the new
source-built image.

## Verification

Run `python tests/bw16/run.py` (Windows: set ZIG_CC to zig.exe).
It executes production protocol/control/UART/UI code, including 100 swaps with
the unchanged production GPS driver, setup/cleanup faults, 20 app entry/exit
cycles, feature reachability, malformed/fragmented responses, real timeouts,
STOP handling, multi-selection/manual pairing, native selection masks, capability
negotiation and legacy timeout, selected-status validation, single-session
30-second deadlines, and native-font layout. The tests assert that multiple
selected radios use one start command rather than repeated host-side starts.

`python tests/bw16_native/run.py` exercises the portable BW16 firmware core and
its protocol interoperability; see the update guide for the release checks.

`python tests/bw16/check_abi.py` checks FAP imports/metadata/relocations against
the linked firmware ELF. Host tests do not establish radio performance,
power stability, physical cable continuity or device endurance.
