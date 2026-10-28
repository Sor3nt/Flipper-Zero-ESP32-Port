/* Synthetic geometry only: this fixture is a render test, not downloaded data. */
#include "wdgwars_map_render.h"
#include "wdgwars_map_basemap.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PIXELS (WDG_MAP_DISPLAY_WIDTH*WDG_MAP_DISPLAY_HEIGHT)
/* Parser HTTP dependency is intentionally unavailable in render checks. */
bool wdg_map_http_get(WdgMapHttpRequest* r) { (void)r; return false; }
static WdgMapSnapshot snapshot;
static WdgMapBasemap basemap;
static uint16_t guarded[PIXELS+32];
static WdgMapPoint at(const WdgMapRenderState* s,float x,float y) {
    double lat,lon;assert(wdgwars_map_screen_to_geo(s,x,y,&lat,&lon));
    return (WdgMapPoint){(int32_t)(lat*1e6),(int32_t)(lon*1e6)};
}
static void add_way(const WdgMapRenderState* s,WdgBaseKind kind,bool closed,const char* name,const float* xy,unsigned n) {
    assert(basemap.feature_count<WDG_BASE_MAX_FEATURES&&basemap.point_count+n<WDG_BASE_MAX_POINTS);
    WdgBaseFeature* f=&basemap.features[basemap.feature_count++];
    *f=(WdgBaseFeature){.first_point=basemap.point_count,.point_count=n,.kind=kind,.closed=closed};
    snprintf(f->name,sizeof(f->name),"%s",name);
    for(unsigned i=0;i<n;i++)basemap.points[basemap.point_count++]=at(s,xy[i*2],xy[i*2+1]);
}
static void fixture(WdgMapRenderState* s) {
    *s=(WdgMapRenderState){.center_lat=12.9716,.center_lon=77.5946,.zoom=15,
        .gps_lat=12.9716,.gps_lon=77.5946,.accuracy_m=28,.now_ms=900,.cache_age_seconds=480,
        .selected_gang_id=1,.satellites=13,.has_fix=true,.gps_connected=true,
        .accuracy_known=true,.follow=true,.online=true,.cache_age_known=true,.area_name="Sample area"};
    memset(&snapshot,0,sizeof(snapshot));memset(&basemap,0,sizeof(basemap));
    snapshot.gang_count=3;
    snapshot.gangs[0]=(WdgMapGang){1,0x775BD8,"Purple Circuit"};
    snapshot.gangs[1]=(WdgMapGang){2,0xE69240,"Solar Scouts"};
    snapshot.gangs[2]=(WdgMapGang){3,0x159F8C,"Emerald Crew"};
    snapshot.cell_anchor=WdgMapCellAnchorSouthWest;snapshot.grid_lat_e6=1500;snapshot.grid_lon_e6=1900;
    WdgMapPoint origin=at(s,10,145);
    for(int x=0;x<7;x++)for(int y=0;y<4;y++) {
        if((x==0&&y==0)||(x==6&&y==3)||(x==2&&y==1))continue;
        unsigned team=x<3?1:x<5?2:3;
        snapshot.cells[snapshot.cell_count++]=(WdgMapCell){origin.lat_e6+y*1500,origin.lon_e6+x*1900,team,50,snapshot.gangs[team-1].color_rgb};
    }
    const float lake[]={-5,85,28,86,55,99,69,128,95,138,107,175,-5,175,-5,85};
    const float park[]={244,-6,330,-6,330,95,291,84,267,54,244,-6};
    const float avenue[]={-10,128,28,111,78,83,124,59,190,53,233,63,272,49,338,20};
    const float route[]={37,-5,62,42,77,77,90,106,94,134,113,175};
    add_way(s,WdgBaseWater,true,"Sample Lake",lake,8);add_way(s,WdgBasePark,true,"Sample Park",park,6);
    add_way(s,WdgBaseRoadMajor,false,"Sample Avenue",avenue,8);add_way(s,WdgBaseRoadMajor,false,"",route,6);
    for(int x=126;x<300;x+=36){float road[]={x-14,-10,x-4,35,x+2,75,x+9,121,x+18,180};add_way(s,WdgBaseRoadMinor,false,"",road,5);}
    for(int y=17;y<170;y+=31){float road[]={-10,y+16,46,y+6,110,y-5,175,y-9,244,y-2,340,y-14};add_way(s,WdgBaseRoadMinor,false,"",road,6);}
    wdgwars_map_viewport_bounds(s,&snapshot.bounds);basemap.bounds=snapshot.bounds;
}
static void ppm(const char* path) {
    FILE* f=fopen(path,"wb");assert(f);fprintf(f,"P6\n320 170\n255\n");
    for(unsigned i=0;i<PIXELS;i++){
        uint16_t c=guarded[i+16];c=(uint16_t)((c<<8)|(c>>8));
        unsigned char rgb[3]={(unsigned char)(((c>>11)&31)*255/31),(unsigned char)(((c>>5)&63)*255/63),(unsigned char)((c&31)*255/31)};
        assert(fwrite(rgb,1,3,f)==3);
    }assert(fclose(f)==0);
}
static uint32_t frame_hash(void) {
    uint32_t hash=2166136261U;
    for(unsigned i=0;i<PIXELS;i++)hash=(hash^guarded[16+i])*16777619U;
    return hash;
}
static void guards(void) {for(unsigned i=0;i<16;i++){assert(guarded[i]==0xABCD);assert(guarded[PIXELS+16+i]==0xABCD);}}
static void projection_tests(void) {
    const double centers[][2]={{0,0},{12.97,77.59},{51.5,-0.12},{84.9,179.999},{-84.9,-179.999}};
    for(unsigned i=0;i<5;i++)for(unsigned z=2;z<=20;z+=3){
        WdgMapRenderState s={.center_lat=centers[i][0],.center_lon=centers[i][1],.zoom=z+0.25};
        float x,y;double lat,lon;assert(wdgwars_map_geo_to_screen(&s,s.center_lat,s.center_lon,&x,&y));
        assert(fabs(x-160)<0.001&&fabs(y-85)<0.001);
        for(int xx=0;xx<320;xx+=51)for(int yy=45;yy<120;yy+=27){
            assert(wdgwars_map_screen_to_geo(&s,(float)xx,(float)yy,&lat,&lon));assert(isfinite(lat)&&isfinite(lon));
            assert(lat>=-85.052&&lat<=85.052&&lon>=-180&&lon<180);
            assert(wdgwars_map_geo_to_screen(&s,lat,lon,&x,&y));
            assert(fabs(x-xx)<0.003);if(fabs(lat)<85.051128)assert(fabs(y-yy)<0.003);
        }
    }
    WdgMapRenderState s={.center_lat=0,.center_lon=179.999,.zoom=15};WdgMapBounds bounds;
    assert(wdgwars_map_viewport_bounds(&s,&bounds)&&bounds.west>bounds.east);
    float x,y;assert(wdgwars_map_geo_to_screen(&s,0,-179.999,&x,&y)&&x>160&&x<250);
    assert(!wdgwars_map_geo_to_screen(&s,NAN,0,&x,&y));assert(!wdgwars_map_geo_to_screen(&s,91,0,&x,&y));
    /* Zoom zero shows the complete physical globe, regardless of GPS center. */
    s=(WdgMapRenderState){.center_lat=12.97,.center_lon=77.59,.zoom=0};
    assert(wdgwars_map_viewport_bounds(&s,&bounds));
    assert(bounds.south==-90&&bounds.north==90&&bounds.west==-180&&bounds.east==180);
    assert(wdgwars_map_geo_to_screen(&s,90,-180,&x,&y)&&fabs(x-24)<0.001&&fabs(y-17)<0.001);
    assert(wdgwars_map_geo_to_screen(&s,-90,180,&x,&y)&&fabs(x-296)<0.001&&fabs(y-153)<0.001);
    for(int lat=-90;lat<=90;lat+=15)for(int lon=-180;lon<=180;lon+=30) {
        double a,b;assert(wdgwars_map_geo_to_screen(&s,lat,lon,&x,&y));
        assert(wdgwars_map_screen_to_geo(&s,x,y,&a,&b));assert(fabs(a-lat)<0.0001&&fabs(b-lon)<0.0001);
    }
    s.zoom=1;assert(wdgwars_map_geo_to_screen(&s,s.center_lat,s.center_lon,&x,&y));
    assert(fabs(x-160)<0.001&&fabs(y-85)<0.001);
    s.zoom=NAN;assert(!wdgwars_map_viewport_bounds(&s,&bounds));
    assert(wdgwars_map_rgb565(0xff0000)==0x00f8);assert(wdgwars_map_rgb565(0x00ff00)==0xe007);assert(wdgwars_map_rgb565(0x0000ff)==0x1f00);
}
int main(int argc,char** argv) {
    assert(argc==2);projection_tests();for(unsigned i=0;i<PIXELS+32;i++)guarded[i]=0xABCD;
    WdgMapRenderState state;fixture(&state);uint16_t* pixels=guarded+16;char file[1024];
    wdgwars_map_render(pixels,&snapshot,&state,&basemap);guards();
    snprintf(file,sizeof(file),"%s/render-online.ppm",argv[1]);ppm(file);
    state.online=false;state.follow=false;state.loading=true;state.now_ms=1350;state.cache_age_seconds=93780;state.status="Saved area";
    wdgwars_map_render(pixels,&snapshot,&state,&basemap);guards();snprintf(file,sizeof(file),"%s/render-offline.ppm",argv[1]);ppm(file);
    fixture(&state);state.zoom=0;state.online=false;state.backdrop_status=WdgMapBackdropOffline;
    wdgwars_map_render(pixels,NULL,&state,NULL);guards();snprintf(file,sizeof(file),"%s/render-world.ppm",argv[1]);ppm(file);
    snapshot.cell_count=3;snapshot.cell_anchor=WdgMapCellAnchorUnknown;
    snapshot.cells[0]=(WdgMapCell){37770000,-122400000,1,80,0x775BD8};
    snapshot.cells[1]=(WdgMapCell){51500000,-120000,2,40,0xE69240};
    snapshot.cells[2]=(WdgMapCell){12970000,77590000,3,90,0x159F8C};
    snapshot.bounds=(WdgMapBounds){-85,-180,85,180};state.backdrop_status=WdgMapBackdropAuto;
    wdgwars_map_render(pixels,&snapshot,&state,NULL);guards();snprintf(file,sizeof(file),"%s/render-world-territories.ppm",argv[1]);ppm(file);
    state.zoom=1;state.center_lon=179;state.center_lat=0;state.has_fix=false;
    wdgwars_map_render(pixels,NULL,&state,NULL);guards();snprintf(file,sizeof(file),"%s/render-world-dateline.ppm",argv[1]);ppm(file);
    fixture(&state);state.backdrop_status=WdgMapBackdropLoading;state.loading=true;state.now_ms=1250;
    wdgwars_map_render(pixels,NULL,&state,NULL);guards();snprintf(file,sizeof(file),"%s/render-loading.ppm",argv[1]);ppm(file);
    state.backdrop_status=WdgMapBackdropError;state.loading=false;state.backdrop_retry=true;
    state.map_detail="Map server timed out. Your GPS connection is working.";
    wdgwars_map_render(pixels,NULL,&state,NULL);guards();snprintf(file,sizeof(file),"%s/render-error.ppm",argv[1]);ppm(file);
    state.backdrop_status=WdgMapBackdropEmpty;state.backdrop_retry=false;state.map_detail=NULL;
    wdgwars_map_render(pixels,NULL,&state,NULL);guards();snprintf(file,sizeof(file),"%s/render-no-streets.ppm",argv[1]);ppm(file);
    fixture(&state);wdgwars_map_render(pixels,&snapshot,&state,&basemap);uint16_t old_row[320];memcpy(old_row,pixels+320*93,sizeof(old_row));
    state.backdrop_status=WdgMapBackdropLoading;state.loading=true;
    wdgwars_map_render(pixels,&snapshot,&state,&basemap);guards();assert(!memcmp(old_row,pixels+320*93,sizeof(old_row)));
    snprintf(file,sizeof(file),"%s/render-refresh.ppm",argv[1]);ppm(file);
    state.backdrop_status=WdgMapBackdropAuto;state.loading=false;
    const char* choices[]={"Download territories here","Download streets + parks","Account and gang","Saved areas","Recenter on GPS","Pan map","About and attribution"};
    wdgwars_map_render_menu(pixels,"Explore your WDGWars map",choices,7,1,"Turn: choose  Click: open  Back: map");guards();
    snprintf(file,sizeof(file),"%s/render-menu.ppm",argv[1]);ppm(file);
    state.has_fix=false;state.gps_connected=true;state.loading=false;state.status="No area cached";state.cache_age_known=false;
    wdgwars_map_render(pixels,NULL,&state,NULL);guards();snprintf(file,sizeof(file),"%s/render-empty.ppm",argv[1]);ppm(file);
    fixture(&state);state.center_lon+=0.07;state.follow=false;state.online=false;
    wdgwars_map_render(pixels,&snapshot,&state,&basemap);guards();
    snprintf(file,sizeof(file),"%s/render-outside.ppm",argv[1]);ppm(file);
    /* Real, provenance-recorded OpenStreetMap response; no fake territories or
     * asserted GPS fix are combined with the genuine geographic preview. */
    snprintf(file,sizeof(file),"%s/../basemap_live_greenwich.json",argv[1]);
    FILE* captured=fopen(file,"rb");assert(captured);
    char* json=malloc(WDG_MAP_RESPONSE_MAX+1);assert(json);
    size_t bytes=fread(json,1,WDG_MAP_RESPONSE_MAX,captured);assert(!ferror(captured));fclose(captured);json[bytes]=0;
    char error[80];WdgMapBounds london={51.476,-.006,51.480,.006};
    assert(wdg_basemap_parse(json,bytes,&london,1000,&basemap,error,sizeof(error)));free(json);
    state=(WdgMapRenderState){.center_lat=51.4783,.center_lon=-.0013,.zoom=16.5,
        .online=false,.cache_age_known=true,.cache_age_seconds=420,.area_name="Greenwich - real OSM test",
        .status="Street preview"};
    wdgwars_map_render(pixels,NULL,&state,&basemap);guards();
    snprintf(file,sizeof(file),"%s/render-real-greenwich.ppm",argv[1]);ppm(file);
    /* Unknown server grid anchors remain point markers, never guessed cells. */
    fixture(&state);snapshot.cell_anchor=WdgMapCellAnchorUnknown;
    wdgwars_map_render(pixels,&snapshot,&state,&basemap);guards();snprintf(file,sizeof(file),"%s/render-markers.ppm",argv[1]);ppm(file);
    uint32_t markers=frame_hash();
    snapshot.grid_lat_e6=500000;snapshot.grid_lon_e6=700000;
    wdgwars_map_render(pixels,&snapshot,&state,&basemap);guards();
    assert(frame_hash()==markers); /* Unknown anchors never invent cell extents. */
    for(unsigned i=0;i<snapshot.cell_count;i++){snapshot.cells[i].gang_id=0;snapshot.cells[i].color_rgb=0xff0000;}
    wdgwars_map_render(pixels,&snapshot,&state,&basemap);guards();uint32_t red=frame_hash();
    for(unsigned i=0;i<snapshot.cell_count;i++)snapshot.cells[i].color_rgb=0x00ff00;
    wdgwars_map_render(pixels,&snapshot,&state,&basemap);guards();assert(frame_hash()!=red);
    /* Hostile cached spans are ignored; extreme offscreen geometry stays bounded. */
    snapshot.point_count=WDG_MAP_MAX_POINTS;snapshot.hull_count=2;
    snapshot.hulls[0]=(WdgMapHull){.gang_id=1,.color_rgb=0x557799,.first_point=4095,.point_count=65535};
    snapshot.hulls[1]=(WdgMapHull){.gang_id=2,.color_rgb=0x557799,.first_point=0,.point_count=4096};
    for(unsigned i=0;i<4096;i++)snapshot.points[i]=(WdgMapPoint){(int32_t)(i%2?84000000:-84000000),(int32_t)(i%3?175000000:-175000000)};
    basemap.features[0].point_count=65535;
    wdgwars_map_render(pixels,&snapshot,&state,&basemap);guards();
    const char* international[]={"UTF-8: caf\xc3\xa9 / \xe6\x9d\xb1\xe4\xba\xac","abcdefghijklmnopqrstuvwxyz","ABCDEFGHIJKLMNOPQRSTUVWXYZ","0123456789 !\"#$%&'()*+,-./",":;<=>?@[\\]^_`{|}~"};
    wdgwars_map_render_menu(pixels,"Font and clipping",international,5,(size_t)-1,NULL);guards();
    snprintf(file,sizeof(file),"%s/render-font.ppm",argv[1]);ppm(file);
    puts("PASS: whole-globe/Mercator roundtrips, poles/dateline, RGB565, clipping, truthful ownership, refresh retention, offline/loading/error/menu rendering");return 0;
}