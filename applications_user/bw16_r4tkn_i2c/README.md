# BW16 I2C 4.5 for T-Embed Plus

This isolated app uses the T-Embed Plus shared I2C bus at address 0x42. It leaves
the original BW16 UART app, its firmware profile, GPIO43/44 and release files
unchanged. Install only with the matching I2C firmware and BW16 I2C image
described in [the I2C setup guide](../../docs/BW16_PLUS_I2C.md).

| T-Embed Plus | BW16 I2C firmware |
| --- | --- |
| SDA / GPIO8 | PA26 |
| SCL / GPIO18 | PA25 |
| 3V3 | 3V3 |
| GND | GND |

The app borrows the firmware's existing 100 kHz bus. It never installs,
reconfigures or deletes a bus driver, and has no UART imports.

Every connect opens a fresh random link session. Fixed 64-byte frames include
CRC32, session, command and response sequence numbers. Commands are retried
with the same sequence, payload and frame until accepted; response chunks are
copied exactly once and acknowledged only after buffering. A lost command
acknowledgement triggers an out-of-band STOP. STOP bypasses a failed or pending
command and retains the existing operation DONE confirmation. While stream
data is pending or STOP is draining, the app checks keys without an extra sleep. The I2C profile
allows 20 seconds for queued capture data and DONE to drain.

All 4.4 menu features remain available: scans, indexed APs, clients, network
groups and selected operations; passive Capture handshake; and the separate
Capture + deauth option. Active capture requires a hold-to-start review, arms RX
first, and requests one three-frame deauth burst on one AP. TX attempts and
EAPOL counts do not prove a disconnect or a complete handshake.

Capture preflight creates and syncs a unique PCAP file under
`/ext/apps_data/bw16/captures` before any capture command. Capture stays on one
AP/channel for up to 120 seconds or 256 written packets. Back requests STOP;
Right displays the path. Write/sync/close errors retain the partial file and
never report Saved. Timestamps combine the RTC sampled just before the command
with BW16 software arrival offsets.

Host validation: `python tests/bw16_i2c_app/run.py` (Windows: set ZIG_CC).
The suite checks framing errors, retries, duplicate chunks, uncertain command
STOP, bus faults, reconnect epochs, UI/SD flows and rendered screens. It also
verifies byte-for-byte preservation of the existing UART app and its tests.
