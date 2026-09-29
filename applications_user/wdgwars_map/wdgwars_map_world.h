#pragma once
#include "wdgwars_map_model.h"
/* Natural Earth 1:110m land, public domain. */
typedef struct { uint16_t first_point, point_count; bool hole; } WdgWorldRing;
extern const WdgMapPoint wdg_world_points[];
extern const WdgWorldRing wdg_world_rings[];
extern const size_t wdg_world_ring_count;
extern const size_t wdg_world_point_count;
