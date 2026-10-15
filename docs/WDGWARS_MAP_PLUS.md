> Release reference. For current source builds and confirmed wiring, see
> [the setup guide](../T-EMBED-BW16-SETUP.md) and [feature status](../T-EMBED-FEATURES.md).
> Fully Power Off with Lilygo USB removed before connecting BW16; Deep Sleep
> is insufficient. SD retries do not make hot-plugging reliable.

﻿# WDGWars Map 1.3 for T-Embed CC1101 Plus

This is the separate **Plus / installed-NRF** release. It adds the native WDGWars map to the Plus firmware while retaining the existing Wardriver Plus app, BW16 I2C 4.5 app and firmware, RF Waterfall and other supplied SD apps. The older Plus 4.5 and non-NRF release bundles remain separate.

## Install

**Flash the T-Embed system firmware in this bundle before opening the new map FAP.** The old Plus 4.5 system is missing four API exports required by the map. Copying only the FAP onto that old firmware is insufficient. This release intentionally provides a full bundle rather than a Plus SD-only update.

1. Extract `T-Embed-CC1101-Plus-WDGWars-1.3.zip`. Disconnect BW16 wiring while flashing the T-Embed.
2. From the extracted bundle directory, flash the T-Embed using its current port:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\T-Embed\FLASH.ps1 -Port COM20
   ```

   COM20 is an example. If necessary, hold the encoder centre (BOOT), press and release rear RST, then release BOOT and use the newly listed port. The supplied script checks image hashes and writes individual firmware regions; it does not perform a full-chip erase or format the SD card.
3. Merge the contents of `T-Embed/SD/` into the SD card. Keep existing `apps_data` settings, logs, captures, offline areas and API credentials. The new map file is:

   ```text
   apps/Tools/wdgwars_map.fap
   ```

4. Safely eject the card, reconnect the existing Plus I2C wiring with power off, and open **Tools > WDGWars Map**.

**BW16 already running Plus I2C 4.5 does not need reflashing.** Its entire firmware folder and existing BW16 I2C FAP are copied unchanged from the working Plus bundle. For a fresh BW16 installation, use the included matching firmware and `BW16-I2C-4.5-REFERENCE.md`; that reference describes the earlier initial installation of both boards. The original UART BW16 firmware is a different transport.

## Plus wiring and GPS

Keep the NRF installed. GPIO43 is NRF CE and GPIO44 is NRF CSN; these pins are unavailable for GPS or BW16 UART. The Plus system initializes the NRF control pins before shared SPI traffic. BW16 remains on the shared I2C link:

| T-Embed Plus | BW16-KIT |
| --- | --- |
| 3.3 V | 3V3 |
| GND | GND |
| GPIO8 / SDA | PA26 / D8 / SDA |
| GPIO18 / SCL | PA25 / D7 / SCL |

The existing link uses address `0x42` at 100 kHz. Follow signal labels, not wire colours. GPIO8/18 remain reserved for I2C, including battery management and NFC.

The map reuses the saved Wardriver settings and the **guarded Wardriver Plus GPS driver**. It rejects GPS configurations using GPIO43/44 or GPIO8/18 before installing a UART or changing those pins. Existing non-NRF RX44/TX43 settings therefore cannot seize the NRF pins. This Plus app never assigns RX44 automatically.

Use GPS only with explicitly configured, suitable UART pins and physically isolated wiring selected through Wardriver Plus. If GPS is off, unconfigured or rejected, the map still works: choose **Set map coordinates**, open a saved area, or use **Whole world overview**. Moving BW16 to I2C does not relocate the GPS connector. Reusing the old UART43/44 GPS wiring requires the separate non-NRF hardware/profile.

## Map features

- A native colour map with rotary zoom, pan, GPS status, current position and cached-data age.
- An immediate offline whole-world view with real simplified coastlines. Zoom 0 fits all continents and both poles; regional views start at zoom 1.
- WDGWars gang ownership colours, names and returned polygons. Separate polygons belonging to one gang stay separate. Detailed cells use coloured markers because their exact boundary anchor convention has not been verified.
- Online street/context downloads from zoom 9, including supported water and park features. Wide views prioritize major roads; closer views request local streets.
- Four SD offline areas, cached account statistics, independent street/territory result details and manual coordinate entry.

The map is a separate FAP; it does not replace Wardriver or run its scan loop. Existing Wardriver Wi-Fi/BLE scanning, notable-device detection, logging, WiGLE and WDGWars upload remain in the unchanged Wardriver Plus app. The map renders natively on the ESP32; it embeds no browser or web frontend.

Version 1.3 also improves street requests: nearby streets are requested and returned first, before the larger surrounding query. Complete usable geometry returned with an Overpass timeout/resource remark can be retained as an explicitly partial map. Malformed or incomplete HTTP/JSON data still leaves the previous cache intact, and street versus territory outcomes are shown separately.

## Online loading and territory service

Connect through the firmware Wi-Fi interface first. Missing map data loads automatically after a map position is available and input has paused briefly. Streets become visible before the independent territory request finishes. Each service has a minimum one-minute interval and respects server cooldowns. Failed unchanged views do not retry repeatedly; use **Update map + territories** to try again. **Online map loading** can be switched off for offline browsing.

Zooms below 6 use the live `/api/territories` global overview. Zoom 6 and closer requests viewport-filtered `/api/member-territories?compact=1&bbox=W,S,E,N&zoom=Z` data. A cached global overview does not masquerade as downloaded detailed cells. Names and colours come from the service; geographic backgrounds do not imply territory ownership.

The prior unsupported-territory error was reproduced using actual authenticated responses. A small local response included a much larger global gang directory, which overflowed the old limit. The parser now validates that directory and retains referenced gangs. The world view uses the dedicated smaller overview endpoint. The bounded model supports 1,024 cells, 1,024 gangs, 2,048 hulls and 32,768 vertices, with a 1 MiB WDGWars response limit. Capacity errors keep the previous map.

The existing key stays in:

```text
apps_data/wardriver/wdgwars.conf
```

Its format is:

```text
API Key: YOUR_64_CHARACTER_WDGWARS_API_KEY
```

The unchanged credential loader reads it from the card. The key is sent only to WDGWars using certificate-verified HTTPS. Streets and the built-in world coastlines need no WDGWars key. Firmware, bundled source and diagnostics contain no key or authenticated response bodies.

## Offline storage and controls

Downloaded areas are stored at `apps_data/wdgwars_map/area1.wdg` through `area4.wdg`. **Copy downloaded map to Area N** requires actual downloaded geometry. CRC, size and geometry checks protect reads; temporary files and a backup protect complete saves. Legacy map caches can be read and migrated; new saves use cache version 2. Cache age represents download age, not a guaranteed server-generation timestamp. Account-only data is never presented as a downloaded map.

- Turn to zoom; click for actions. A failed/missing map can offer click-to-retry.
- **Whole world overview** opens zoom 0; **Center on GPS** follows a valid fix.
- **Pan map** uses turn for north/south and hold-plus-turn for east/west.
- **Layers** selects all, territories or streets. **Territory near map center** describes a nearby cell or returned outline.
- **Offline areas** opens or copies areas. **Last download details** shows each service's outcome.
- Back returns, leaves pan mode, cancels a download or exits. Cancelling pauses automatic loading; an SD save already committing finishes safely.

## Sources and verification

World land data is simplified [Natural Earth 1:110m land](https://www.naturalearthdata.com/downloads/110m-physical-vectors/110m-land/), distributed as [public domain](https://www.naturalearthdata.com/about/terms-of-use/). Source data and provenance are included under `source/tools/data/wdgwars/`.

Street/context vectors are **(c) OpenStreetMap contributors**, [ODbL 1.0](https://www.openstreetmap.org/copyright), fetched through the public Overpass service. The layer remains bounded to 256 paths and 4,096 vertices; dense areas may be partial. It provides map context rather than turn-by-turn routing or a complete address/POI database. See `WDGWARS_BASEMAP_SOURCES.md` for coverage and service limits. It never bulk-downloads standard raster tiles.

Territory and account information comes from [WDGWars](https://wdgwars.pl/press). The release contains build/source checksums, actual Plus ELF/FAP compatibility checks, HTTP/parser/cache/input/GPS-guard tests and authenticated-response replay summaries. Preserved Wardriver Plus and BW16 I2C apps are checked against the new Plus firmware. Flash scripts run in verification-only mode during packaging, and ZIP members are checked against their source files. No device is flashed by the build or packaging tools; physical validation of this Plus release remains pending.
