#include "wdgwars_map_basemap.h"
#include "wdgwars_map_json.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

static void* base_alloc(size_t size) {
#ifdef ESP_PLATFORM
    return heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return calloc(1, size);
#endif
}

#define BASE_METRES_PER_DEGREE 111320.0
#define BASE_PI 3.14159265358979323846
#define BASE_LIMIT_METRES 50000.0
#define BASE_SOURCE_ELEMENTS 1024U

typedef struct {
    WdgMapPoint points[WDG_BASE_MAX_WAY_POINTS];
    size_t count;
    bool clipped, overflow;
    WdgBaseKind kind;
    bool recognized, count_marker, count_present;
    char source_count[24];
    char name[40];
} BaseWay;

static void message(char* out, size_t capacity, const char* text) {
    if(out && capacity) snprintf(out, capacity, "%s", text);
}

static double minimum(double a, double b) { return a < b ? a : b; }

static double longitude_scale(const WdgMapBounds* b) {
    double lat = b->south <= 0 && b->north >= 0 ? 0 : minimum(fabs(b->south), fabs(b->north));
    return (double)cosf((float)(lat * BASE_PI / 180.0));
}

static double zoom_limit(uint8_t zoom) {
    return zoom == 9 ? 50000.0 : zoom == 10 ? 30000.0 : zoom == 11 ? 16000.0 : zoom == 12 ? 8000.0 : 4000.0;
}

static bool area_limit(const WdgMapBounds* b, double limit) {
    return b && wdg_map_bounds_valid(b) && b->south >= -85.0 && b->north <= 85.0 &&
        (b->north - b->south) * BASE_METRES_PER_DEGREE <= limit + 1.0 &&
        (b->east - b->west) * BASE_METRES_PER_DEGREE * longitude_scale(b) <= limit + 1.0;
}

static bool area_valid(const WdgMapBounds* b) { return area_limit(b, BASE_LIMIT_METRES); }

bool wdg_basemap_bounds(const WdgMapBounds* view, uint8_t zoom, WdgMapBounds* output) {
    if(!view || !output || zoom < WDG_BASE_MIN_ZOOM || zoom > 22 ||
       !wdg_map_bounds_valid(view) || view->south < -85.0 || view->north > 85.0) return false;
    double lat = (view->south + view->north) * .5, lon = (view->west + view->east) * .5;
    double limit = zoom_limit(zoom);
    double height = minimum(view->north - view->south, limit / BASE_METRES_PER_DEGREE);
    WdgMapBounds b = {.south = lat - height * .5, .north = lat + height * .5};
    double width = minimum(view->east - view->west,
        limit / (BASE_METRES_PER_DEGREE * longitude_scale(&b)));
    b.west = lon - width * .5; b.east = lon + width * .5;
    if(!area_valid(&b)) return false;
    *output = b;
    return true;
}

bool wdg_basemap_query(const WdgMapBounds* b, uint8_t zoom, char* path, size_t capacity) {
    if(path && capacity) path[0] = '\0';
    if(!path || zoom < WDG_BASE_MIN_ZOOM || zoom > 22 || !area_limit(b, zoom_limit(zoom))) return false;
    char query[1200], box[100], center[100];
    double lat = (b->south + b->north) * .5, lon = (b->west + b->east) * .5;
    double h = (b->north - b->south) * .125, w = (b->east - b->west) * .125;
    snprintf(box, sizeof(box), "%.6f,%.6f,%.6f,%.6f", b->south, b->west, b->north, b->east);
    snprintf(center, sizeof(center), "%.6f,%.6f,%.6f,%.6f", lat-h, lon-w, lat+h, lon+w);
    const char* roads = zoom <= 10 ? "[highway~\"^(motorway|trunk|primary)\"]" :
        zoom <= 12 ? "[highway~\"^(motorway|trunk|primary|secondary|tertiary)\"]" :
        "[highway][highway!~\"^(construction|proposed)$\"]";
    /* Query and emit center streets before touching the larger viewport. This
     * preserves useful nearby output if a later selection reaches server limits.
     * Reuse the default set instead of retaining full .r/.o copies in memory. */
    int n = snprintf(query, sizeof(query),
        "[out:json][timeout:20][maxsize:33554432][bbox:%s];"
        "way%s(%s)->.c;.c out body geom(%s) qt 160;.c out count;"
        "way%s;(._; - .c;);out body geom(%s) qt %u;out count;%s",
        box, roads, center, box, roads, box, zoom >= 13 ? 80U : 96U,
        zoom >= 13 ? "(way[natural=water];way[waterway~\"^(river|stream|canal)$\"];way[leisure=park];way[landuse~\"^(forest|grass)$\"];);" : "");
    if(n >= 0 && (size_t)n < sizeof(query) && zoom >= 13) {
        int extra = snprintf(query + n, sizeof(query) - (size_t)n,
            "out body geom(%s) qt 16;out count;", box);
        if(extra < 0 || (size_t)extra >= sizeof(query) - (size_t)n) return false;
        n += extra;
    }
    if(n < 0 || (size_t)n >= sizeof(query)) return false;
    const char prefix[] = "/api/interpreter?data=";
    size_t used = sizeof(prefix) - 1;
    if(capacity <= used) return false;
    memcpy(path, prefix, used);
    const char hex[] = "0123456789ABCDEF";
    for(size_t i = 0; i < (size_t)n; ++i) {
        unsigned char c = (unsigned char)query[i];
        bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        size_t bytes = safe ? 1 : 3;
        if(used + bytes >= capacity) { path[0] = '\0'; return false; }
        if(safe) path[used++] = (char)c;
        else { path[used++] = '%'; path[used++] = hex[c >> 4]; path[used++] = hex[c & 15]; }
    }
    path[used] = '\0';
    return true;
}

static bool equals_any(const char* text, const char* const* values, size_t count) {
    for(size_t i = 0; i < count; ++i) if(strcmp(text, values[i]) == 0) return true;
    return false;
}

static bool tags(WdgJson* j, BaseWay* way) {
    if(!wdg_json_object(j)) return false;
    bool first = true, road = false, major = false, water = false, park = false, rail = false;
    char key[64], value[80];
    int next;
    while((next = wdg_json_member_truncated(j, &first, key, sizeof(key))) > 0) {
        bool wanted = !strcmp(key, "name") || !strcmp(key, "name:en") || !strcmp(key, "ref") ||
            !strcmp(key, "highway") || !strcmp(key, "natural") || !strcmp(key, "waterway") ||
            !strcmp(key, "leisure") || !strcmp(key, "landuse") || !strcmp(key, "railway") || !strcmp(key, "total");
        if(!wanted) { if(!wdg_json_skip(j)) return false; continue; }
        if(!wdg_json_string_truncated(j, value, sizeof(value))) return false;
        if(!strcmp(key, "total")) {
            if(way->count_present || strlen(value) >= sizeof(way->source_count)) return false;
            strcpy(way->source_count, value); way->count_present = true;
        } else if(!strcmp(key, "name") || ((!strcmp(key, "name:en") || !strcmp(key, "ref")) && !way->name[0])) {
            size_t cut = strlen(value);
            if(cut >= sizeof(way->name)) {
                cut = sizeof(way->name) - 1;
                while(cut && ((unsigned char)value[cut] & 0xc0) == 0x80) --cut;
            }
            memcpy(way->name, value, cut); way->name[cut] = '\0';
        } else if(!strcmp(key, "highway")) {
            static const char* const majors[] = {"motorway", "motorway_link", "trunk", "trunk_link", "primary", "primary_link", "secondary", "secondary_link", "tertiary", "tertiary_link"};
            static const char* const minors[] = {"residential", "unclassified", "service", "living_street", "pedestrian", "footway", "path", "track", "steps", "cycleway"};
            major = equals_any(value, majors, sizeof(majors) / sizeof(majors[0]));
            road = major || equals_any(value, minors, sizeof(minors) / sizeof(minors[0]));
        } else if(!strcmp(key, "natural")) water |= !strcmp(value, "water");
        else if(!strcmp(key, "waterway")) {
            static const char* const names[] = {"river", "stream", "canal", "drain"};
            water |= equals_any(value, names, sizeof(names) / sizeof(names[0]));
        } else if(!strcmp(key, "leisure")) {
            static const char* const names[] = {"park", "garden", "recreation_ground"};
            park |= equals_any(value, names, sizeof(names) / sizeof(names[0]));
        } else if(!strcmp(key, "landuse")) {
            static const char* const names[] = {"forest", "grass", "recreation_ground"};
            park |= equals_any(value, names, sizeof(names) / sizeof(names[0]));
        } else if(!strcmp(key, "railway")) {
            static const char* const names[] = {"rail", "light_rail", "tram", "subway"};
            rail |= equals_any(value, names, sizeof(names) / sizeof(names[0]));
        }
    }
    way->recognized = road || water || park || rail;
    way->kind = road ? (major ? WdgBaseRoadMajor : WdgBaseRoadMinor) : water ? WdgBaseWater : park ? WdgBasePark : WdgBaseRailway;
    return next == 0;
}

static bool point(WdgJson* j, WdgMapPoint* out, bool* gap) {
    WdgJson probe = *j;
    if(wdg_json_null(&probe)) { *j = probe; *gap = true; return true; }
    if(!wdg_json_object(j)) return false;
    bool first = true, got_lat = false, got_lon = false;
    char key[64]; double lat = 0, lon = 0; int next;
    while((next = wdg_json_member_truncated(j, &first, key, sizeof(key))) > 0) {
        if(!strcmp(key, "lat")) {
            if(got_lat || !wdg_json_number(j, &lat)) return false;
            got_lat = true;
        } else if(!strcmp(key, "lon")) {
            if(got_lon || !wdg_json_number(j, &lon)) return false;
            got_lon = true;
        } else if(!wdg_json_skip(j)) return false;
    }
    if(next < 0 || !got_lat || !got_lon || !isfinite(lat) || !isfinite(lon) ||
       lat < -90 || lat > 90 || lon < -180 || lon > 180) return false;
    out->lat_e6 = (int32_t)(lat * 1000000.0 + (lat < 0 ? -0.5 : 0.5));
    out->lon_e6 = (int32_t)(lon * 1000000.0 + (lon < 0 ? -0.5 : 0.5));
    *gap = false;
    return true;
}

static bool geometry(WdgJson* j, BaseWay* way) {
    if(!wdg_json_array(j)) return false;
    bool first = true; int next;
    while((next = wdg_json_element(j, &first)) > 0) {
        WdgMapPoint p = {0}; bool gap = false;
        if(!point(j, &p, &gap)) return false;
        if(gap) {
            p.lat_e6 = INT32_MIN; p.lon_e6 = INT32_MIN; way->clipped = true;
            /* Cropped long ways can contain thousands of consecutive nulls.
             * One gap preserves the exact segment break without spending points. */
            if(way->count && way->points[way->count-1].lat_e6 == INT32_MIN) continue;
        }
        if(way->count < WDG_BASE_MAX_WAY_POINTS) way->points[way->count++] = p;
        else way->overflow = true;
    }
    return next == 0;
}

static bool same_point(WdgMapPoint a, WdgMapPoint b) {
    return a.lat_e6 == b.lat_e6 && a.lon_e6 == b.lon_e6;
}

static void append_way(WdgMapBasemap* map, const BaseWay* way) {
    if(!way->recognized) return;
    if(way->overflow) { map->partial = true; return; }
    if(way->clipped) map->partial = true;
    size_t start = 0;
    while(start < way->count) {
        while(start < way->count && way->points[start].lat_e6 == INT32_MIN) ++start;
        size_t end = start;
        while(end < way->count && way->points[end].lat_e6 != INT32_MIN) ++end;
        size_t count = end - start;
        if(count >= 2) {
            if(map->feature_count == WDG_BASE_MAX_FEATURES || map->point_count + count > WDG_BASE_MAX_POINTS) {
                map->partial = true; return;
            }
            WdgBaseFeature* f = &map->features[map->feature_count++];
            f->first_point = map->point_count; f->point_count = (uint16_t)count; f->kind = way->kind;
            f->closed = !way->clipped && start == 0 && count == way->count && count >= 4 &&
                same_point(way->points[start], way->points[end - 1]);
            memcpy(f->name, way->name, sizeof(f->name));
            memcpy(map->points + map->point_count, way->points + start, count * sizeof(WdgMapPoint));
            map->point_count += (uint16_t)count;
        } else if(count) map->partial = true;
        start = end + 1;
    }
    if(!way->count) map->partial = true;
}

static bool element(WdgJson* j, WdgMapBasemap* map, BaseWay* way) {
    memset(way, 0, sizeof(*way));
    if(!wdg_json_object(j)) return false;
    char key[64], type[32] = {0};
    bool first = true, have_type = false, have_tags = false, have_geometry = false;
    WdgJson geometry_cursor = {0}; int next;
    while((next = wdg_json_member_truncated(j, &first, key, sizeof(key))) > 0) {
        if(!strcmp(key, "type")) {
            if(have_type || !wdg_json_string(j, type, sizeof(type))) return false;
            have_type = true;
        } else if(!strcmp(key, "tags")) {
            if(have_tags || !tags(j, way)) return false;
            have_tags = true;
        } else if(!strcmp(key, "geometry")) {
            if(have_geometry) return false;
            geometry_cursor = *j; have_geometry = true;
            if(!wdg_json_skip(j)) return false;
        } else if(!wdg_json_skip(j)) return false;
    }
    if(next < 0 || !have_type) return false;
    if(!strcmp(type, "count")) { way->count_marker = true; return way->count_present; }
    if(strcmp(type, "way")) { map->partial = true; return true; }
    if(!way->recognized) return true;
    if(!have_geometry) { map->partial = true; return true; }
    if(!geometry(&geometry_cursor, way)) return false;
    append_way(map, way);
    return true;
}

typedef enum {
    BaseServerNone, BaseServerTimeout, BaseServerMemory,
    BaseServerRateLimit, BaseServerBusy, BaseServerUnknown
} BaseServerReason;

static bool contains_ascii(const char* text, size_t length, const char* word) {
    size_t n = strlen(word);
    for(size_t i = 0; i + n <= length; ++i) {
        size_t j = 0;
        while(j < n) {
            unsigned char c = (unsigned char)text[i+j];
            if(c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if(c != (unsigned char)word[j]) break;
            ++j;
        }
        if(j == n) return true;
    }
    return false;
}

static bool server_reason(WdgJson* j, bool remark, BaseServerReason* reason) {
    size_t start = j->offset;
    /* Validate the entire JSON string, allowing escaped diagnostic newlines,
     * without copying arbitrary server text into the UI. */
    if(!wdg_json_string(j, NULL, 0)) return false;
    const char* raw = j->data + start; size_t length = j->offset - start;
    *reason = BaseServerUnknown;
    if(!remark) return true;
    if(contains_ascii(raw,length,"out of memory") || contains_ascii(raw,length,"memory limit")) *reason = BaseServerMemory;
    else if(contains_ascii(raw,length,"timed out") || contains_ascii(raw,length,"timeout")) *reason = BaseServerTimeout;
    else if(contains_ascii(raw,length,"rate limit") || contains_ascii(raw,length,"too many requests")) *reason = BaseServerRateLimit;
    else if(contains_ascii(raw,length,"too busy") || contains_ascii(raw,length,"server busy")) *reason = BaseServerBusy;
    return true;
}

static bool clip_interval(double direction, double distance, double* low, double* high) {
    if(direction == 0) return distance >= 0;
    double t = distance / direction;
    if(direction < 0) { if(t > *high) return false; if(t > *low) *low = t; }
    else { if(t < *low) return false; if(t < *high) *high = t; }
    return true;
}

static bool useful_geometry(const WdgMapBasemap* map) {
    const WdgMapBounds* b = &map->bounds;
    for(unsigned i = 0; i < map->feature_count; ++i) {
        const WdgBaseFeature* f = &map->features[i];
        for(unsigned j = 1; j < f->point_count; ++j) {
            const WdgMapPoint* a = &map->points[f->first_point+j-1];
            const WdgMapPoint* z = &map->points[f->first_point+j];
            if(same_point(*a,*z)) continue;
            double x = a->lon_e6 / 1e6, y = a->lat_e6 / 1e6;
            double dx = z->lon_e6 / 1e6 - x, dy = z->lat_e6 / 1e6 - y;
            double low = 0, high = 1;
            if(clip_interval(-dx,x-b->west,&low,&high) && clip_interval(dx,b->east-x,&low,&high) &&
               clip_interval(-dy,y-b->south,&low,&high) && clip_interval(dy,b->north-y,&low,&high)) return true;
        }
    }
    return false;
}

static const char* reason_message(BaseServerReason reason, bool partial) {
    if(reason == BaseServerTimeout) return partial ? "Partial streets: Overpass timeout" : "Overpass timed out; zoom in and retry";
    if(reason == BaseServerMemory) return partial ? "Partial streets: Overpass memory limit" : "Overpass memory limit; zoom in and retry";
    if(reason == BaseServerRateLimit) return "Overpass rate limit; retry later";
    if(reason == BaseServerBusy) return "Overpass busy; retry later";
    return "Overpass could not finish this area";
}

bool wdg_basemap_parse(const char* data, size_t length, const WdgMapBounds* bounds,
    uint32_t downloaded_at, WdgMapBasemap* output, char* error, size_t error_capacity) {
    if(!data || !output || !length || length > WDG_BASE_RESPONSE_MAX || !area_valid(bounds)) {
        message(error, error_capacity, "Invalid basemap data or area"); return false;
    }
    WdgMapBasemap* map = base_alloc(sizeof(*map));
    BaseWay* way = base_alloc(sizeof(*way));
    if(!map || !way) { free(map); free(way); message(error, error_capacity, "Basemap memory unavailable"); return false; }
    map->bounds = *bounds; map->downloaded_at = downloaded_at;
    WdgJson j; wdg_json_init(&j, data, length);
    bool ok = wdg_json_object(&j), first = true, have_elements = false, server_error = false;
    BaseServerReason reason = BaseServerNone;
    char key[64]; int next = -1;
    while(ok && (next = wdg_json_member_truncated(&j, &first, key, sizeof(key))) > 0) {
        if(!strcmp(key, "elements")) {
            if(have_elements || !wdg_json_array(&j)) { ok = false; break; }
            have_elements = true;
            bool array_first = true; int item; size_t elements = 0, group = 0;
            while((item = wdg_json_element(&j, &array_first)) > 0) {
                ++elements;
                if(elements > BASE_SOURCE_ELEMENTS) {
                    map->partial = true;
                    if(!wdg_json_skip(&j)) { ok = false; break; }
                } else if(!element(&j, map, way)) { ok = false; break; }
                else if(way->count_marker) {
                    uint32_t count = 0;
                    if(!way->source_count[0]) { ok = false; break; }
                    for(const char* p = way->source_count; *p; ++p) {
                        if(*p < '0' || *p > '9' || count > (UINT32_MAX - (unsigned)(*p-'0')) / 10U) { ok = false; break; }
                        count = count * 10U + (unsigned)(*p-'0');
                    }
                    if(!ok) break;
                    if(count < group) { ok = false; break; }
                    if(count > group) map->partial = true;
                    group = 0;
                } else ++group;
            }
            if(item < 0) ok = false;
        } else if(!strcmp(key, "remark") || !strcmp(key, "error")) {
            if(server_error) { ok = false; break; }
            server_error = true;
            ok = server_reason(&j, !strcmp(key, "remark"), &reason);
        } else ok = wdg_json_skip(&j);
    }
    bool valid = ok && next == 0 && have_elements && wdg_json_finish(&j) && wdg_basemap_valid(map);
    bool partial = valid && (reason == BaseServerTimeout || reason == BaseServerMemory) && useful_geometry(map);
    ok = valid && (!server_error || partial);
    if(ok) {
        if(partial) map->partial = true;
        *output = *map;
        message(error, error_capacity, partial ? reason_message(reason,true) :
            map->partial ? "Basemap partial" : "Basemap downloaded");
    } else message(error, error_capacity, valid && server_error ? reason_message(reason,false) : "Invalid basemap response");
    free(way); free(map);
    return ok;
}

bool wdg_basemap_valid(const WdgMapBasemap* map) {
    if(!map || !area_valid(&map->bounds) || map->feature_count > WDG_BASE_MAX_FEATURES ||
       map->point_count > WDG_BASE_MAX_POINTS) return false;
    for(size_t i = 0; i < map->point_count; ++i) {
        WdgMapPoint p = map->points[i];
        if(p.lat_e6 < -90000000 || p.lat_e6 > 90000000 || p.lon_e6 < -180000000 || p.lon_e6 > 180000000) return false;
    }
    for(size_t i = 0; i < map->feature_count; ++i) {
        const WdgBaseFeature* f = &map->features[i];
        if(f->kind < WdgBaseWater || f->kind > WdgBaseRailway || f->point_count < 2 ||
           f->point_count > WDG_BASE_MAX_WAY_POINTS || (size_t)f->first_point + f->point_count > map->point_count ||
           !memchr(f->name, '\0', sizeof(f->name))) return false;
        if(f->closed && (f->point_count < 4 || !same_point(map->points[f->first_point],
            map->points[f->first_point + f->point_count - 1]))) return false;
    }
    return true;
}

bool wdg_basemap_fetch(WdgBasemapFetch* fetch) {
    if(!fetch) return false;
    fetch->status = WdgMapClientInvalid;
    fetch->http_status = 0; fetch->retry_after_s = 0; fetch->bytes_received = 0;
    message(fetch->message, sizeof(fetch->message), "Zoom to 9+ for map download");
    WdgMapBounds bounds; char path[WDG_BASE_PATH_SIZE];
    if(!fetch->map || !wdg_basemap_bounds(&fetch->bounds, fetch->zoom, &bounds) ||
       !wdg_basemap_query(&bounds, fetch->zoom, path, sizeof(path))) return false;
    fetch->bounds = bounds;
    if(fetch->cancelled && fetch->cancelled(fetch->context)) {
        fetch->status = WdgMapClientCancelled;
        message(fetch->message, sizeof(fetch->message), "Download cancelled"); return false;
    }
    char* body = base_alloc(WDG_BASE_RESPONSE_MAX + 1U);
    if(!body) {
        fetch->status = WdgMapClientMemory;
        message(fetch->message, sizeof(fetch->message), "Basemap memory unavailable"); return false;
    }
    WdgMapHttpRequest request = {.service = WdgMapServiceOverpass, .path = path,
        .body = body, .capacity = WDG_BASE_RESPONSE_MAX + 1U,
        .cancelled = fetch->cancelled, .context = fetch->context};
    bool ok = wdg_map_http_get(&request);
    fetch->status = request.status; fetch->http_status = request.http_status;
    fetch->retry_after_s = request.retry_after_s; fetch->bytes_received = request.length;
    message(fetch->message, sizeof(fetch->message), request.message);
    if(ok && fetch->cancelled && fetch->cancelled(fetch->context)) {
        ok = false; fetch->status = WdgMapClientCancelled;
        message(fetch->message, sizeof(fetch->message), "Download cancelled");
    }
    if(ok) {
        ok = wdg_basemap_parse(body, request.length, &bounds, fetch->downloaded_at,
            fetch->map, fetch->message, sizeof(fetch->message));
        fetch->status = ok ? WdgMapClientOk : WdgMapClientSchema;
    }
    free(body);
    return ok;
}
