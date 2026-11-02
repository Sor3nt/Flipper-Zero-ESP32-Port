# ESP Flasher (ESP32 port)



Port of [0xchocolate's ESP Flasher](https://github.com/0xchocolate/flipperzero-esp-flasher)

as shipped in [Momentum-Apps](https://github.com/Next-Flip/Momentum-Apps/tree/55a446b1b01bf2a2f98161d704e62cc47075ad30/esp_flasher)

(v1.9), built into the firmware. It flashes ESP chips over the board's external UART using

Espressif's [esp-serial-flasher](https://github.com/espressif/esp-serial-flasher) (vendored in

`lib/`, Apache-2.0).



## Wiring



| Board pin | Target |

|---|---|

| UART TX (GPIO43 on paradoxt / smoochie / T-Embed) | RX (U0RXD) |

| UART RX (GPIO44) | TX (U0TXD) |

| GND | GND |

| GPIO11 (paradoxt, optional) | EN |

| GPIO12 (paradoxt, optional) | BOOT strap (IO0; the pin on the target's BOOT button) |



The exact pins are listed in the app under *About*. The UART pads are shared with RFID, the

USB-UART Bridge and Expansion modules; the app claims them through `furi_hal_uart_shared` and

disables Expansion while it runs.



The target needs its own power. *Settings > VBUS 5V* turns on the board's 5 V boost (OTG) for

targets powered from it; it is switched off again when the app closes.



## Download mode



*Settings > Reset pins*:



- **Off**: hold BOOT, tap RESET on the target. The app waits ~10 s for it.

- **esptool**: EN/IO0 active low, idle high. Works for EN/IO0 wired directly and for the

  classic esptool two-transistor auto-reset circuit (RTS -> EN, DTR -> IO0).

- **Flipper**: active high, idle low, for Flipper-style boards whose transistors are driven

  by the Flipper's GPIO.



## What changed against upstream



- UART goes through the ESP-IDF driver instead of `furi_hal_serial` (not implemented on this

  port); the console RX thread is parked while the flasher owns the UART.

- DTR/RTS (Flipper PC1/PC3/PB2/PC0) and the SWCLK + OTG power-cycle trick do not exist here;

  replaced by the EN/IO0 lines above.

- The connected chip is detected and checked. Manual Flash *Target: Auto-detect* picks the

  bootloader offset from the chip (0x1000 ESP32/S2, 0x0 S3/C3/C6/H2/C2, 0x2000 C5/P4) unless an

  address was entered by hand; any other Target (and every Quick Flash entry) aborts on a

  different chip before writing anything.

- Every file is MD5-verified after writing (ROM `SPI_FLASH_MD5`; skipped on ESP8266).

- Missing SD files are reported before the target is touched.

- **ESP32-C5**: Quick Flash entries, and the ROM crystal quirk handled: the C5 ROM can assume a

  40 MHz crystal on a 48 MHz part (or the reverse), which puts the turbo baud rate ~20% off.

  The app reads `PCR_SYSCLK_CONF` and the UART divider and corrects the requested rate, as

  esptool's `ESP32C5ROM.change_baud()` does.

- Library: `MD5Init/Update/Final` renamed (the ESP32-S3 ROM defines the same symbols and

  ESP-IDF routes `esp_rom_md5_*` to them); an unknown chip ID no longer indexes past

  `esp_target[]`.

- Thread-safety: console writes are serialized and the text buffer never reallocates under

  the text box; console refresh events are coalesced (a full event queue blocks forever).



## SD card layout (Quick Flash)



The app itself is built into the firmware (no `.fap`). Quick Flash only needs the firmware

images on the SD card, under `/ext/apps_data/esp_flasher/assets/`:



```

marauder/s2/      Marauder "flipper" build (Flipper WiFi Devboard, ESP32-S2)

marauder/wroom/   Marauder "marauder_dev_board_pro" build (ESP32)

marauder/s3/      Marauder "multiboardS3" build (ESP32-S3)

marauder/c5/      Marauder "esp32c5devkitc1" build (ESP32-C5, needs 8 MB flash)

custom/c5/        your own ESP32-C5 build

    bootloader.bin   -> chip bootloader offset (0x1000 / 0x0 / 0x2000, picked per chip)

    partitions.bin   -> 0x8000

    boot_app0.bin    -> otadata offset from partitions.bin (optional in custom/c5)

    firmware.bin     -> first app partition from partitions.bin

flipperhttp/s2/flipper_http_bootloader.bin, flipper_http_partitions.bin, flipper_http_firmware_a.bin,

               boot_app0.bin (optional: otadata selecting ota_0)

blackmagic/s2/bootloader.bin, partition-table.bin, blackmagic.bin

wardriver/f0-wardrive-wroom.bin, f0-wardrive-s3.bin   (merged images)

```



The Marauder folders are the four files of one board from the official release's

`marauder-installer-assets.zip` (`*.bootloader.bin`, `*.partition-table.bin`,

`*.ota-data.bin`, app `.bin`), renamed. FlipperHTTP, Black Magic and Wardriver are the

files from the upstream app's `resources/`. Before writing, the app checks that every file

exists, that the connected chip matches, and that the partition layout fits the target's

flash. Firmware at other offsets: Manual Flash with *Advanced Mode*.
