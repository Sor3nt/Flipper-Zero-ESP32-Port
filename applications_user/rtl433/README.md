# RTL433 1.1.6 — Receive queue fixes

Native ESP32-S3 FAP for LilyGo T-Embed CC1101 / CC1101 Plus running the
Sor3nt-based T-Embed Native Tools firmware. No Raspberry Pi is involved. This binary
is not for a stock Flipper Zero.

## Install

Exit RTL433 and replace **apps/Sub-GHz/rtl433.fap** on the SD card with
**SD/apps/Sub-GHz/rtl433.fap** from this archive. The app title must show
**RTL433 1.1.6**. This update only changes the FAP; no firmware reflash is
required on the existing compatible T-Embed Native Tools build. Firmware API checks
are included in validation/symbols.log.

Choose **Start receiver (original)** for the original two-profile AUTO
schedule. It alternates OOK650 and FSK40, with a 2-second listening period
extended up to 3 seconds during a burst. It submits packets during that
period rather than waiting until it finishes.

Radio settings, captures, exports and decoder selections remain intact.
If the home screen reports fewer than the standard 333 enabled decoders,
open **Decoder catalog** and **hold OK** to restore the default selection.
Use the transmitter's frequency and leave band hopping off for a comparison
with a particular device. No settings file needs to be deleted.

## What was actually wrong

The previous release restored scan timing but left two receive-path
regressions. These were reproduced with complete sensor fixtures through
the actual receive worker and decoder engine, with simulated GPIO/RSSI.

1. **Buffered FSK pulses were discarded after a processing delay.** The
   newer receiver applied the 4 ms pretrigger age limit to the pending
   input queue too. That could discard a real packet or its beginning when
   the capture worker ran late. The first receiver adapter did not do this.
   Version 1.1.6 processes pending received pulses without that age cutoff.
   Closed-gate noise history remains bounded to 256 edges / 4 ms; the input
   queue remains bounded to 4096 edges, with explicit overflow recovery.

2. **Unfinished snapshots could crowd out complete packets.** Periodic
   snapshots of a noisy channel could fill all three decoder queue slots
   when decoding fell behind. The next complete sensor burst was dropped.
   Version 1.1.6 allows at most one unfinished snapshot outstanding,
   including one already decoding. Further partial snapshots are coalesced
   while capture continues, leaving queue capacity for completed bursts.
   Complete packets can still overflow the bounded queue under sustained
   overload; this is not a promise of unlimited throughput.

## Evidence

| Test | Before (1.1.5) | 1.1.6 |
| --- | --- | --- |
| Valid WH31E FSK packet queued while capture worker pauses 12 ms | 0 decoded, no edge drops | 1 decoded, ID 195 and humidity 92 intact |
| Complete Prologue OOK packet after noisy partial snapshots and a stalled decoder | 0 decoded, 3 frame drops | 1 decoded, 0 frame drops, temperature 23.4 C and humidity 56 intact |

The same delayed-FSK test passes against the original 1.0.1 receive adapter
using the current shared types and decoder engine. This isolates the adapter
regression; it is not a physical side-by-side test of the two whole releases.
See validation/baseline.log, before.log, before-backlog.log and tests.log.

The existing host sanitizer suite also passes: original and ALL scanning,
weak OOK capture, FSK pretrigger/tail handling, continuous-noise recovery,
doorbell decoding, full decoded fields, synthetic 315 MHz Toyota TPMS,
receive lifecycle, raw replay and incremental UI rendering. The catalog
tests exercise synthetic inputs and bounds, not every physical transmitter.

No user-provided failing waveform or RF hardware was available. These fixes
address proven software losses; they do not establish that every signal
reported missing, or physical range, is now restored. For a remaining failure,
the original transmitted .sub/raw file and its frequency/profile are useful.

## Features retained

- Signal Desk theme, compact text, icon and fast result updates.
- Original AUTO, optional ALL six-profile scan, and FIXED reception.
- All 377 catalog entries, with 333 default-enabled decoders.
- Weak OOK acceptance, bounded FSK pretrigger, overlapping noisy-channel
  windows, decoder scheduling and prior memory-bounds fixes.
- Complete decoded fields, history, SD logging, JSON/CSV export and raw replay.

ALL profiles shares one receiver over time. It cannot listen to every
frequency/modulation simultaneously and can miss short transmissions while
another profile is active. It remains available as the second menu action.
Hold OK on the live screen to lock the current frequency/profile; hold it
again to resume the previous automatic mode.

## Source and build

The archive contains the complete **app source overlay**, its vendored
decoder library, licenses, tests, a patch from 1.1.5 and validation logs.
The full firmware tree and ESP-IDF are not duplicated. Overlay
source/applications_user/rtl433 onto the matching T-Embed Native Tools firmware source.
Build with its ESP-IDF 5.4.1 / Xtensa toolchain active:

```sh
FAP_FW_BUILD_DIR=build_native FAP_APP_VERSION_OVERRIDE=116 \
  bash buildFap.sh applications_user/rtl433
```

Host regressions from the firmware root:

```sh
bash applications_user/rtl433/tests/run.sh
```

No firmware source changes or GitHub upload are part of this update.
