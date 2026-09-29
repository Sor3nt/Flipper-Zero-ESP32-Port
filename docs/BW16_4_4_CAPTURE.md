# BW16 capture update: 4.4

This release adds **Capture + deauth** alongside the existing passive **Capture
handshake** option. Both save `.pcap` files to the T-Embed SD card. The working
T-Embed system firmware, Wardriver, RF Waterfall/spectrogram and other SD apps
are retained. The separate 4.2 and 4.3 bundles are preserved.

## Install

Only the BW16 firmware and BW16 FAP need updating. There is no need to flash the
T-Embed again if it already runs the working firmware.

1. Exit the BW16 app and copy `T-Embed/SD/` contents onto the SD card. The updated
   app is `apps/GPIO/bw16_r4tkn.fap`.
2. Disconnect BW16 from T-Embed and connect BW16 USB to the PC.
3. From this release's `BW16` folder, use your actual BW16 port:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\FLASH_BW16.ps1 -Port COM3 -BaudRate 460800
   ```

4. Enter upload mode: hold BURN, press/release RST, then release BURN. After
   verified SUCCESS, press RST, unplug USB and reconnect the four-wire cable.

Wiring is unchanged: yellow T-Embed RX44 to BW16 PA7/TX0; white T-Embed TX43 to
BW16 PB2/RX1; 3.3 V and GND. GPS and BW16 use the shared header at different times.

The `T-Embed` folder includes the complete working firmware and `FLASH.ps1` for
restoration. If restoring it, enter download mode by holding the encoder centre
(BOOT), pressing/releasing rear RST, then releasing BOOT, and use the newly
listed COM port. See [LilyGO's download procedure](https://github.com/Xinyuan-LilyGO/T-Embed-CC1101/blob/master/docs/flash_download_tool.md).

## Capture with a reconnect attempt

Open **Capture + deauth**, select one AP and hold OK on its review screen.
The app creates and syncs a new PCAP file before issuing any radio command.
The BW16 then:

1. Tunes to that AP's channel and arms the receiver.
2. Sends one brief burst of up to three deauth frame submissions for that AP.
3. Keeps listening for the reconnect until you stop, two minutes elapse or the
   256-packet capture limit is reached.

The receiver stays armed during the burst so there is no deliberate gap before
listening. There are no repeated bursts or channel hops during this operation.
The burst is limited to the first second after starting and can be cancelled
with STOP. An SD preflight failure or receiver startup failure prevents it.

The displayed TX attempts count radio submissions, not confirmed delivery or
client disconnection. Saved EAPOL frames are counted separately; neither count
proves a complete handshake. An AP/client that rejects the deauth frames may
remain connected. You can reconnect your own client normally while listening.

The selected AP means one BSSID and channel. A client that reconnects to the
other band is outside this capture. Existing paired-band deauth controls remain
separate menu options.

## Passive capture and files

**Capture handshake** retains passive capture and sends no deauth frames.
Connect or reconnect your own client normally while it is listening.
Stop either capture with OK or Back; Right shows the file path.
Files are created without overwriting earlier captures:

```text
apps_data/bw16/captures/capture_0001.pcap
```

Only the first actual matching beacon and matching EAPOL frames are saved.
Partial exchanges and retries are preserved. A file with zero EAPOL frames
contains no captured handshake. Inspect the PCAP in Wireshark to determine
whether a complete exchange was captured. SD or UART failures are reported and
may leave a partial file for inspection.

The format remains classic PCAP 2.4, raw IEEE 802.11 linktype 105, with a 1600-byte
snapshot limit. Timestamps combine the T-Embed start-time clock with BW16
software receive timestamps. UART is unchanged at 115200, with a capture ID,
sequence and CRC32 protecting each record. The installed driver accepts raw
receive descriptors up to 1578 bytes; hardware capture success remains dependent
on the board, selected network and radio mode.

## Compatibility and validation

The new combined option requires the 4.4 BW16 capability. With 4.3 BW16 firmware,
passive capture remains available and the new option requests an update. Older
radio controls keep their existing protocol. Capture IDs prevent stale session
markers from updating a new capture.

Host tests cover receive-before-transmit ordering, a single bounded burst,
STOP and startup failures, passive zero-TX behavior, SD persistence, corrupt or
stale UART records, and the rendered UI. The source-built BW16 binary, final FAP
ABI and complete archive checksums are checked during release. These checks do
not verify radio performance on hardware. No device is flashed by the build.

Source snapshots and tests are included under `source/` for use within the full
project checkout; this snapshot is not a standalone copy of the ESP-IDF project.

Format references: [libpcap](https://github.com/the-tcpdump-group/libpcap/blob/master/pcap-savefile.manfile.in),
[IANA linktypes](https://www.iana.org/assignments/pcap), and
[Wireshark EAPOL dissector](https://github.com/wireshark/wireshark/blob/master/epan/dissectors/packet-eapol.c).
The exact AmebaD 3.1.9 driver and firmware source hashes are recorded in the
release manifest.
