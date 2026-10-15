> Release reference. For current source builds and confirmed wiring, see
> [the setup guide](../T-EMBED-BW16-SETUP.md) and [feature status](../T-EMBED-FEATURES.md).
> Fully Power Off with Lilygo USB removed before connecting BW16; Deep Sleep
> is insufficient. SD retries do not make hot-plugging reliable.

# T-Embed CC1101 Plus + BW16 I2C 4.5

This separate build keeps the NRF installed and moves BW16 communication to the
existing I2C port. It includes a newly compiled T-Embed system image, BW16 image,
and **BW16 I2C** FAP. The original non-NRF UART 4.4 bundle is preserved separately.
Both devices and the new FAP must be updated for this version to communicate.

## Wiring (power off both boards first)

Remove the previous BW16 UART connections from GPIO43/44. Use these four wires:

| T-Embed CC1101 Plus | BW16-KIT |
| --- | --- |
| 3.3 V | 3V3 |
| GND | GND |
| GPIO8 / SDA | PA26 / D8 / SDA |
| GPIO18 / SCL | PA25 / D7 / SCL |

Use the signal labels to identify the connector orientation. Wire colours do not
identify I2C signals. PA7/TX0 and PB2/RX1 are unused by this build. Keep the NRF
installed; GPIO43 is its CE and GPIO44 is its CSN. The Plus firmware drives CE low
and CSN high at boot, before shared SPI/SD traffic.

The link uses 3.3 V logic, slave address **0x42**, and the existing **100 kHz** bus.
It shares the port with battery management and NFC; it does not replace their
bus driver or change their configuration. The external supply must remain stable
under BW16 load. Moving data pins addresses the NRF/SD pin conflict; it cannot
correct a separate power dip.

The [official LilyGO pin map](https://github.com/Xinyuan-LilyGO/T-Embed-CC1101/blob/master/docs/pinmap_cn.md)
identifies the shared pins. BW16 pin mapping comes from the pinned AmebaD 3.1.9
BW16 variant included in the local build environment.

## Install

1. Disconnect the four BW16 wires before flashing either board.
2. Flash the **new Plus T-Embed firmware**, using its actual USB port:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\T-Embed\FLASH.ps1 -Port COM20
   ```

   If necessary, hold the encoder centre (BOOT), press/release rear RST, then
   release BOOT and use the newly listed COM port.
3. Connect only BW16 USB and flash the matching I2C firmware:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\BW16\FLASH_BW16.ps1 -Port COM3 -BaudRate 460800
   ```

   Enter upload mode: hold BURN, press/release RST, then release BURN. Wait for
   verified SUCCESS, press RST, and unplug USB before reconnecting the I2C wires.
4. Copy the contents of `T-Embed/SD/` onto the SD card. Open
   **GPIO > BW16 I2C**, from `apps/GPIO/bw16_r4tkn_i2c.fap`.
   The previous `bw16_r4tkn.fap` is the UART app and cannot use this transport.
5. Power off, connect the four I2C wires above, and restart both boards.

COM20 and COM3 are examples; choose the ports currently shown on your computer.
The scripts verify image hashes before opening a port. T-Embed flashing writes
individual firmware regions and does not erase the entire flash or SD card.

## Included functionality

The BW16 app retains the existing scan/loading interface, multiple selection,
manual pairing of networks across bands, selected-target radio controls, passive
PCAP capture and Capture + deauth option. Radio/capture behavior and limits come
from the unchanged 4.4 controller core. A scan entry still identifies a BSSID and
channel; capture listens on one selected channel. PCAP files remain in
`apps_data/bw16/captures/` and successful capture must be checked in the file.

Wardriver 1.5.2 is included as a Plus build with a GPS pin guard. Its survey,
BLE and storage features are retained. Saved GPS settings cannot take GPIO43/44
from the NRF or GPIO8/18 from I2C; survey continues without GPS on those pins.
Copy the supplied SD/apps/Tools/wardriver.fap over the earlier file to apply
this guard. The other supplied SD apps/database are retained. The newly
built system retains Spectrum Analyzer/RF Waterfall. On the Plus, **the old GPS
UART43/44 connector conflicts with the installed NRF too**. The BW16 UART capability is disabled and the supplied Wardriver blocks those
GPS pins; moving BW16 to I2C does not relocate GPS. The
original no-NRF UART bundle remains the version for that GPS wiring.

The I2C link validates fixed-size messages with CRC32, acknowledges stream data,
and suppresses duplicate command execution. Queued capture lines remain atomic.
A three-second loss of valid host requests stops active BW16 work. A STOP message
bypasses a full command queue; the app allows 20 seconds to drain pending capture
records. I2C is slower than UART, and heavy traffic can still drop received radio
frames when the bounded receive queue fills; those drops are counted.

A reconnect during a previous scan may report that BW16 is not ready. Let that
scan finish and reconnect, or reset BW16. Data from old sessions is discarded.

## Validation and first hardware check

Builds and host tests exercise frame corruption/retries, command deduplication,
STOP/reconnect, partial I2C transactions, capture queue draining and SD file writes.
FAP imports are checked against the actual Plus firmware ELF; package files have
SHA256 checksums. No board is flashed or live radio test run by these checks.
**Electrical behavior and operation on a physical Plus/BW16 pair remain untested.**

After installing, first confirm SD access with NRF installed, open BW16 I2C and
run a scan. Check battery/NFC operation and reopen an SD file while BW16 is wired.
If communication fails, check the new SDA/SCL wiring and both matching images.
If rebooting persists, capture the reset log and check power separately.

The source snapshots under `source/` are for the full project checkout, not a
standalone SDK. Build helpers create `build_t_embed_plus_i2c` and use
`sdkconfig.plus_i2c`; the existing `build_t_embed`, root `sdkconfig` and UART 4.4
release files are not replaced. `BUILD_INFO.json` records provenance and hashes.
