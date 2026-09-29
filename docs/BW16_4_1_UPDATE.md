# BW16 R4TKN 4.1 FAP update

Replace `apps/GPIO/bw16_r4tkn.fap` on your T-Embed SD card with the file in
`SD/apps/GPIO/` from this package. Exit the old app before replacing it, then
reopen Apps -> GPIO -> BW16 R4TKN. No T-Embed or BW16 firmware flash is needed.
Your working R4TKN v4 TX0-fix firmware and UART wiring stay in use.

## New controls

- **Scan networks**: fresh scan, animated loading indicator, then indexed results.
- **Select networks**: rotate to browse; short OK marks/unmarks a network group.
  An identical SSID automatically includes its discovered 2.4 GHz and 5 GHz radios.
- **Pair Wi-Fi names**: choose one name with OK, then choose its other-band name
  with OK. Use this when the two bands have different names. Return to Select
  networks and mark the joined group. Repeat for another pair if needed.
- **Deauth selected**: review the marked radios; rotate to check each band,
  channel and BSSID. Hold OK to start a batch lasting at most 30 seconds before
  requesting STOP. You can also hold OK in Select networks to open this review.
- **OK / Back during a batch**: stop the entire batch and wait for confirmation.

Pairing is explicit; `LAB_0305_5G` will not silently select a differently named
network. Only entries found in the current indexed list can be paired/selected.
Links and marks reset on a rescan, list replacement or app close. `[x]` means all
members marked; `[-]` means some; `[ ]` means none.

The BW16 cycles selected radios one at a time, waiting for confirmed STOP between
them. It does not transmit on both bands simultaneously. TX attempts do not prove
that devices disconnected; clients may reconnect or roam between visits.
The original station, client, all-scanned and UART diagnostic options remain.

## Your working cable

| Wire / T-Embed | BW16 |
| --- | --- |
| Yellow / RX GPIO44 | PA7 / TX0 |
| White / TX GPIO43 | PB2 / RX1 |
| 3.3 V | 3V3 |
| GND | GND |

GPS and BW16 use this header separately. Close the app before swapping modules.

## Validation

Host tests cover the production parser, selection/pairing, command sequencing,
START/STOP and quiet gating, cancellation, 30-second deadline and timer wrap,
UART/GPS resource handling, UI inputs and native-font screen layout. The FAP's
imports and relocations are checked against the existing T-Embed firmware ELF.
New multi-target radio behavior still needs testing on your physical hardware.
`BUILD_INFO.json` records the FAP hash and hashes of unchanged firmware artifacts.
