#pragma once
#include "wdgwars_map_model.h"
#include "wdgwars_map_basemap.h"
#define WDG_CACHE_SLOTS 4U
#define WDG_CACHE_PATH "/ext/apps_data/wdgwars_map"
#define WDG_CACHE_VERSION 2U
#define WDG_CACHE_V1_SIZE 114736U
#define WDG_CACHE_TERRITORIES 1U
#define WDG_CACHE_BASEMAP 2U
#define WDG_CACHE_ACCOUNT 4U
#define WDG_CACHE_CENTER 8U
/* No credentials in this payload. Version/size/CRC checked before use. */
typedef struct {
    double center_lat, center_lon, zoom;
    uint32_t flags, account_at;
    WdgMapSnapshot territories;
    WdgMapBasemap basemap;
    WdgMapAccount account;
} WdgMapSavedArea;
/* Frozen shipped v1.0/v1.1 layout. Do not replace literal array sizes with
 * current model limits. Named 80-byte hulls are part of that on-disk format. */
typedef struct {
    WdgMapBounds bounds;
    uint32_t downloaded_at, source_epoch;
    int32_t grid_lat_e6, grid_lon_e6;
    uint16_t cell_count, gang_count, hull_count, point_count;
    uint8_t zoom;
    bool source_time_valid, cells_skipped;
    WdgMapCellAnchor cell_anchor;
    WdgMapCell cells[1024];
    WdgMapGang gangs[64];
    WdgMapHull hulls[128];
    WdgMapPoint points[4096];
} WdgMapSnapshotV1;
typedef struct {
    double center_lat, center_lon, zoom;
    uint32_t flags, account_at;
    WdgMapSnapshotV1 territories;
    WdgMapBasemap basemap;
    WdgMapAccount account;
} WdgMapSavedAreaV1;
typedef struct { uint32_t magic, version, length, crc; } WdgCacheHeader;
/* Returns 0 for unknown versions or mismatched lengths before allocation. */
size_t wdg_cache_payload_size(const WdgCacheHeader* header);
/* Validates CRC and version-specific ranges, then converts to current layout.
 * Output is worker-owned scratch storage, never a displayed active snapshot. */
bool wdg_cache_decode(const WdgCacheHeader* header, const void* payload,
    size_t length, WdgMapSavedArea* scratch);
uint32_t wdg_cache_crc(const void* data, size_t length);
bool wdg_cache_area_valid(const WdgMapSavedArea* area);
void wdg_cache_header(WdgCacheHeader* header, const WdgMapSavedArea* area);
bool wdg_cache_verify(const WdgCacheHeader* header, const WdgMapSavedArea* area);
/* Worker-only. Load output is scratch storage, not the displayed snapshot. */
bool wdg_cache_load(unsigned slot, WdgMapSavedArea* scratch);
bool wdg_cache_save(unsigned slot, const WdgMapSavedArea* area);
