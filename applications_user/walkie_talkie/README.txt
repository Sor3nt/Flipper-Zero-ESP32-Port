WALKIE TALKIE 1.2 — WIRELESSPI NATIVE RX2 EXPERIMENTAL PORT

Target: LilyGo T-Embed CC1101/Plus, SOR3NT Native Tools with PCM speaker exports.
Not an STM32 Flipper Zero FAP. Not validated against Momentum or other builds.

INSTALL
Copy SD_ROOT/apps/Sub-GHz/walkie_talkie.fap to /apps/Sub-GHz/ on the SD card.
Open Walkie Talkie from Apps > Sub-GHz, or launch it using the file browser.
The old PR #125 firmware requires an update for the new speaker exports.
No plugins, app_data or additional receiver are required. The icon is embedded in the FAP.

FEATURES
22 original FRS channel entries; manual tuning; up/down scanning; two-sample
signal confirmation; pause on activity and resume after 500 ms without signal;
RSSI and five signal bars; adjustable squelch (-120 to -30 dBm); mute;
subchannel labels 0–38; channel list; rotary-accessible settings and volume.
Settings are session-only and reset when the application closes.
Receive only. No transmission, recording, or CTCSS/DCS decoding.

CONTROLS
Monitor: turn to change channel, click to mute/unmute.
Hold click: start scan; while scanning, stop; while holding an active channel,
hold click to skip to the next channel and continue scanning.
Back: menu. Hold Back: exit from any page.
Menu: turn to select, click to open. Exit is also available in the menu.
Settings: turn to select, click to enter edit mode (*), turn to change,
click to finish editing. Back first ends editing, then returns to the menu.
FRS channels: turn to highlight, click to tune.

EXPERIMENTAL SOUND
The onboard CC1101 pulse-duration callback feeds a bounded 256-event queue.
A separate worker converts pulse durations to 44.1 kHz PCM through the
firmware's existing I2S speaker API. Fractional sample timing is retained.
DC blocking, conservative default volume (3/10), stale-event rejection,
channel-change epochs, silence on idle, and dropped-pulse diagnostics are
included. Long gaps over 20 ms are discarded as silence.
The queue does not grow; nothing is allocated per incoming pulse. Capture
interrupts are disabled while audio is muted, squelched or scanning without
a held signal. UI refresh is limited to 10 Hz except user input.

For a first sound test, select your transmitter's matching channel, leave
scan off, unmute, and set Auto squelch OFF in Settings. This may produce
noise even without a signal. Start with low volume. Enable squelch again
and adjust its threshold once you have observed the signal's RSSI.
Check About / diagnostics for dropped pulses. Drops indicate capture/audio
throughput loss, not successful voice decoding. Speaker busy or Audio write
error is shown if the audio resource cannot be acquired or written.

IMPORTANT LIMITATIONS
This is experimental DIGITAL RECEIVE SOUND, not an analog FM voice decoder.
Do not expect intelligible speech or fidelity equivalent to a real radio.
The original upstream README also documents this limitation.
Channels 8–14 (467 MHz) are outside TI's specified 387–464 MHz CC1101 band.
They remain selectable because Native RX2 allows them, but reception there
is experimental. Antenna/front-end matching also affects reception.

BUILD IN THIS TREE
Run bash BUILD-NATIVE-TOOLS.sh with ESP-IDF 5.4.1 active.
This PR adds furi_hal_speaker_stop and furi_hal_speaker_write_pcm exports.
Rebuild/flash the matching firmware before using this app on the old PR #125 build.
The historical RX2 firmware already has those exports.
Run python3 tests/walkie_talkie/test_controls.py for host control regressions.
No hardware acceptance or intelligible voice decoding is claimed.

UPSTREAM
https://github.com/coolshrimp/flipperzero-walkie-talkie
Commit e7fa94988258c3b6100ac863b0a2401e348f6192
Original channel monitor and icon: Coolshrimp Modz, MIT.
Native port rewrites platform glue, audio output, rotary controls and lifecycle.
