# Native Tools app audit — 2026-10-21

This inventory covers every `application.fam` under `applications_user`,
including inherited upstream applications. Entries indicate source presence,
not that every app was built or hardware-tested in this update.
The seven-app build selects the I2C BW16 and the relevant Wardriver variant.

| App | Source | Manifest version |
| --- | --- | --- |
| FindMy Flipper | `applications_user/FindMyFlipper/FindMyFlipper` | "3.5" |
| TagTinker | `applications_user/TagTinker-main` | "2.0" |
| Asteroids | `applications_user/asteroids` | Not declared |
| Blackjack | `applications_user/blackjack` | Not declared |
| Blackjack Split | `applications_user/blackjack_split` | Not declared |
| BW16 R4TKN | `applications_user/bw16_r4tkn` | "4.4" |
| BW16 I2C | `applications_user/bw16_r4tkn_i2c` | "4.5" |
| Dice D&D | `applications_user/dice` | "1.3" |
| Authenticator | `applications_user/flipper-zero_authenticator/totp` | "5.190" |
| XRemote | `applications_user/flipper_xremote` | "1.4" |
| Game 15 | `applications_user/game15` | Not declared |
| Mifare Fuzzer | `applications_user/mifare_fuzzer` | "1.5" |
| nRF24 Jammer | `applications_user/nRF24_jammer` | "1.4.0" |
| NFC Magic | `applications_user/nfc_magic` | "2.0" |
| RFID detector | `applications_user/nfc_rfid_detector` | "1.4" |
| Pong | `applications_user/pong` | Not declared |
| ProtoPirate 3.6.3 | `applications_user/protopirate` | "3.6.3" |
| Reverse Shell | `applications_user/reverse_shell` | "1.0" |
| RFID2 Reader | `applications_user/rfid2_reader` | "1.0" |
| Roulette | `applications_user/roulette` | "1.0" |
| RTL433 | `applications_user/rtl433` | "1.1.8" |
| Snake 2.0 | `applications_user/snake20` | "2.3" |
| Snake | `applications_user/t_embed_snake` | Not declared |
| Tamagotchi | `applications_user/tamagotchi_p1` | 0.1 |
| Tetris | `applications_user/tetris` | Not declared |
| Texas Holdem | `applications_user/texas_holdem` | Not declared |
| Walkie Talkie | `applications_user/walkie_talkie` | "1.2" |
| Wardriver | `applications_user/wardriver` | "1.5.2" |
| Wardriver Plus | `applications_user/wardriver_plus_i2c` | "1.5.2" |
| WDGWars Map | `applications_user/wdgwars_map` | "1.3" |
| Weather Station | `applications_user/weather_station` | "2.0" |
| WIFITE | `applications_user/wifite` | "0.8.1" |
| wM-Buster | `applications_user/wmbuster` | "1.0" |

## Refresh validation

- RTL433 1.1.8 complete host regression suite passed under ASan/UBSan with LeakSanitizer disabled.
- Walkie Talkie production control handlers and PCM writer host tests passed under UBSan.
- Both changed FAPs compiled for ESP32-S3 using ESP-IDF 5.4.1 headers/toolchain: RTL433 125 imports, Walkie Talkie 55 imports.
- Imports resolve against the updated source API table; Xtensa ELF type and supported allocated relocations checked. API hashes remain sorted and unique, with existing entries retained.
- Shell/Python build integration syntax and whitespace checks passed.

This refresh did not rebuild/link the complete firmware or flash hardware.
The API checks above are source-table checks, not proof of linked firmware compatibility.
The existing GitHub workflows require maintainer approval; a green upstream CI run is not claimed.
Vendored RTL433 compiler format warnings remain; this is not a warning-free build claim.

