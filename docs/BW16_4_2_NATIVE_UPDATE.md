# BW16 native selection update: FAP 4.2

This update consists of a **new source-built BW16 controller firmware** and the
**4.2 T-Embed FAP**. The T-Embed system firmware does not need flashing.
The BW16 image implements the R4TKN v4 UART command protocol, with an additional
native selected-target operation. It is not an upstream R4TKN release or an exact
rebuild of its unavailable application source.

## Why this changes band switching

The previous FAP used about 1.5 seconds on one selected radio, sent STOP, waited
for its acknowledgement and UART quiet, then started another radio. The new FAP
sends the complete selected set once. BW16 spends 100 ms on each selected entry
in sequence, with a small bounded burst at each visit. A full rotation takes
approximately the selected entry count times 100 ms, plus radio/loop latency. UART carries
start/status/stop messages rather than controlling every band change.

BW16 still operates one channel at a time. Faster rotation reduces the gap but
does not guarantee that a client remains disconnected. Protected management
frames, reconnection and roaming behavior are unchanged. Both device and FAP
retain independent 30-second limits, and Back requests STOP for the whole set.

## Install

1. Exit the BW16 app on the T-Embed. Replace SD `apps/GPIO/bw16_r4tkn.fap` with
   the copy under this package's `SD/apps/GPIO/` directory.
2. Disconnect BW16 from the T-Embed and connect only its USB cable to the PC.
3. From this package's `BW16` directory run, using your actual port:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\FLASH_BW16.ps1 -Port COM3
   ```

4. Enter upload mode: hold BURN, press/release RST, then release BURN. Wait for
   checksum verification and SUCCESS. Press RST afterward.
5. Unplug BW16 USB and reconnect the working four-wire cable below.

The uploader defaults to 460800 baud. `-BaudRate 115200` is available if needed.
`-CheckOnly` verifies the package without opening a port.

| T-Embed wire | BW16 |
| --- | --- |
| Yellow / RX GPIO44 | PA7 / TX0 |
| White / TX GPIO43 | PB2 / RX1 |
| 3.3 V | 3V3 |
| GND | GND |

GPS and BW16 use the header separately. Close the app before swapping them.

## Use

Scan networks, pair your differently named 2.4/5 GHz entries using **Pair Wi-Fi
names**, mark the desired groups in **Select networks**, then choose **Deauth
selected**. Rotate through the review to check SSID, BSSID, band and channel.
Hold OK to start. Short OK or Back stops the entire operation.

Pairing/selection still resets on a rescan or list replacement. Identical raw
SSIDs automatically share a group; different names require explicit pairing.
Only the selected entries from that scan are used. The app checks the native
capability when connecting; with older BW16 firmware it reports that a BW16
update is needed for multiple selected radios. Legacy single station/client,
scan/list and all-cached options remain available.

TX counts are attempts, not confirmation that a client disconnected. Selected
AP status is sampled periodically; the displayed entry is the last reported
entry rather than a real-time view of every 100 ms change.

## Restore the previous working setup

From `BW16`, use:

```powershell
powershell -ExecutionPolicy Bypass -File .\FLASH_BW16.ps1 -Port COM3 -RestoreWorking
```

This restores the tested **R4TKN v4 TX0-fix** image and its boot files. Also restore
`rollback/apps/GPIO/bw16_r4tkn.fap` to SD `apps/GPIO/`. Your TX0 wiring stays the same.
Original 4.0/4.1 release directories and firmware files are preserved separately.

## Source comparison and verification

[Spectra Mini's pinned public repository](https://github.com/rusyln/Spectra-Mini-BW16/tree/09a56e57839ed6563004346484a41ad1b7e2175c)
contains a README and screenshot, not its firmware implementation.
[SpectraReaper's source](https://github.com/wirebits/SpectraReaper/blob/2727a1f076f5a566d72f82b6b6b2ed3c769218df/SpectraReaper/SpectraReaper.ino#L189-L279)
keeps selected indices on BW16 and advances through them in its device loop.
That architectural distinction guided this update. Its older SDK-specific raw
packet helper is not a drop-in match for the installed SDK.

The new bridge pins Realtek AmebaD 3.1.9 and includes the matching GPL-licensed
packet helper with attribution under `source/firmware/bw16_controller/`.
Host tests exercise the actual portable firmware core and actual FAP controller,
including protocol interoperability, explicit selection validation, rotation,
STOP and independent deadlines, cancellation, malformed commands and UI layout.
The FAP's imports are checked against the installed T-Embed firmware ELF.

No COM port was opened and no radio traffic was sent while building/testing this
release. Physical boot, UART pins, scan/client behavior and multi-band radio
results require hardware validation. The prior working image is included for
rollback. `BUILD_INFO.json` records exact source/artifact hashes and limitations.
