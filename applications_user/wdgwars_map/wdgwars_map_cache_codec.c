#include "wdgwars_map_cache.h"
#include <math.h>
#include <string.h>
#include <stddef.h>

_Static_assert(sizeof(WdgMapHull) == 80, "v1 named hull layout changed");
_Static_assert(sizeof(WdgMapSnapshotV1) == 68416, "v1 snapshot layout changed");
_Static_assert(sizeof(WdgMapSavedAreaV1) == WDG_CACHE_V1_SIZE, "v1 cache layout changed");
_Static_assert(offsetof(WdgMapSavedAreaV1, territories) == 32, "v1 territory offset changed");
_Static_assert(offsetof(WdgMapSavedAreaV1, basemap) == 68448, "v1 basemap offset changed");
uint32_t wdg_cache_crc(const void* data, size_t length) {
    const unsigned char* p=data; uint32_t crc=~0U;
    while(length--) { crc^=*p++; for(unsigned j=0;j<8;++j) crc=(crc>>1)^(0xEDB88320U & (0U-(crc&1U))); }
    return ~crc;
}
static bool point(int32_t lat,int32_t lon) { return lat>=-90000000 && lat<=90000000 && lon>=-180000000 && lon<=180000000; }
static bool text_ok(const char* s,size_t n) { return memchr(s,0,n)!=NULL; }
bool wdg_cache_area_valid(const WdgMapSavedArea* a) {
    if(!a || !isfinite(a->center_lat) || !isfinite(a->center_lon) || !isfinite(a->zoom) ||
        a->center_lat < -85.05112878 || a->center_lat > 85.05112878 ||
        a->center_lon < -180 || a->center_lon > 180 || a->zoom<0 || a->zoom>20 || (a->flags&~15U)) return false;
    if(a->flags&WDG_CACHE_ACCOUNT) {
        if(!text_ok(a->account.username,sizeof(a->account.username)) || !text_ok(a->account.gang,sizeof(a->account.gang))) return false;
    }
    if((a->flags&WDG_CACHE_BASEMAP) && !wdg_basemap_valid(&a->basemap)) return false;
    if(a->flags&WDG_CACHE_TERRITORIES) {
        const WdgMapSnapshot* m=&a->territories;
        if(!wdg_map_bounds_valid(&m->bounds) || m->cell_count>WDG_MAP_MAX_CELLS || m->gang_count>WDG_MAP_MAX_GANGS ||
           m->hull_count>WDG_MAP_MAX_HULLS || m->point_count>WDG_MAP_MAX_POINTS || m->zoom>20 ||
           m->cell_anchor>WdgMapCellAnchorCenter || m->cell_anchor<WdgMapCellAnchorUnknown ||
           m->grid_lat_e6<0 || m->grid_lat_e6>180000000 || m->grid_lon_e6<0 || m->grid_lon_e6>360000000) return false;
        if(m->cell_count && m->cell_anchor!=WdgMapCellAnchorUnknown && (!m->grid_lat_e6 || !m->grid_lon_e6)) return false;
        for(unsigned i=0;i<m->gang_count;++i) if(!text_ok(m->gangs[i].name,sizeof(m->gangs[i].name)) || m->gangs[i].color_rgb>0xFFFFFF) return false;
        for(unsigned i=0;i<m->cell_count;++i) if(!point(m->cells[i].lat_e6,m->cells[i].lon_e6) || m->cells[i].color_rgb>0xFFFFFF) return false;
        for(unsigned i=0;i<m->point_count;++i) if(!point(m->points[i].lat_e6,m->points[i].lon_e6)) return false;
        for(unsigned i=0;i<m->hull_count;++i) {
            const WdgMapHull* h=&m->hulls[i];
            if(!text_ok(h->name,sizeof(h->name)) || h->point_count<3 || h->first_point>m->point_count || h->point_count>m->point_count-h->first_point || h->color_rgb>0xFFFFFF) return false;
        }
    }
    return true;
}
void wdg_cache_header(WdgCacheHeader* h,const WdgMapSavedArea* a) {
    *h=(WdgCacheHeader){0x5744474DU,WDG_CACHE_VERSION,sizeof(*a),wdg_cache_crc(a,sizeof(*a))};
}
bool wdg_cache_verify(const WdgCacheHeader* h,const WdgMapSavedArea* a) {
    return h && a && h->magic==0x5744474DU && h->version==WDG_CACHE_VERSION && h->length==sizeof(*a) &&
        h->crc==wdg_cache_crc(a,sizeof(*a)) && wdg_cache_area_valid(a);
}

size_t wdg_cache_payload_size(const WdgCacheHeader* h) {
    if(!h || h->magic!=0x5744474DU) return 0;
    if(h->version==1 && h->length==WDG_CACHE_V1_SIZE) return WDG_CACHE_V1_SIZE;
    if(h->version==WDG_CACHE_VERSION && h->length==sizeof(WdgMapSavedArea)) return sizeof(WdgMapSavedArea);
    return 0;
}
bool wdg_cache_decode(const WdgCacheHeader* h,const void* payload,size_t length,WdgMapSavedArea* out) {
    if(!out || !payload || !wdg_cache_payload_size(h) || length!=h->length || h->crc!=wdg_cache_crc(payload,length)) return false;
    if(h->version==WDG_CACHE_VERSION) {
        const WdgMapSavedArea* a=payload;
        if(!wdg_cache_area_valid(a)) return false;
        if(out!=a) memcpy(out,a,sizeof(*out));
        return true;
    }
    const WdgMapSavedAreaV1* old=payload;
    const WdgMapSnapshotV1* m=&old->territories;
    /* Check legacy capacities even when the corresponding display flag is off:
     * an old count cannot address the newly expanded arrays during migration. */
    if(m->cell_count>1024 || m->gang_count>64 || m->hull_count>128 || m->point_count>4096) return false;
    memset(out,0,sizeof(*out));
    out->center_lat=old->center_lat; out->center_lon=old->center_lon; out->zoom=old->zoom;
    out->flags=old->flags; out->account_at=old->account_at;
    /* Identical fixed metadata prefix, followed by explicitly sized old arrays. */
    memcpy(&out->territories,m,offsetof(WdgMapSnapshotV1,cells));
    memcpy(out->territories.cells,m->cells,sizeof(m->cells));
    memcpy(out->territories.gangs,m->gangs,sizeof(m->gangs));
    memcpy(out->territories.hulls,m->hulls,sizeof(m->hulls));
    memcpy(out->territories.points,m->points,sizeof(m->points));
    out->basemap=old->basemap; out->account=old->account;
    return wdg_cache_area_valid(out);
}
