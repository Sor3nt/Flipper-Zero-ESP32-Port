> Release reference. For current source builds and confirmed wiring, see
> [the setup guide](../T-EMBED-BW16-SETUP.md) and [feature status](../T-EMBED-FEATURES.md).
> Fully Power Off with Lilygo USB removed before connecting BW16; Deep Sleep
> is insufficient. SD retries do not make hot-plugging reliable.

# T-Embed CC1101 Plus SD Recovery 1.3.1

This separate **Plus / installed-NRF** bundle changes the T-Embed system firmware to improve SD startup and recovery. Every supplied SD file, including WDGWars Map 1.3, Wardriver Plus and the BW16 I2C app, and the entire BW16 firmware folder are byte-identical to the working Plus WDGWars 1.3 release. Existing Plus, non-NRF and non-NRF I2C bundles stay separate.

The recovery profile is enabled only in this new build. It retains NRF CE43/CSN44 and BW16 I2C SDA8/SCL18. This is a firmware recovery change; it does not establish that a power dip, loose connection or electrical fault has been fixed. Physical verification on your Plus remains necessary.

## Recovery behavior

- The Plus boot path deselects the SD card, display and radios before enabling shared-bus traffic. After a 100 ms settling delay it initializes the SD card into SPI mode before the radio/display startup. A failed early SD attempt is logged and boot continues.
- Initial card probing remains at the ESP-IDF 400 kHz clock. The maximum negotiated transfer-clock cap starts at 20 MHz, then falls back to 10 MHz and 4 MHz on fresh initialization attempts after failures. A successful lower rate remains in use until the SD driver is fully released or the board restarts. Each HAL request makes only one initialization attempt and cleans up its own device registration before a retry.
- The normal initial mount retains its existing three tries. If that automatic mount still fails with a recoverable error, the storage service makes at most **six additional attempts**, each at least **five seconds after the previous attempt completes**. It waits while files/directories are open or USB mass storage owns the card. Recovery is not an endless retry loop.
- Explicit mount, unmount/eject or format actions cancel the pending automatic cycle. A card reporting no filesystem stops automatic recovery without formatting. A successful recovered mount publishes the normal mounted event so existing apps can see it.
- Entering USB mass storage first serializes cancellation/unmount through the storage service. A busy card or failed filesystem detach blocks that handoff rather than exposing the same card to local recovery and USB at once.

The recovery path never formats the card, toggles the board power rail, or disables brownout protection. USB boot logs include the reset reason; a detected brownout is reported explicitly. These changes address firmware startup and retry handling, while logs are still needed to establish the cause of any continuing reset.

## Install the T-Embed update

**If you already have Plus WDGWars 1.3 and BW16 I2C 4.5, flash only the T-Embed.** The SD apps and BW16 image have not changed. Keep the existing SD card, settings, offline maps, API credentials, logs and captures.

1. Extract `T-Embed-CC1101-Plus-SD-Recovery-1.3.1.zip`. Disconnect BW16 wiring while flashing the T-Embed.
2. From the extracted directory, run with your T-Embed's current COM port:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\T-Embed\FLASH.ps1 -Port COM20
   ```

   COM20 is an example. The script verifies image hashes and writes the individual firmware regions without a whole-chip erase. It does not format or write the SD card. Close any serial monitor before flashing. If the port is unavailable, use the device's BOOT/reset procedure and the resulting port.
3. Power off before reconnecting BW16. Leave the NRF installed. Start with the existing SD inserted, then check SD access and the existing apps.

A complete SD payload is included for a fresh installation: merge the contents of `T-Embed/SD/` into the card while preserving your own `apps_data` files. No SD replacement is required when upgrading from Plus WDGWars 1.3.

The unchanged BW16 image is included for recovery or an initial I2C installation. A BW16 already running I2C 4.5 needs no reflash. A BW16 still running UART firmware requires the matching I2C firmware before using these wires. For that case, disconnect it from the T-Embed and flash through its USB using its current port:

```powershell
powershell -ExecutionPolicy Bypass -File .\BW16\FLASH_BW16.ps1 -Port COM3 -BaudRate 460800
```

COM3 is an example. Follow the script's BURN/reset upload-mode prompt.

## Confirmed wire assignments

Switch power off before moving either end of a wire. The labels below are authoritative; the colours refer to your assigned four-wire connection.

| Wire | T-Embed CC1101 Plus | BW16-KIT |
| --- | --- | --- |
| **White** | **GPIO8 / SDA** | **PA26 / D8 / SDA** |
| **Yellow** | **GPIO18 / SCL** | **PA25 / D7 / SCL** |
| **Red** | **3.3 V** | **3V3** |
| **Black** | **GND** | **GND** |

The link is I2C address `0x42` at 100 kHz. SDA connects to SDA and SCL connects to SCL; these are not the old UART TX/RX pins. These assignments supersede older yellow/white colour references. GPIO8/18 are also used by the board's shared I2C devices.

GPIO43/44 remain NRF control pins on the Plus. Do not connect GPS or BW16 UART to them with the NRF installed. The preserved Wardriver Plus/map guards reject those GPS assignments. The map can use manual coordinates and saved offline areas without GPS. See `WDGWARS-MAP-1.3-REFERENCE.md` for the unchanged map, key-file, offline-area and control details.

## Check SD recovery and collect USB logs

The recovery logic avoids formatting the card. Test with your existing card and the same wiring that previously reproduced the problem. Check a cold start with the card already inserted, opening the file browser, and opening the unchanged Wardriver or map FAP. Keep a copy of important card files before intentionally reproducing resets.

If the board resets or the card stays unavailable, capture the **T-Embed USB console** from before a reset through the SD failure and recovery attempt. Use a serial terminal on the T-Embed port at 115200 baud, with DTR/RTS toggling disabled if your terminal offers that setting, then press the rear reset button once. Save the complete boot log rather than only the last line. USB Serial/JTAG console is configured; UART43/44 is not the console.

Useful evidence includes the reset reason, the new recovery-profile startup messages, SD initialization/mount errors and their numeric codes, retry/recovery messages, and whether SD access returned. Record whether BW16 was connected at power-on and whether the reset occurred before or after opening its app. Close the serial terminal before attempting another flash. A log may contain network names or device details; review it before sharing publicly.

## Build and validation

The isolated build uses ESP-IDF 5.4.1, `sdkconfig.plus_sd_recovery` and `build_t_embed_plus_sd_recovery`. Its generated configuration differs from the Plus WDGWars profile only by `CONFIG_T_EMBED_PLUS_SD_RECOVERY=y`; BW16 I2C stays enabled and external UART43/44 stays disabled.

```powershell
python tools\build_t_embed_plus_sd_recovery.py
python tools\validate_t_embed_plus_sd_recovery.py
python tools\package_t_embed_plus_sd_recovery.py
```

The package requires a fresh main build, a passing recovery test report with source hashes, and successful compatibility checks for the three unchanged FAPs against the actual new firmware ELF. It verifies all protected prior files and the complete SD/BW16 payload, runs the flash scripts in verification-only modes, and checks ZIP CRC and every archived byte.

`T-Embed/build-reference/VALIDATION.json` records the new recovery checks. The reports under `inherited-plus-1.3/` describe the previous unchanged map FAP's host tests and authenticated-response replay; they are not new network or hardware tests. `INHERITED_VALIDATION.json` states that distinction. Private keys and authenticated response bodies are not included. `hardware_validated` remains false until this firmware is tested on the physical board.