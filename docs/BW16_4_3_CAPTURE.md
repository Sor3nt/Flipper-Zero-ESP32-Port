# BW16 handshake capture: 4.3

This update adds passive EAPOL capture to the BW16 controller and saves standard
`.pcap` files on the T-Embed SD card. The existing T-Embed firmware, Wardriver,
RF Waterfall/spectrogram, Spectrum Analyzer, and 4.2 radio controls are retained.
The saved `releases/T-Embed-and-BW16-4.2` working package is preserved separately.

## Install

Flash the new BW16 firmware and copy the new FAP. The T-Embed system firmware
does not need another flash if it already runs the working 4.2 setup.

1. Exit the BW16 app. Copy `T-Embed/SD/` contents from this release onto the SD
   card, preserving your existing settings and logs.
2. Disconnect BW16 from T-Embed and connect only its USB cable to the PC.
3. In the release's `BW16/` folder, run with your actual BW16 port:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\FLASH_BW16.ps1 -Port COM3
   ```

4. Hold BURN, press/release RST, release BURN. Wait for verified SUCCESS.
   Press RST, unplug USB, and reconnect the working four-wire cable.

Wiring stays the same: yellow RX44 to BW16 PA7/TX0; white TX43 to BW16 PB2/RX1;
3.3 V and GND. GPS and BW16 use the shared header at different times.

## Capture

Open **Capture handshake**, choose one AP from the indexed network list, and
hold OK on the review screen to start. The receiver stays on that AP's channel. Connect or reconnect
one of your devices normally while capture is running. Stop with OK or Back.
Capture also stops at its two-minute deadline or packet limit.

Files are created without overwriting previous captures under:

```text
apps_data/bw16/captures/capture_0001.pcap
```

The screen reports EAPOL frames actually saved and capture errors/drops. A PCAP
with zero EAPOL frames contains no captured handshake; even several EAPOL frames
do not prove that one complete exchange was captured. Open the file in Wireshark
to inspect it. Retries and partial exchanges are preserved as observed.

Capture is a separate passive operation. It does not trigger deauthentication,
and it does not hop between paired bands. Existing radio controls are separate
menu options. Older BW16 firmware keeps its existing controls but needs this
firmware update to provide PCAP capture.

## What is recorded

Only frames matching the selected AP's BSSID are accepted: a real beacon when
available, plus valid EAPOL traffic. No synthetic handshake or beacon is created.
Records preserve the actual frame bytes supplied by the driver. Unsupported,
malformed or oversized packets are rejected and bounded queues report drops.

The file uses classic PCAP 2.4, raw IEEE 802.11 linktype 105, and a 1600-byte
snapshot limit. Timestamps combine the T-Embed's start-time clock with BW16
software receive timestamps. They are not hardware radio timestamps.
The device's installed driver accepts receive descriptors up to 1578 bytes and
cannot be assumed to decode every modern AP/client PHY mode. Capture success
therefore needs a real test with the connected board and selected network.

UART stays at 115200. Filtered packets are framed with a capture ID, sequence,
relative receive time, length and CRC32 covering metadata and payload. Corrupt
records are not written as valid PCAP packets. SD open/write/sync/close failures
are reported; an error can leave a partial file for inspection.

## Validation and sources

The release checks portable packet filtering, UART framing/CRC, PCAP fields,
controller state/deadlines and the actual app's storage/error paths using host
stubs. Both firmware builds and the FAP's linked ABI are checked. These checks
do not establish hardware capture success. No COM port is opened during builds.

Format references: [libpcap savefile format](https://github.com/the-tcpdump-group/libpcap/blob/master/pcap-savefile.manfile.in),
[IANA linktype registry](https://www.iana.org/assignments/pcap), and
[Wireshark EAPOL dissector](https://github.com/wireshark/wireshark/blob/master/epan/dissectors/packet-eapol.c).
The installed Realtek AmebaD 3.1.9 `wifi_conf.h` documents promiscuous mode 3
(`RTW_PROMISC_ENABLE_2`) as raw 802.11 reception. The matching driver library and
source hashes are recorded in `BUILD_INFO.json`.
