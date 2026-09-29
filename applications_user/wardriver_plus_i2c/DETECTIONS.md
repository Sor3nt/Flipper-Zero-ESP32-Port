# Wardriver 1.4.0 detection coverage

This version expands the existing passive Wardriver FAP, using signature facts
from [SquachWatch-CYD](https://github.com/skizzophrenic/SquachWatch-CYD), revision
`ecaff618e67b9c67121174937d0145c7567e1585`. It does not run CYD firmware on the
T-Embed. The adapted OUI data carries the upstream GPL-3.0 notice and the full
license is included as `LICENSE-SquachWatch`. Radio, parser, monitor and UI logic
are implemented for this port. Credit: skizzophrenic and SquachWatch contributors;
see upstream `docs/DETECTIONS.md` for their signature provenance.

## Implemented detection paths

| Display | Evidence and limits |
| --- | --- |
| AirTag | Valid Apple Find My type 0x12, payload length 2 or 25, status class 1. All four battery levels work. Name-only matches have lower confidence. |
| Find My | Generic accessory status class 2 or FD44; cannot passively distinguish every newer AirTag from third-party Find My products. Class 0 Apple phones/computers are not counted as trackers. |
| AirPods | Find My status class 3; kept separate from AirTag. |
| SmartTag | FD5A service; name matches are weaker. |
| Tile / Chipolo | FEED or FEEC / FD84 services. |
| Google tag | FEAA service data with FHN 0x40/0x41 frame and a valid payload length; bare FEAA is not enough. |
| Flock? | Flock/Penguin/Pigvision/FS Ext Battery names or Flock's registered B4:1E:52 OUI. XUNTONG alone remains a supplier clue. |
| Axon? | Axon/AB2-/AB3-/AB4- names or a registered Axon prefix. |
| Meta glasses / Glasses? | FD5F service versus weaker Meta/Luxottica/Snap company IDs, which can also identify other products. |
| Raven? | The five upstream advertised services 3100, 3200, 3300, 3400, 3500. |
| Camera? / Ring? / ALPR? | 37 retained vendor-prefix entries across all vendor categories. Manufacturer identity does not establish the exact product or its purpose. |
| Drone / RID | Valid Remote ID Basic ID, Location or System message, over legacy BLE service data or Wi-Fi beacon vendor IE; packed messages supported. |
| Flipper | 3081/3082/3083, company 0E29, registered OUI or a weaker advertised name. |
| Pwnagotchi | Self-declaration in beacon IE 222; name and pwnd_tot keys. |
| Pineapple? / Deauther? | Pineapple_ / pwned SSID prefixes. These names can be changed or imitated. |
| Skimmer? | Low-confidence BLE serial-module names/SPP service only. These modules have many legitimate uses. No Bluetooth Classic scan. |
| Deauth burst | At least six deauthentication/disassociation frames from one transmitter within 3 s, 15 s alert cooldown; retry duplicates excluded. |
| Evil twin? | Different BSSIDs advertising the same SSID with conflicting Privacy bits within 30 s. A security mismatch is suspicious, not proof of an attack. |
| iBeacon / Eddystone | Structured packet matches. Available in All devices and the Beacons filter, excluded from the ordinary Notable count. |

The original six hint bits retain their numeric values. Local CSV keeps its first
21 columns and appends 12 fields: Device, Confidence, Evidence, RIDLatitude,
RIDLongitude, RIDAltitudeMeters, RIDOperatorLatitude, RIDOperatorLongitude,
RIDOperatorType, RIDLocationUptimeMs, RIDOperatorUptimeMs, HintFlags. WiGLE CSV
format remains unchanged. Observer GPS is never replaced by a drone's broadcast
position. Missing coordinates stay blank; unknown later RID fixes clear older
coordinates. Operator type 0 means takeoff, 1 live GNSS, 2 fixed location.

## Use

- A new notable observation shows a five-second banner on the home screen,
  using the selected group and minimum-confidence filter.
- **Menu > Notable devices** shows the detected class and last two address bytes.
  The full name and address remain on detail page 1.
- **Menu > Detection filter**: turn to select All notable, Trackers, Cameras,
  Radio tools, Wi-Fi activity or Beacons; OK cycles the minimum confidence.
  Filters last for the current FAP launch and never remove logged observations.
- Device detail pages: identification/serial, evidence/confidence, observation
  times, observer GPS, aircraft position/altitude, operator position, live RSSI.
  OK advances and Back returns. Signal strength indicates neither distance nor
  direction; the screen shows the age and flags stale readings.
- Confidence is a signature grade, not a probability or authentication check.
  Radio advertisements, names and addresses can be spoofed.

## Repeat scans

The old parser required the full 25-byte Find My payload. A short nearby-state
advertisement could still appear as an ordinary BLE device but lose its notable
classification in a fresh run. Both forms are now classified from their status.
Each Start clears the session table, pending observations and activity monitor;
scan duplicate filtering is disabled. Unexpected continuous BLE scan completion
is retried up to three times with a 1 s backoff and visible status. Once seen, classification is retained for
that address during the current session even if later packets omit the signature.
A new session must hear a new packet; old logs are not presented as live finds.
Rotating addresses remain separate observations and are not silently linked to a
person or counted as a known physical device.

This is a demonstrated parser gap, not a confirmed diagnosis of the user's
hardware report. RF reception, a tag connected to its owner, firmware state and
advertisement timing can also change what is heard. Verify Stop/Start and full
FAP exit/reopen with known devices; watch Session stats for BLE errors.

## Hardware and scope

ESP32-S3 supports BLE, not Bluetooth Classic. The FAP uses 2.4 GHz Wi-Fi and BLE
legacy scanning; this build does not enable extended advertisements/Coded PHY,
Wi-Fi NAN Remote ID, 5/6 GHz, cellular interception detection or silent-device
detection. No remote connections, GATT probing, deauthentication transmission,
tracker ringing, jamming, mesh messaging or firmware updates are added.
CYD cosmetic/game/mesh features are not counter-surveillance detection paths.

Shared Espressif/Liteon/Realtek module prefixes and locally administered/random
addresses are not treated as proof of Flock, a camera or a radio attack tool.
Broad Apple pairing packets are not called AirTags: that also labels AirPods as
AirTags. The protocol type/status must identify the product, or the UI retains a
more general label. These are deliberate precision limits on upstream heuristics.

## Primary references

- SquachWatch signature catalog: https://github.com/skizzophrenic/SquachWatch-CYD/blob/ecaff618e67b9c67121174937d0145c7567e1585/docs/DETECTIONS.md
- AirGuard status classification: https://github.com/seemoo-lab/AirGuard/blob/f3fbadf/app/src/main/java/de/seemoo/at_tracking_detection/database/models/device/DeviceManager.kt
- AirTag states and status: https://crysp.petsymposium.org/popets/2023/popets-2023-0102.pdf
- Google FHN framing: https://developers.google.com/nearby/fast-pair/specifications/extensions/fmdn
- Remote ID wire layouts: https://github.com/opendroneid/opendroneid-core-c/blob/master/libopendroneid/opendroneid.h
- ESP32-S3 Bluetooth: https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/ble/overview.html

## Verification

Run `python tests/host/run_wardriver_tests.py` with `ZIG_CC` set on Windows,
then `buildFap.sh applications_user/wardriver` in the configured ESP-IDF 5.4.1
environment and `python tests/bw16/check_abi.py build_t_embed/fap/wardriver.fap`.
Tests use synthetic protocol fixtures and mocked SDK boundaries. They do not
establish physical detection range or hardware reliability.
