> Release reference. For current source builds and confirmed wiring, see
> [the setup guide](../T-EMBED-BW16-SETUP.md) and [feature status](../T-EMBED-FEATURES.md).
> Fully Power Off with Lilygo USB removed before connecting BW16; Deep Sleep
> is insufficient. SD retries do not make hot-plugging reliable.

# Non-NRF T-Embed CC1101: BW16 I2C + WDGWars Map 1.3

This separate release moves **BW16 communication to I2C** on the non-NRF T-Embed CC1101 while retaining the original GPS UART on GPIO43/44. It includes a new T-Embed system image, WDGWars Map 1.3, the unchanged BW16 I2C 4.5 firmware/controller FAP, and the original non-NRF Wardriver FAP. Previous UART and Plus/NRF bundles remain separate.

**Use this profile only when NRF is electrically absent from GPIO43/44.** For a T-Embed CC1101 Plus with NRF installed, use the separate Plus WDGWars bundle. This profile reserves GPIO43/44 for UART and does not perform the Plus NRF control-pin initialization at boot. It is not a method of sharing those pins with an installed NRF.

## Install both matching transports

1. Extract `T-Embed-NonNRF-I2C-WDGWars-1.3.zip`. Disconnect external BW16/GPS wiring while flashing.
2. Flash the **new T-Embed system image** from the extracted bundle directory:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\T-Embed\FLASH.ps1 -Port COM20
   ```

   COM20 is an example; use the board's current USB port. The old non-NRF UART firmware does not enable this BW16 I2C transport, so a FAP-only replacement is insufficient. The flash script checks image hashes and writes individual firmware regions without a full-chip erase or SD format.
3. **If BW16 currently runs the UART firmware, flash the included I2C firmware too.** Connect only BW16 USB to the PC and run:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\BW16\FLASH_BW16.ps1 -Port COM3 -BaudRate 460800
   ```

   COM3 is an example. Enter BW16 upload mode by holding BURN, pressing and releasing RST, then releasing BURN. Wait for verified success, reset the board and disconnect USB before reconnecting module wires.

   A BW16 already running **I2C 4.5** needs no reflash: the firmware in this bundle is byte-identical to the existing Plus I2C 4.5 image. The BW16 transport works with either host profile; the host pin configuration is what differs.
4. Merge `T-Embed/SD/` into the card, preserving existing `apps_data` settings, logs, captures, caches and API key. Open:

   ```text
   GPIO > BW16 I2C       apps/GPIO/bw16_r4tkn_i2c.fap
   Tools > WDGWars Map   apps/Tools/wdgwars_map.fap
   ```

   Back up and remove the old UART controller `apps/GPIO/bw16_r4tkn.fap` from the SD card when installing this I2C app. The bundle omits it, but merging folders alone does not remove an existing copy or its menu entry. The original UART diagnostic probe is retained separately as a diagnostic tool, not the I2C controller.
5. Safely eject the card. With power off, connect the I2C wires below and restart the boards.

The original non-NRF Wardriver remains at `apps/Tools/wardriver.fap`, unchanged. Its Wi-Fi/BLE scanning, GPS integration, notable-device detection, logging, SD functionality, WiGLE and WDGWars upload remain in that app. RF Waterfall and the other supplied apps are retained.

## BW16 wiring

Remove the previous BW16 RX/TX wires from GPIO43/44. Connect these four signals:

| Assigned wire | Non-NRF T-Embed CC1101 | BW16-KIT |
| --- | --- | --- |
| Existing power wire | 3.3 V | 3V3 |
| Existing ground wire | GND | GND |
| White | GPIO8 / SDA | PA26 / D8 / SDA |
| Yellow | GPIO18 / SCL | PA25 / D7 / SCL |

These colours reflect the owner-confirmed cable (white SDA, yellow SCL); the printed pin labels remain authoritative. With power off, move both ends of the signal wires to these I2C pins. Both boards must run the matching I2C firmware described above. The BW16 I2C firmware does not receive commands or send replies on the previous PA7/TX0 and PB2/RX1 connection. I2C uses address `0x42`, 100 kHz and 3.3 V logic on the existing power/NFC bus. Keep the established stable 3.3 V supply and common ground.

The included `BW16-I2C-4.5-REFERENCE.md` documents the reused module transport. Its host instructions describe the earlier Plus release; **use this guide for the non-NRF host configuration and GPS pins**.

## GPS remains on the original UART

GPS keeps the non-NRF UART header:

- GPS TX goes to T-Embed RX GPIO44.
- T-Embed TX GPIO43 can connect to GPS RX when required by the existing configuration; receive-only operation leaves that signal unused.
- Keep the existing GPS power/ground arrangement appropriate for the module.

The map reuses saved non-NRF Wardriver GPS settings. Its default, when enabling unconfigured GPS, remains UART2 RX44 at 9600 baud with no TX pin assigned. The map rejects GPS assignments to GPIO8/18 because those pins now carry BW16 and the existing I2C peripherals. The original Wardriver binary itself is unchanged; keep its GPS configuration on the established UART43/44 header and do not assign it to I2C pins.

This separates the electrical connections. It does not add simultaneous background execution of the Wardriver, BW16 and WDGWars FAPs. Open the app needed for the current task. The built-in NRF tools remain inherited applications; they are not intended for use on this non-NRF UART profile.

Without GPS, choose **Set map coordinates**, open a saved area, or select **Whole world overview**. A saved area does not overwrite an arriving live GPS position during startup.

## Native map features

The map is a separate ESP32-native colour FAP, with no browser or web frontend. It provides:

- Immediate offline whole-world coastlines at zoom 0, regional views from zoom 1, and bounded street/context downloads from zoom 9.
- Live WDGWars gang colours, labels and returned polygons; multiple polygons belonging to one gang remain separate.
- Detailed territory cell markers, current GPS position/status, zoom and cached-data age.
- Four SD offline areas, manual coordinates, map panning, selectable layers, account statistics and separate street/territory download details.

Online territory overview uses `/api/territories` below zoom 6. Detailed views use viewport-filtered `/api/member-territories?compact=1&bbox=W,S,E,N&zoom=Z`. The parser handles the observed global gang directory while retaining relevant entries. Its bounded model supports 1,024 cells, 1,024 gangs, 2,048 hulls and 32,768 vertices, with a 1 MiB WDGWars response limit. Exact plain-cell boundary anchors remain unverified, so cells are markers; server polygons use their returned coordinates directly.

Version 1.3 requests and returns nearby streets before processing the wider surrounding area. Complete usable geometry returned with an Overpass timeout/resource remark can be retained as an explicitly partial map. Malformed or incomplete data keeps the old cache. Dense street data is bounded to 256 paths and 4,096 vertices; this map does not provide turn-by-turn routing or a complete address/POI database.

## Online and offline use

Connect through the firmware Wi-Fi interface. After GPS or coordinates establish a position, missing data loads automatically once input pauses. Streets appear before the separate territory request finishes. Each service has a minimum one-minute interval and honors server cooldowns; failed unchanged views are not retried repeatedly. **Update map + territories** performs a manual refresh. Disable **Online map loading** for offline browsing.

The key stays in the existing file:

```text
apps_data/wardriver/wdgwars.conf
```

With this format:

```text
API Key: YOUR_64_CHARACTER_WDGWARS_API_KEY
```

The key is sent only to WDGWars using certificate-verified HTTPS. Built-in coastlines and street downloads need no WDGWars key. Authenticated response bodies and the user's key are not included in the release.

Offline slots are `apps_data/wdgwars_map/area1.wdg` through `area4.wdg`. CRC/size/geometry checks protect reads; temporary files and a backup protect saves. Older map caches can be migrated; new saves use cache version 2. **Copy downloaded map to Area N** requires actual geometry, not merely saved account/center metadata. Cache age represents download age, not guaranteed server-generation time.

Turn to zoom and click for actions. **Pan map** uses turn for north/south and hold-plus-turn for east/west. **Center on GPS** follows a valid fix. **Territory near map center**, **Layers**, **Offline areas** and **Last download details** expose the corresponding controls. Back returns, cancels or exits; cancelling pauses automatic loading, while an SD save already committing finishes safely.

## Sources and verification

World land is simplified [Natural Earth 1:110m land](https://www.naturalearthdata.com/downloads/110m-physical-vectors/110m-land/), [public domain](https://www.naturalearthdata.com/about/terms-of-use/). Street vectors are **(c) OpenStreetMap contributors**, [ODbL 1.0](https://www.openstreetmap.org/copyright), through Overpass. Territory/account information comes from [WDGWars](https://wdgwars.pl/press). See `WDGWARS_BASEMAP_SOURCES.md` for geographic coverage and service limits. No global Overpass or bulk raster-tile requests are made.

The bundle includes build/source hashes, actual firmware/FAP ABI checks, combined I2C/UART profile tests, GPS pin checks, HTTP/parser/cache tests and authenticated territory response replay summaries. The original non-NRF Wardriver and reused BW16 I2C FAP are checked against the new firmware. Packaging verifies flash checksums without opening hardware and checks every ZIP member. Previous releases remain protected. Physical validation of this new combined transport profile remains pending.
