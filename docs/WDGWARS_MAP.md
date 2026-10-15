# WDGWars Map 1.3 - street loading and Plus support

This is an update to the separate, native WDGWars FAP for the **non-NRF T-Embed CC1101**. The existing Wardriver and BW16 applications are unchanged. A separate Plus/NRF bundle is documented in WDGWARS_MAP_PLUS.md; do not flash this non-NRF bundle on the Plus.

## Install the update

If the 1.0, 1.1 or 1.2 map app already runs on your T-Embed, **no firmware flash is needed**. Close the app, replace `apps/Tools/wdgwars_map.fap` on the SD card with the new FAP, then reopen it. The custom WD icon is embedded in that file. Keep your existing `apps_data` files and API key.

`WDGWars-Map-1.3-SD-Update.zip` contains the small SD update. The complete `T-Embed-NonNRF-WDGWars-1.3.zip` also includes the same compatible T-Embed firmware and unchanged working BW16 firmware for a fresh installation. Merge its `T-Embed/SD` directory into the card; do not format or replace your existing data directory. Previous release bundles remain preserved.

## Whole-world map and territories

Choose **Whole world overview** from the encoder menu, or zoom all the way out to **GLOBE (zoom 0)**. All continents and both poles fit on screen. Real, simplified Natural Earth coastlines are built into the FAP, so the geographic overview works immediately **offline**. Regional map views continue from zoom 1; detailed streets download at zoom 9 and closer.

Online mode uses the live `GET /api/territories` overview at zooms below 6. Its returned gang polygons, colors and names are rendered directly; separate polygons for one gang remain separate. Zoom 6 and closer uses the viewport-filtered `/api/member-territories?compact=1&bbox=W,S,E,N&zoom=Z` endpoint for detailed cells and outlines. A cached overview does not count as downloaded detailed cells when you zoom in.

The 1.1 error was reproduced against the actual authenticated service: a small viewport returned only 5 cells and 2 hulls, but also a global directory of 583 gangs. The old 64-gang limit rejected that valid response. Version 1.2 resolves and retains relevant gang entries from that directory regardless of JSON field order. The old world member request also returned approximately 3.9 MB and 20,886 hulls. The dedicated world overview returned approximately 678 kB with 1,478 hulls, 465 gangs and 12,586 vertices during verification. These are observations from 29 September 2026, not fixed expectations for future API results.

The bounded PSRAM model now supports 1,024 cells, 1,024 gangs, 2,048 hulls and 32,768 hull vertices, with a 1 MiB WDGWars response limit. Overpass remains limited to 512 KiB. Capacity or malformed-data errors keep the previous snapshot and identify the failing field or limit. The exact plain-cell anchor convention remains unverified, so cells use colored markers; server polygon coordinates are used directly. No gang data is invented or distributed as live content.

## Online and offline loading

Version 1.3 addresses Overpass incomplete responses: the street query emits nearby roads before querying the wider area and uses a finite 32 MiB server memory budget. The local response limit remains 512 KiB. Recognized server timeout/memory notices with useful geometry in a complete, valid JSON response are displayed as **partial streets**, with the actual reason in Last download details. Empty, malformed, truncated or other error responses retain the old map. There are no automatic retry loops. The public map service can still time out under load.

**256 paths is the intentional compact street-cache limit**, separate from WDGWars cells and gang outlines. It means the street map can be partial in a dense area; zooming in requests a smaller area with more local detail. Territory counts are independent of street-download status. The revised query was verified with a real 4 km city request returning HTTP 200 and no Overpass error, plus recorded timeout/memory regression cases.

The app automatically loads missing map information after Wi-Fi and a map position are available. A short pause after turning/panning avoids requests on every input. Moving or zooming outside cached coverage schedules the appropriate bounded update. Each service is limited to at least one request per minute, honors server rate limits, and does not automatically retry a failed unchanged view.

Street data loads first, becomes visible, then WDGWars territories load independently. A slow or rejected WDGWars request does not hold the street map off screen. Switch **Online map loading** off to browse saved areas without automatic requests. **Update map + territories** remains an explicit manual action. Whole-world coastlines never require a request to a street-map server.

The old app could save account/center metadata and call it a saved map. Version 1.1 distinguishes that from actual geometry. **Copy downloaded map to Area N** refuses account-only/empty data. An empty server map response is identified as empty; it is not labeled as a successful visible map. **Last download details** shows street and territory results separately, with path/cell/outline counts. Loading, offline, outside-coverage and network-error states are explicit.

Four offline slots remain at `/ext/apps_data/wdgwars_map/area1.wdg` through `area4.wdg`. Version 1.0/1.1 cache files are CRC-checked and expanded in memory when read. New saves use cache version 2; the previous complete file remains available as the backup. Older app versions cannot read the new version 2 file directly. CRC/size/geometry checks and temporary/backup writes protect existing data. Cache freshness means download age, not guaranteed server generation time. SD-save failures are distinct from successful in-memory downloads. Account data and keys are never treated as map geometry.

## Key and GPS

The WDGWars key is read from the existing SD file:

```text
apps_data/wardriver/wdgwars.conf
```

Contents:

```text
API Key: YOUR_64_CHARACTER_WDGWARS_API_KEY
```

The key remains on your card. Street maps and built-in coastlines need no key. The app uses the unchanged Wardriver credential loader and does not overwrite Wardriver settings.

GPS reuses the existing Wardriver configuration. If no pins are configured, enabling GPS in this non-NRF app uses UART2 RX GPIO44 at 9600 baud, receive-only. GPS and BW16 use the same header separately. A fix is optional: use **Set map coordinates**, open an offline area or select the whole-world view. **Center on GPS** returns to your position at street zoom. Startup cache loading preserves an arriving live GPS fix.

## Controls

- Turn to zoom; click for the action menu. A missing/error map may offer direct click-to-retry.
- **Whole world overview** opens zoom 0; **Center on GPS** returns to your location.
- **Pan map**: turn north/south; hold and turn east/west. Back leaves pan mode.
- **My account & statistics** shows cached player, gang and Wi-Fi/BLE/aircraft/mesh/total counts. Click there to refresh it.
- **Territory near map center** shows a nearby cell or returned gang outline.
- **Layers** selects all, territories or streets.
- **Offline areas** opens or copies downloaded areas. **Last download details** separates map and territory status.
- Back returns from menus, cancels a download or exits. Cancelling pauses automatic loading. An SD commit already underway finishes safely.

## Geographic detail and sources

The world background uses simplified **Natural Earth 1:110m land**, public domain: [dataset](https://www.naturalearthdata.com/downloads/110m-physical-vectors/110m-land/), [terms](https://www.naturalearthdata.com/about/terms-of-use/). The source GeoJSON and SHA-256 provenance are included under `source/tools/data/wdgwars/`. Regenerate with `tools/generate_wdgwars_world.py`; no network download occurs during compilation.

Streets and supported water/park features use **(c) OpenStreetMap contributors**, [ODbL 1.0](https://www.openstreetmap.org/copyright), through Overpass. Zooms 9/10/11/12 cover up to approximately 50/30/16/8 km; zoom 13+ covers up to 4 km. Queries prioritize central streets, then outer roads and context. Local storage remains bounded to 256 paths and 4096 vertices; dense areas are marked partial. Routing, addresses and complete navigation POIs are not included. See `WDGWARS_BASEMAP_SOURCES.md` for geometry and service details. No global Overpass or bulk raster-tile requests are made.

Territory/account data comes from [WDGWars](https://wdgwars.pl/press). HTTPS certificate and hostname checks remain enabled; the WDGWars key is sent only to WDGWars. Overpass receives only the requested bounding box/query.

## Verification

Validation covers the produced FAP against the existing firmware, WD icon, globe projection, real street responses, HTTP framing, cache faults and app control flows. The territory fix is additionally checked with the actual authenticated local and global responses through the production parser/client adapter and native renderer. The API key is never included in diagnostics, source, firmware or release files; authenticated response bodies remain outside the distribution. No device was flashed. Physical testing of version 1.3 remains necessary. GPS/config wrapper tests also cover the non-NRF and Plus profiles, including reserved pins and old saved configurations.
