# BW16 R4TKN Scan (ESP32-S3 FAP)

Scan-only integration of SOR3NT PR #114, based on working revision `0e367fb`.
This app sends only `SCAN\n`. It displays SSID, channel, BSSID and RSSI. It does
not implement deauthentication, beacon flooding, client attacks or a stop command.
Back leaves the local scan screen; a BW16 scan already started may continue.

## Version 2: built-in NRF can remain fitted

**The wiring has changed from version 1 and PR #114. Do not use the old mapping.**

| T-Embed CC1101 Plus | BW16 peripheral UART |
| --- | --- |
| GPIO43, UART1 RX (also NRF CE) | TX1 |
| GPIO44, UART1 TX (also NRF CSN) | RX1 |
| GND | GND |

UART1 is 115200 baud, 8N1, with 3.3 V logic. Power the BW16 through its own
appropriate regulated input, such as its USB connector if fitted. Do not tie
separately powered supply rails together. RX1/TX1 are the evidenced R4TKN
board's peripheral UART, not necessarily its USB console. Exact BW16 pad numbers
require that module's pinout; BW15 and Five Ghost compatibility is not established.

The [LILYGO pin map](https://github.com/Xinyuan-LilyGO/T-Embed-CC1101/blob/master/docs/pinmap_cn.md)
and local board header assign CE=43, CSN=44, SCK=11, MOSI=9 and MISO=10.
The [Plus product](https://lilygo.cc/products/t-embed-cc1101-plus) includes NRF.
Qwiic GPIO8/18 serve the board's I2C devices; they are not assumed spare.

The original mapping put BW16 replies onto NRF CSN, selecting it during LCD,
SD or CC1101 SPI transfers. Releasing the ESP32 output driver alone did not solve
that. Version 2 reverses the UART mapping, keeping NRF CSN under host control:

- Probe NRF CONFIG/SETUP_AW and verify writable CONFIG with PWR_UP=0. NRF remains
  powered down throughout the session; incoming UART activity on CE cannot
  activate the radio in this state. A missing or unresponsive NRF fails setup.
- TX idle HIGH holds CSN inactive during BW16 replies.
- Sending `SCAN\n` briefly toggles CSN. Hold both the shared firmware SPI mutex
  and IDF's hardware bus arbiter until UART TX finishes, stopping SPI clocks
  throughout. The hardware lock also drains previously queued LCD DMA.
- On short write or TX timeout, disconnect the UART output route and force CSN
  HIGH before releasing SPI. A failed session cannot send again until reopened.
- On exit, park CE LOW/CSN HIGH, release UART and restore the saved pin state.
  NRF stays powered down; its normal app initializes it when opened again.

These rules follow the [Nordic nRF24L01 specification](https://devzone.nordicsemi.com/cfs-file/__key/support-attachments/beef5d1b77644c448dabff31668f3a47-aad1a46f307945a7b0204fd969e86bdf/content.pdf):
PWR_UP=0 disables the radio independently of CE, but SPI remains active. This is
why power-down AND the controlled-CSN/SPI-lock arrangement are required.
**The implementation has host tests, not electrical validation on your board.**
NRF clones, unexpected wiring and third-party apps bypassing resource ownership
remain unverified. Normal NRF operation and BW16 scanning are mutually exclusive.

Console output uses native USB Serial/JTAG. RFID still uses UART1 on 43/44 and
cannot run concurrently. Disconnect RFID/external IR from these pins. The
unrelated PR IR change was excluded; onboard IR remains GPIO2 TX / GPIO1 RX.
Existing native USB HID/MSC modes may take over the USB port as before.

## What "prepare/arm UART with BW16 unplugged" means

It means unplug **the BW16 TX1/RX1 signal wires**, not the built-in NRF.
At boot GPIO43 is driven LOW for NRF CE. A connected BW16 TX output could fight
that output. Software running later cannot protect this earlier boot interval.

1. Leave NRF fitted. Boot with BW16 TX1/RX1 disconnected from the T-Embed.
   Keep common ground connected if convenient. Do not power off or reset with
   BW16 TX attached. Automatic power-off must not occur during the session.
2. Open **BW16 Scan**, hold OK at **prepare pins**, and wait for **Now connect
   BW16**. This verifies NRF power-down, acquires ownership, releases the RX
   output driver and configures UART. If setup fails, leave BW16 disconnected.
3. Connect BW16 TX1 to **43**, RX1 to **44**, with shared GND and 3.3 V logic.
   Use a connector/switch that avoids accidental shorts.

4. Allow boot output to settle, then press OK to scan. **UART ready; unverified**
   means only local setup succeeded. A scan completes only after `SCAN_DONE`.
5. Up/Down browses results. OK scans again after at least 500 ms of serial quiet.
   A `+` after the AP count means additional results exceeded the 50-entry limit.
   SSIDs are stored up to 32 bytes; the display shows the first 20 characters and
   substitutes `?` for bytes outside its ASCII font. Hidden SSIDs are labeled.
6. Back shows **Disconnect BW16 wires**. Unplug both serial wires, then hold OK
   to restore the previous pin configuration and exit. Do not reset with BW16
   still connected. There is no automatic detection of physical isolation.

After any timeout, malformed record or RX loss, disconnect/reset the BW16 and
reopen the app. The protocol has no transaction identifiers; immediately retrying
an unsuccessful scan could mistake delayed old output for a new response.

## Protocol evidence and limits

Primary evidence: [rusyln/flipper-zero-bw16-r4tkn](https://github.com/rusyln/flipper-zero-bw16-r4tkn/tree/9325761a5b8ff5b827f4faa4bbdc4afd3ee91a69),
commit `9325761a5b8ff5b827f4faa4bbdc4afd3ee91a69`, especially
[`RTL8720DN-BW16/v4.zip`](https://github.com/rusyln/flipper-zero-bw16-r4tkn/blob/9325761a5b8ff5b827f4faa4bbdc4afd3ee91a69/RTL8720DN-BW16/v4.zip).
Its `km0_km4_image2.bin` SHA-256 is
`995e34da08942e6edd9eae9f22bc55e9b0ffd193ac195e5c8f3cbbc2b25a8e2a`.
The archive includes `application.axf`, `application.map` and `application.asm`.
The separate R4TKN-FIRMWARE-BW16 repository also exposes a flasher rather than
the required bridge source. No complete bridge source was found in these repos.

The v4 ARM Thumb code and symbols establish these scan details:

- `setup` at `0x0e000d28` calls `UARTClassTwo::begin` for `Serial1` with 115200
  and serial configuration argument 6. This integration uses the PR's 8N1 setting.
- `loop` at `0x0e0008a8` splits commands at LF, ignores CR and dispatches `SCAN`
  to `doScan` at `0x0e00009c` (call at `0x0e000964`).
- `doScan` emits **bare** `SSID,channel,12-hex-digit-BSSID,RSSI` lines to Serial1,
  followed by `SCAN_DONE`. The 12-digit MAC is constructed byte-by-byte in hex.
  Empty SSIDs become `HIDDEN NETWORK` in this image. Empty completed scans still
  emit `SCAN_DONE`; a module-side scan-start failure may emit no completion.
- The separate `doStationList` at `0x0e000230` has the format
  `list-station: %s,%u,%d` and `LIST_STATION_DONE`. Other image strings include
  `%s,%u,%s,%d` and `list-station:`. The parser accepts validated prefixed records
  too, but `LIST_STATION_DONE` alone does not complete a `SCAN` transaction.

The host tests use synthetic examples based on those formats, not captured RF
traffic. Commas inside SSIDs are supported by parsing fields from the right.
Logs are ignored, numeric fields are bounded, and MAC syntax is checked. There
is no handshake/version command that establishes the identity of your installed
image. Compatibility with your actual module, newer commercial R4TKN versions,
Five Ghost or other BW16 firmware is **unverified**.

The old README promised beacon spam and other absent features, contradicted its
own source, and described every received line as an AP. Those claims are removed.

## Ownership, failures and cleanup

- A cooperative firmware lease prevents this app, the built-in NRF app and the
  RFID reader from owning the shared pins concurrently. UART1 is also checked
  before installation, and installation must succeed before any configuration.
- Every UART/GPIO setup operation is checked. Failed setup unwinds only resources
  acquired by this session; an existing UART is never deleted. The app checks
  short writes and TX timeout; neither TX nor driver setup is scan success.
- UART RX uses a 4096-byte ring and 64-event driver queue. The main loop drains
  bounded chunks; no extra RX thread or copied-line queue can outlive the FAP.
  FIFO/buffer overflow, full event queue, parity/framing errors and corrupt or
  overlong lines invalidate the scan. Lines are discarded through the next LF.
- Scan timeout is 20 seconds. Input queue writes never wait for queue space;
  overflow is reported and Back is latched. Input, draw and main-loop state are
  protected by a mutex. GUI callbacks are detached before freeing queues/model.
- App, queue, mutex and viewport allocation failures are handled. Three existing
  Furi allocation helpers now return NULL instead of dereferencing it. Failure
  before a GUI can be allocated is reported through the USB log.
- GPIO mux, pulls, pin state, output routing/levels/enables, UART1 RX route and
  IDF reservation bits are saved/restored. Restoration requires BW16 unplugged.
  A paired insomnia hold prevents idle power management disrupting the session.
  Failed driver deletion retains ownership until a retry succeeds.
- This lease is cooperative. Old third-party NRF/GPIO FAPs and direct driver
  callers can bypass it; it is not a general hardware arbiter. Do not launch
  such a background app with BW16 connected.

## Build and ABI

Use the matching firmware and FAP together; the stock firmware lacks these new
exports. `firmware_api.c` is the runtime DJB2-hash table, searched in sorted
order. Merely including a header or `fap_libs` does not export a driver function.
All used UART functions plus GPIO configuration, pin-lease and guarded-NRF helpers are
explicitly exported, keeping the existing API major 1/minor 0. The loader checks
the major and target; it does not negotiate these additive symbols by minor.

`buildFap.sh` builds an Xtensa ELF32 relocatable FAP, with retained symbols and
relocations, long calls, embedded literals, target 32 and a 16384-byte stack.
It only **warns** about missing symbols, so `tests/bw16/check_abi.py` must pass:
it verifies the actual linked firmware table, hashes, addresses, imports and
FAP manifest. The app remains external; no APPS entry adds it to the firmware.

Build with ESP-IDF 5.4.1 using the firmware and FAP commands in the
[deployment guide](../../BW16-DEPLOYMENT.md). Board-specific defaults select the
USB Serial/JTAG console so UART0 does not drive GPIO43/44 during normal operation.
The BW16 signal wires still need to be disconnected during boot/reset.

Host checks: `python tests/bw16/run.py --zig <path-to-zig.exe>` on Windows, or
`sh tests/bw16/run.sh` with GCC/Clang on Linux (includes ASan/UBSan there).
See [deployment instructions](../../BW16-DEPLOYMENT.md) for artifacts, flashing
addresses, SD destination and the scan-only hardware checklist.
