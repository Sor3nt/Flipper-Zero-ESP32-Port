# RTL433 sources

Vendored decoder and pulse-slicer C sources from https://github.com/NorthernMan54/rtl_433_ESP
Commit: f0a81569b58a60ea8b3a5efd8e9b39670e8d8df8

Upstream rtl_433 snapshot documented by that project: 25.12-346-g91b2ebdb.
The Arduino/RadioLib wrapper is replaced with a native T-Embed Native Tools FAP adapter.
Preserved source license notices apply; distributed under GPL-3.0-or-later.

Local change in 1.1.1: the two bitbuffer capacity-warning calls use the
existing logger hook instead of direct stderr writes. The app aggregates
these warnings on-screen to avoid serial stalls on noisy signals. Decoder
logic, bitbuffer bounds and capacity handling are unchanged.

Local change in 1.1.2: pulse_slicer_dmc checks that a second symbol exists
before consuming a short-symbol pair. This prevents lookahead past a full
capture ending on an unpaired short symbol; incomplete bits are not invented.

Local changes in 1.1.3: native RZI slicer and dispatch, implemented from the
vendored tfa_30_3307.c timing/bitstream description (not represented as a
verbatim upstream port); bounded Oregon-v1 preamble/sync lookahead. The
radio, session-mode and dashboard changes belong to the native adapter.
