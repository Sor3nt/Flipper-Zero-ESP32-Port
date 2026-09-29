# RF Spectrogram / Waterfall

The main menu's **RF Waterfall** app implements the Bruce beta "Spectogram"
workflow. In Bruce these names refer to the same feature, not two different
instruments. The original Sub-GHz / Spectrum Analyzer entry is retained.
There is no Momentum application registry or category-menu replacement.

## Reference and measurement

Inspected beta/dev revision:
[`d547ac5a22e787045226b3b829f54f79cf6296ec`](https://github.com/BruceDevices/firmware/tree/d547ac5a22e787045226b3b829f54f79cf6296ec).
Its `src/modules/rf/rf_waterfall.cpp`, `src/core/spectrum_plot.{cpp,h}`
and RF menu mapping are unchanged from the earlier inspected beta
`213f570f16afe84974906180e339154eec26e187`.

This is a stepped CC1101 RSSI sweep, NOT FFT data or simultaneous broadband
capture. One sweep measures 64 frequencies including both endpoints, requests
3 ms settling after each tune, and uses the approximately 203 kHz CC1101 receive
filter corresponding to Bruce's 200 kHz request. Calibration is time-bounded.
Sweep speed includes calibration, bus arbitration and RTOS scheduling.

Actual samples feed a newest-first color waterfall and raw peak/RSSI readout.
The top trace is filled, interpolated and visually eased; its decaying peak line
and strongest-bin highlight follow beta behavior. Interpolation is not extra
RF resolution. No decorative random "noise grass" is fabricated.

The trace baseline is now at the screen midpoint (y=85 on 320x170), with a
narrow blue glow immediately below the red trace and a dark blue body. The
waterfall starts at y=88; this visual change does not alter RF measurements.

## Controls

- Main menu -> RF Waterfall -> Start Waterfall.
- Set Start and End in kHz before starting; default 433.000 to 435.000 MHz.
- Reversed boundaries are swapped; equal/invalid endpoints are rejected.
- Rotate in the plot to pan; step scales with the selected span.
- Encoder press pauses/resumes; Back returns to setup, then exits.
- Five frequency ticks, strongest frequency and dBm are displayed.

The supported bands are 300-348, 387-464 and 779-928 MHz, additionally checked
against the existing HAL. Gaps are skipped and marked with a floor sentinel,
never tuned. Short bursts can be missed between sweeps. This app does not transmit.
The existing Spectrum Analyzer uses its separate 128-bin/control profile.

## Local architecture and memory

`applications/main/rf_waterfall/` is an ordinary native FAM app using existing
Furi GUI/number input/dialogs. It shares the local spectrum worker/compositor;
it does not initialize a Bruce or Arduino driver stack.

320x170 display allocation bounds:

- Reused PSRAM framebuffer: 108,800 bytes.
- Fixed uint8 history ring: 128 x 96 = 12,288 bytes; waterfall mode displays
  the newest 53 rows and uses 64 samples per row.
- Reused internal DMA stripe: 320 x 8 x 2 = 5,120 bytes, matching the existing
  display SPI transfer limit. No per-sweep framebuffer allocations.
- Raw, peak and eased arrays: 1,536 bytes; two 4 KiB worker/render stacks.

These are source-layout bounds, not measured free-heap numbers. The display
operation serializes shared SPI traffic and drains DMA before reusing the stripe.
Exit unsubscribes input, stops/joins workers, idles/sleeps CC1101, releases
direct drawing and frees buffers. Allocation failure during scan startup cleans
partial buffers and reports an error.

Original single-foreground-app loader semantics are retained. Do not run a
separate CC1101 CLI/background operation concurrently; this build does not add
a global resource-manager architecture.

## Verification

`python tests/host/run_rf_tests.py` exercises the actual production sampler
(300 sweeps, 64/128 bins, endpoints, dwell, calibration failure and cleanup),
10,000 history wraps and 200 production compositor frames with framebuffer guards
and LCD stripe-size checks. The 320x170 synthetic preview is visually inspected;
it is not hardware reception evidence.

Physical boot, RF sensitivity/frequency accuracy, display behavior and repeated
device launch/exit remain hardware acceptance checks. No screenshot/data export
or calibrated spectrum-instrument accuracy is claimed.
