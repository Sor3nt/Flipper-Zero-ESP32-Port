#pragma once
#include "wdgwars_map_model.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WDG_MAP_DISPLAY_WIDTH 320
#define WDG_MAP_DISPLAY_HEIGHT 170
/* Pixels are byte-swapped RGB565, ready for furi_hal_display_draw_rgb565. */
typedef struct WdgMapBasemap WdgMapBasemap;
typedef enum {
    WdgMapBackdropAuto = 0,
    WdgMapBackdropLoading,
    WdgMapBackdropMissing,
    WdgMapBackdropOffline,
    WdgMapBackdropError,
    WdgMapBackdropEmpty,
} WdgMapBackdropStatus;
typedef struct {
    double center_lat, center_lon, zoom;
    double gps_lat, gps_lon;
    float accuracy_m;
    uint32_t now_ms, cache_age_seconds, selected_gang_id;
    uint8_t satellites;
    bool has_fix, gps_connected, accuracy_known, follow, online, loading;
    bool cache_age_known;
    WdgMapBackdropStatus backdrop_status;
    bool backdrop_retry; /* Caller guarantees a click retries the map request. */
    const char* map_message; /* Optional status-card headline. */
    const char* map_detail;  /* Optional explanatory line, wrapped to card width. */
    const char* status;         /* Optional short notice; no borrowed ownership. */
    const char* area_name;      /* Optional downloaded-area name. */
    const char* basemap_status; /* Optional coverage/freshness notice. */
} WdgMapRenderState;

/* Pure rendering: no allocations, HTTP, SPI, storage, locks or state mutation. */
void wdgwars_map_render(uint16_t* pixels, const WdgMapSnapshot* map,
    const WdgMapRenderState* state, const WdgMapBasemap* basemap);
/* Render after the map. A selected index >= count gives an information sheet. */
void wdgwars_map_render_menu(uint16_t* pixels, const char* title,
    const char* const* lines, size_t count, size_t selected, const char* footer);

/* Zoom 0 (or <0.5) is a complete 272x136 equirectangular world overview,
 * independent of camera center, including both poles and dateline edges.
 * Zoom >=1 uses north-up Web Mercator with caller-owned continuous easing.
 * Longitude wraps around the antimeridian. A returned viewport can have west >
 * east when it crosses it; clients should split that into two API requests. */
bool wdgwars_map_geo_to_screen(const WdgMapRenderState* state,
    double latitude, double longitude, float* x, float* y);
bool wdgwars_map_screen_to_geo(const WdgMapRenderState* state,
    float x, float y, double* latitude, double* longitude);
bool wdgwars_map_viewport_bounds(const WdgMapRenderState* state, WdgMapBounds* out);
uint16_t wdgwars_map_rgb565(uint32_t rgb);