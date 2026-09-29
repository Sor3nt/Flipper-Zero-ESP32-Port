# ProtoPirate receive stability

| Build name | Hardware | App update |
| --- | --- | --- |
| Lilygo T-embed cc1101 | T-Embed CC1101 without NRF | `proto_pirate.fap` |
| Lily T-embed cc1101 plus | T-Embed CC1101 Plus with NRF | `proto_pirate.fap` |

The receive-history fix uses the same app binary on both firmware variants. These
are compatibility labels, not new hardware profiles or pin mappings. Existing
protocol plugins and saved captures remain in place.

## Failure and fix

A new decoded signal appended to history could close `loaded_ff` while the UI
was still reading it during auto-save or saved-signal matching. On ESP32 this
can abort in the storage API after several successfully decoded signals.

Appending now preserves the borrowed reader until the UI releases/switches it
or removes its record. A recursive history mutex protects index, text-arena and
scratch-path access across receive, UI and GUI threads. The receive callback
commits the record and pending flags under that lock, releases it, then queues a
UI update. It does not acquire a view-model lock while holding the history lock.

Protocol decoding, transmission, hardware pin assignments and the BW16 firmware
are unchanged. The existing stop/join-before-destruction lifecycle remains required.

## Validation

Run `python3 tests/protopirate_rx/run.py` from the repository root. On Windows set
`ZIG_CC` to the installed `zig.exe`; the existing host runner uses native mocks.
GCC builds enable ASan and UBSan. The harness executes the production history
implementation with fake storage/decoder boundaries and a real concurrent thread.

Covered cases: borrowed-reader lifetime during incoming records, GUI text reads
while the arena grows, pending flags, history capacity, duplicate reception,
serialization/storage failures, switching/deleting readers, reset and cleanup.
The same regression fails with the pre-fix source and passes with the fix.

The replacement ESP32-S3 app was compiled and all 258 imports were checked against
linked local firmware for both variants (including Plus SD Recovery 1.3.2).
The crash was captured on the non-NRF variant; hardware confirmation of the fixed
app is pending. The standalone host test passed on Windows; sanitizer execution
must be reported separately when run on a compatible GCC host.

This fix does not claim to resolve an unrelated SPI assertion seen once during
SD-card initialization at boot.
