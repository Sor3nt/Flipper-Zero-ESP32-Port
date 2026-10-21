# RTL433 1.1.8 — Short-burst validation

Native ESP32-S3 FAP for LilyGo T-Embed CC1101 / CC1101 Plus running the
Sor3nt-based WirelessPi firmware. No Raspberry Pi is involved. This binary
is not for a stock Flipper Zero.

## Install

Exit RTL433. Replace **apps/Sub-GHz/rtl433.fap** on the SD card with
**SD/apps/Sub-GHz/rtl433.fap** from this archive. The title and About screen
must show **RTL433 1.1.8**. No firmware reflash is needed on the compatible
WirelessPi Native Tools 1.0 / RF3 build. All 125 imports were checked against
the linked RF3 firmware image. Keep the rest of the SD card in place.

Saved settings, decoder selections, captures and exports remain intact.
The icon, Signal Desk theme, compact font, receive profiles, gain settings,
queue fixes and fast screen updates are retained.

## New in 1.1.8

Fixed a reproduced short-burst regression in 1.1.7: capture termination
extends the final low gap to 100 ms. The strict validator rejected that gap,
so a valid two-frame press had only one eligible frame and disappeared.
The final complete 24-bit payload plus short stop pulse can now confirm a
preceding matching frame with a real sync. A terminal marker alone cannot
establish a match. Two matching payloads, timing and stop-pulse validation
remain required; single, mismatched or incomplete frames are rejected.

All 256 final-byte values pass the two-frame termination regression. This
also covers button encodings beyond 1/2/4/8. It does not establish why the
user's particular remaining buttons produce smoke or another model: raw
recordings of each physical button are still needed for that diagnosis.

For comparison, capture each button separately, stop reception immediately,
open **Last raw pulse / save**, and press OK. Files are saved under
**apps_data/rtl433/** with the `.rtl` extension. Note which file corresponds
to each button; include the exact unexpected model name or a photo.

## Identification changes retained from 1.1.7

A synthetic Princeton waveform reproduced the three reported labels in
1.1.6: Akhan-100F14, RF-tech and Smoke-GS558. These were decoder collisions,
not evidence of three physical devices.

- Akhan uses the same 24-bit EV1527 encoding as other remotes. Those bits
  cannot establish a particular manufacturer/model.
- RF-tech had no timing tolerance. Its slicer interpreted unrelated short
  PWM gaps as zero bits, producing a bogus temperature record.
- GS558 accepted incomplete frame shapes without its two short stop pulses.
- The generic remote decoder checked high pulse widths but did not validate
  the full Princeton pulse/gap timing and sync structure.

## Changes

1. The generic remote entry now checks 24 complete pulse/gap pairs, sync
   timing and at least two consecutive matching frames. It adapts to pulse
   timing rather than assuming one fixed remote speed. It reports
   **Princeton** as an encoding family, including raw code, address, data
   nibble, pulse time, repeats and tri-state view. This is not a brand claim.
2. An Akhan result for the same independently validated code is deduplicated.
   If Akhan is selected alone, its output says Princeton-compatible and
   retains Akhan only as an explicitly unverified candidate.
3. RF-tech now checks plausible gap timing and decimal temperature fractions.
4. GS558 requires its documented complete stop-pulse framing. Genuine
   RF-tech and GS558 fixtures still decode.
5. **Every catalog result** now exposes identification status and validation
   evidence in JSON/CSV. On-screen, **~** means a model candidate. The
   second line shows CANDIDATE, CRC, CHECKSUM or PARITY. Princeton's line
   says FRAMING because full timing and matching frames were validated.
   CRC/checksum/parity labels describe checks reported by the decoder;
   they do not prove a manufacturer, authenticity or a unique physical device.
6. The About screen explains these labels, and optional decoders carry an
   ambiguity warning in the catalog.

No catalog entries were removed. There are still 377 entries, 376 runnable
and 333 enabled by default. All decoded sensor fields are preserved.
Multiple independent transmissions can still produce multiple results.

## Validation and limits

The host ASan/UBSan tests include:

- 80 Princeton button/timing/jitter/cache-order cases, with all runnable
  decoders enabled: one family result and none of the three false models.
- RF-tech documented data and GS558 documented framing; separate remote
  and sensor bursts in one capture remain available together.
- Single, incomplete, changing, malformed-gap and malformed-sync frames;
  sample-rate changes, leading noise, clipped endings and multiple keys.
- Evidence reporting for all 377 catalog entries, plus 1,504 bounded
  synthetic decoder inputs. These are not 376 physical device recordings.
- Original/ALL scanning, weak OOK, FSK pretrigger and queue handling,
  doorbell, synthetic 315 MHz Toyota TPMS, full JSON/CSV fields, lifecycle,
  raw replay and incremental UI regressions.

Zero false positives cannot be guaranteed across RF protocols: many share
encodings and lack integrity checks, and checksums can collide. This release
fixes the reproduced collisions and stops treating model candidates as
confirmed identities. It does not claim every physical device in the catalog
has been tested. No RF hardware or recording of your particular remote was
available; the reproduction uses independent synthetic pulse timings.

The radio profile schedule and sensitivity are unchanged from 1.1.6. ALL
profiles still shares one receiver over time; it is not simultaneous
reception on every modulation/frequency.

## Source and build

The ZIP contains a complete **app source overlay**, vendored decoder
library, licenses, tests, patch from 1.1.7 and validation results.
The firmware source and ESP-IDF are not duplicated. Overlay
source/applications_user/rtl433 onto the matching WirelessPi source tree.
With ESP-IDF 5.4.1, its Xtensa toolchain and Pillow available:

```sh
FAP_BOARD=lilygo_t_embed_cc1101 FAP_FW_BUILD_DIR=build_native_rf \
FAP_APP_VERSION_OVERRIDE=118 bash buildFap.sh applications_user/rtl433
```

Use the build directory containing your compatible firmware ELF/API exports.
This delivered binary was checked against RF3 commit
33549321fab648901691ff528ac52c427eb25fd3. The patch baseline is 1.1.7 commit
9c91ec2cb14bc8efb87fcf347b452ba029b2776d.

Run host regressions from the firmware root:

```sh
bash applications_user/rtl433/tests/run.sh
```

No firmware source changes or GitHub upload are part of this update.
