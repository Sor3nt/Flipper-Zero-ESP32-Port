# Build WIFITE 0.8 for T-Embed T-Embed Native Tools

Full modified project source. No Raspberry Pi is involved.
Firmware version: 2.0.0-tembed-tools. App name: WIFITE.

Use Linux or WSL with ESP-IDF **5.4.1**, including its submodules and installed
ESP32-S3 toolchain. The SDK is a separate dependency, not bundled here.
This version intentionally checks the exact SDK version for its driver bridge.

From the extracted project directory:

```sh
. /path/to/esp-idf/export.sh
python -m pip install idf-component-manager==2.2.2 pillow
idf.py -B build_wifite -D SDKCONFIG=sdkconfig.wifite \
  -D FLIPPER_BOARD=lilygo_t_embed_cc1101 build
FAP_FW_BUILD_DIR=build_wifite FAP_APP_VERSION_OVERRIDE=8 \
  bash buildFap.sh applications_user/wifite
```

Outputs:
- Firmware: `build_wifite/furi_esp32.bin`
- App: `build_wifite/fap/wifite.fap`
- Bootloader: `build_wifite/bootloader/bootloader.bin`
- Partition table: `build_wifite/partition_table/partition-table.bin`
- OTA initializer: `build_wifite/ota_data_initial.bin`

Flash the matching firmware, then copy `wifite.fap` to `apps/WiFi/` on the SD card.
From the project directory, with your device port substituted:

```sh
idf.py -B build_wifite -p /dev/ttyACM0 flash
```

Manual offsets are 0x0 for bootloader, 0x8000 for partition table, 0x10000 for
firmware and 0xbf0000 for OTA initializer. ESP32-S3, 16 MB flash, DIO, 80 MHz.
Retain the existing partition layout and SD files.

Main changed code:
- `applications_user/wifite/`: FAP UI, scrolling terminal and launcher icon.
- `components/wifi/wifite/`: native WPS core, Pixie search, PIN progression,
  driver connection bridge and radio worker.
- `components/wifi/wifite.h`: resident firmware API used by the FAP.
- `tests/wifite/`: synthetic host protocol tests.
- `sdkconfig.wifite`: matching build configuration.

The firmware build and synthetic core tests passed. The revised connection
path has not been tested on a physical T-Embed or router; live success is not
verified. See `WIFITE.md` for scope, controls, limits and host test commands.
The firmware's existing configure step patches the SDK raw-TX sanity check
for other tools; use a dedicated SDK checkout if you want to isolate it.

The WPS source adapter pins the unmodified v5.4.1 esp_wps.c hash
`bd8a19e9e3f10def466718088230938c79171f80326de4111bcd3c980d20fa82`.
CMake generates build_wifite/wifite_sdk/esp_wps.c and compiles it on the original
wpa_supplicant target. No manual SDK source editing is needed.

Validation commands:
```sh
python3 tests/wifite/test_link.py
IDF_PATH=/path/to/esp-idf python3 tests/wifite/test_sdk_adapter.py
```
