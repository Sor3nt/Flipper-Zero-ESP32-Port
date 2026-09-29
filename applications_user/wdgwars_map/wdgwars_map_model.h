#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define WDG_MAP_MAX_CELLS 1024
#define WDG_MAP_MAX_GANGS 1024
#define WDG_MAP_MAX_HULLS 2048
#define WDG_MAP_MAX_POINTS 32768
#define WDG_MAP_NAME_SIZE 65
#define WDG_MAP_RESPONSE_MAX (1024U * 1024U)
#define WDG_MAP_ATTRIBUTION "Territories: WDGWars"
typedef struct { double south, west, north, east; } WdgMapBounds;
typedef struct { int32_t lat_e6, lon_e6; } WdgMapPoint;
typedef enum { WdgMapCellAnchorUnknown, WdgMapCellAnchorSouthWest, WdgMapCellAnchorCenter } WdgMapCellAnchor;
typedef struct { int32_t lat_e6, lon_e6; uint32_t gang_id, count, color_rgb; } WdgMapCell;
typedef struct { uint32_t id, color_rgb; char name[WDG_MAP_NAME_SIZE]; } WdgMapGang;
typedef struct { uint32_t gang_id, color_rgb; uint16_t first_point, point_count; char name[WDG_MAP_NAME_SIZE]; } WdgMapHull;
typedef struct {
    WdgMapBounds bounds;
    uint32_t downloaded_at, source_epoch;
    int32_t grid_lat_e6, grid_lon_e6;
    uint16_t cell_count, gang_count, hull_count, point_count;
    uint8_t zoom;
    bool source_time_valid, cells_skipped;
    WdgMapCellAnchor cell_anchor;
    WdgMapCell cells[WDG_MAP_MAX_CELLS];
    WdgMapGang gangs[WDG_MAP_MAX_GANGS];
    WdgMapHull hulls[WDG_MAP_MAX_HULLS];
    WdgMapPoint points[WDG_MAP_MAX_POINTS];
} WdgMapSnapshot;
typedef struct {
    char username[WDG_MAP_NAME_SIZE], gang[WDG_MAP_NAME_SIZE];
    uint32_t gang_id, wifi, ble, aircraft, mesh, total;
} WdgMapAccount;
bool wdg_map_bounds_valid(const WdgMapBounds* bounds);
