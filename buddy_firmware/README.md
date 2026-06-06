# ESP32 Buddy

Headless companion firmware for cheap ESP32 boards that pair with the
**Flipper-Zero-ESP32 port** (the master, e.g. a LilyGo T-Embed CC1101) over
**ESP-NOW**.

The idea: a buddy advertises *which features it can run*. The master discovers
buddies, lets the user pair with one, and can then start/stop those features on
the buddy remotely. The buddy needs no display, no SD card, no buttons — just
power and an antenna.

```
  ┌──────────────┐   ESP-NOW (ch 1)    ┌──────────────┐
  │   T-Embed    │  ───────────────►   │  ESP32 buddy │
  │  (master /   │   discover / pair   │  (headless / │
  │   Flipper)   │  ◄───────────────   │   client)    │
  └──────────────┘   features / status └──────────────┘
```

## Status

- ✅ Discovery + pairing, **compatible with the unmodified master** (the buddy
  shows up in the master's "Mesh Clients" list and pairs).
- ✅ Headless auto-accept pairing (configurable), master stored in NVS across
  reboots.
- ✅ Capability advertisement (bitmask) + a feature framework. Features:
  `Identify` (id 0) and `Capture HS` (id 2, passive WPA-handshake capture).
- ✅ Feature control + reliable result delivery are fully wired on the master
  (Lock-Menu → Mesh → client menu → Device / Wifi).
- ✅ **Store-and-forward handshakes:** the buddy holds the EAPOL frames (M1..M4)
  + beacon PER BSSID in a durable store (`buddy_hs_store`, RAM + **NVS**). Each
  complete handshake (M2 & M3) is delivered as ONE fragmented, ack'd unit
  (`BuddyWireResult`); the master reassembles it into a `.pcap` and acks — only
  then is the record dropped. Survives arbitrary master absence AND buddy
  reboot/power loss.

## Build & flash

ESP-IDF **v5.4.1** (same as the master), expected at `$HOME/esp/esp-idf`.

```bash
./build.sh                      # target esp32, build + flash (auto-detect port)
./build.sh esp32s3              # for an ESP32-S3 board
./build.sh esp32c6 /dev/ttyACM0 # explicit target + port
./build.sh esp32 --build-only   # build only

# serial log:
idf.py -B build_esp32 monitor
```

On boot the buddy prints its name and MAC:

```
I (xxx) buddy-now: ready ch=1 mac=A1:B2:C3:D4:E5:F6
I (xxx) buddy-node: buddy 'Buddy-D4E5F6' ready, caps=0x00000005, unpaired
```

Then on the master: open the Lock-Menu → **Mesh Clients**, wait for discovery,
select the buddy, press OK to pair. The buddy auto-accepts and logs
`paired with '<master name>'`.

## Configuration

Everything tweakable lives in [`main/buddy_config.h`](main/buddy_config.h):

| Define | Default | Meaning |
|---|---|---|
| `BUDDY_ESPNOW_CHANNEL` | `1` | WiFi channel — **must match the master** |
| `BUDDY_NAME_PREFIX` | `"Buddy"` | name = prefix + last 3 MAC bytes |
| `BUDDY_AUTO_ACCEPT_PAIR` | `1` | `0` = only pair while BOOT button held |
| `BUDDY_PAIR_BUTTON_GPIO` | `0` | button used when auto-accept is off |
| `BUDDY_IDENTIFY_LED_GPIO` | `2` | LED blinked by the `Identify` feature (`-1` = none) |

## Wire protocol

ESP-NOW payload, max 64 bytes. `[0]=0x4D 'M'`, `[1]=type`, rest type-specific.
Full definition in [`main/buddy_protocol.h`](main/buddy_protocol.h). Sub-encodings:
`name = [len][bytes]` (len ≤ 32), `caps = [4 bytes little-endian]` (bit *i* set ⇒
feature id *i* supported).

| # | Type | Dir | Payload |
|---|---|---|---|
| 1 | `DiscoverReq` | master → bcast | `[name]` |
| 2 | `DiscoverRsp` | buddy → master | `[name][caps]` |
| 3 | `PairReq` | master → buddy | `[name]` |
| 4 | `PairRsp` | buddy → master | `[accepted:1][name][caps]` |
| 5 | `Disconnect` | master → buddy | — |
| 6 | `DisconnectAck` | buddy → master | — |
| 7 | `FeatureQuery` | master → buddy | — |
| 8 | `FeatureList` | buddy → master | `[count]{[id][name]}` |
| 9 | `FeatureStart` | master → buddy | `[id][arg_len][args]` |
| 10 | `FeatureStop` | master → buddy | `[id]` |
| 11 | `FeatureStatus` | buddy → master | `[id][state][len][data]` |
| 12 | `PcapFrame` | buddy → master | *(reserved/unused — handshakes go via `Result`)* |
| 13 | `Result` | buddy → master | `[id][frag_idx][frag_cnt][chunk]` (fragmented, reliable) |
| 14 | `ResultAck` | master → buddy | `[id]` |

Types **1–6 are identical to the master's `MeshWireType`** and must not change.
Types **7–14 are extensions** the master implements. A `Result` reassembles (over
its fragments of one `id`) into
`[type:1][ssid_len:1][ssid][frame_count:1]{ [msg_num:1][len:2 LE][bytes] }`
(msg_num 0 = beacon, 1..4 = EAPOL M1..M4). The buddy keeps re-sending it until the
master acks (`ResultAck`), so a captured handshake survives master absence and
buddy reboot (durable store, NVS).

## Code layout

```
main/
  buddy_config.h     all compile-time knobs
  buddy_protocol.h   wire format contract (shared with the master)
  buddy_espnow.[ch]  WiFi STA + ESP-NOW transport, RX worker task, send/peers
  buddy_store.[ch]   NVS persistence of the paired master
  buddy_node.[ch]    the brain: pairing state machine + command dispatch
  buddy_features.[ch] feature registry + capability bitmask
  buddy_hs_store.[ch] durable per-BSSID handshake store (RAM + NVS, retain-until-ack)
  buddy_hs_parser.[ch] 802.11 / EAPOL parsing for handshake capture
  features/
    feat_identify.c    id 0 — blink onboard LED to physically locate the buddy
    feat_capture_hs.c  id 2 — passive WPA handshake capture → buddy_hs_store
```

## Adding a feature

1. Create `main/features/feat_<x>.c` exporting a `const BuddyFeature` with a
   unique `id` (0–31; the id is also its capability bit). `start()`/`stop()`
   run in the worker task and report via `buddy_node_feature_status()`.
2. Add the source to `main/CMakeLists.txt`.
3. `extern` it and add it to the array in `main/buddy_features.c`.

The new capability is then advertised automatically in the bitmask and in the
`FeatureList` response.

## Master integration (done)

The T-Embed master drives all of this from the desktop service
(`applications/services/desktop/`): `mesh_service.c` parses `caps`, sends
`FeatureQuery`/`Start`/`Stop` + `ResultAck`, and reassembles fragmented `Result`s;
`mesh_capture.c` writes the `.pcap` (one file per net, `/ext/wifi/buddy_<name>_<ssid>.pcap`).
The mesh scenes (client menu → Device / Wifi → Handshake) expose Identify,
Disconnect and handshake capture. A delivered handshake pops a 3 s "Handshake
received" overlay (reliable `Result`/`ResultAck`).
