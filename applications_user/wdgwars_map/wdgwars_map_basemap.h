#pragma once
#include "wdgwars_map_model.h"
#include "wdgwars_map_client.h"

#define WDG_BASE_RESPONSE_MAX (512U * 1024U)
#define WDG_BASE_MAX_FEATURES 256U
#define WDG_BASE_MAX_POINTS 4096U
#define WDG_BASE_MAX_WAY_POINTS 512U
#define WDG_BASE_PATH_SIZE 2048U
#define WDG_BASE_MIN_ZOOM 9U
#define WDG_BASE_REFRESH_SECONDS 60U
#define WDG_BASE_ATTRIBUTION "(c) OpenStreetMap contributors"
#define WDG_BASE_LICENSE "OpenStreetMap data: ODbL 1.0"

typedef enum {
    WdgBaseWater, WdgBasePark, WdgBaseBuilding,
    WdgBaseRoadMinor, WdgBaseRoadMajor, WdgBaseRailway
} WdgBaseKind;
typedef struct {
    uint16_t first_point, point_count;
    WdgBaseKind kind;
    bool closed;
    char name[40];
} WdgBaseFeature;
typedef struct WdgMapBasemap {
    WdgMapBounds bounds;
    uint32_t downloaded_at;
    uint16_t feature_count, point_count;
    bool partial;
    WdgBaseFeature features[WDG_BASE_MAX_FEATURES];
    WdgMapPoint points[WDG_BASE_MAX_POINTS];
} WdgMapBasemap;
typedef WdgMapBasemap WdgBasemap;

/* A centered subset of view: Z9 50km, Z10 30km, Z11 16km, Z12 8km,
 * Z13+ 4km maximum per dimension. Z9-12 query major roads only. Antimeridian
 * crossing and polar bounds are rejected. The caller shows this actual coverage. */
bool wdg_basemap_bounds(const WdgMapBounds* view, uint8_t zoom, WdgMapBounds* output);
/* Relative, percent-encoded path for the fixed Overpass HTTP service. */
bool wdg_basemap_query(const WdgMapBounds* bounds, uint8_t zoom, char* path, size_t capacity);
/* Transactional: output is unchanged on malformed JSON, fatal server errors, or OOM.
 * A timeout/memory remark may retain useful complete validated geometry only as
 * partial=true with an explicit reason in error. Empty/unknown errors never commit.
 * A valid empty area succeeds. Bounds are the requested coverage, not inferred
 * from features. Truncated/unsupported geometry is marked partial, never filled.
 * Work buffers are heap allocated; no geometry-sized arrays use the UI stack. */
bool wdg_basemap_parse(const char* data, size_t length, const WdgMapBounds* bounds,
    uint32_t downloaded_at, WdgMapBasemap* output, char* error, size_t error_capacity);
/* Validate cache contents before renderer access (cache layer also checks CRC). */
bool wdg_basemap_valid(const WdgMapBasemap* map);

/* Caller schedules downloads and enforces the 60-second minimum interval and
 * Retry-After; this function never retries, polls GPS or accepts an API key. */
typedef struct {
    WdgMapBounds bounds; /* Visible bounds in; actual zoom-bounded download bounds out. */
    uint8_t zoom;
    uint32_t downloaded_at;
    WdgMapCancel cancelled;
    void* context;
    WdgMapBasemap* map;
    WdgMapClientStatus status;
    int http_status;
    uint32_t retry_after_s;
    size_t bytes_received;
    char message[64];
} WdgBasemapFetch;
bool wdg_basemap_fetch(WdgBasemapFetch* fetch);
