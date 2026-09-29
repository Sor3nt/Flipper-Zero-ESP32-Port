/* End-to-end application adapters + production HTTP helper. Only ESP-IDF/WLAN
 * transport is mocked, so relative paths/auth/timeouts cannot bypass coverage. */
#define main http_unit_main
#include "test_client.c"
#undef main
#include "wdgwars_map_basemap.h"
#include "wdgwars_map_json.h"
static const char street_json[]="{\"version\":0.6,\"elements\":[{\"type\":\"way\",\"id\":1,\"tags\":{\"highway\":\"residential\",\"name\":\"Test Street\"},\"geometry\":[{\"lat\":51.477,\"lon\":0},{\"lat\":51.478,\"lon\":0.001}]}]}";
static const char overview_json[]="[{\"gang_id\":7,\"name\":\"Test team\",\"color\":\"#3366cc\",\"members\":1,\"points\":20,\"rank\":1,\"logo\":null,\"hull\":[[40,-80],[41,-81],[41,-80]]}]";
static char* many_gang_hulls(void) {
    char* data=malloc(WDG_MAP_RESPONSE_MAX+1); assert(data); size_t used=0;
    data[used++]='[';
    for(unsigned h=0;h<1500;++h) {
        int n=snprintf(data+used,WDG_MAP_RESPONSE_MAX+1-used,"%s{\"gang_id\":%u,\"name\":\"Synthetic gang %u\",\"color\":\"#3366cc\",\"members\":1,\"points\":20,\"rank\":1,\"logo\":null,\"hull\":[",h?",":"",h%600+1,h%600+1);
        assert(n>0&&(size_t)n<WDG_MAP_RESPONSE_MAX+1-used); used+=(size_t)n;
        static const double lat[]={41.0000,41.0001,41.0003,41.0006,41.0010,41.0014,41.0017,41.0019,41.0020,41.0019,41.0017,41.0014,41.0010,41.0006,41.0003,41.0001};
        static const double lon[]={-81.0010,-81.0006,-81.0003,-81.0001,-81.0000,-81.0001,-81.0003,-81.0006,-81.0010,-81.0014,-81.0017,-81.0019,-81.0020,-81.0019,-81.0017,-81.0014};
        for(unsigned p=0;p<16;++p) {
            n=snprintf(data+used,WDG_MAP_RESPONSE_MAX+1-used,"%s[%.6f,%.6f]",p?",":"",lat[p],lon[p]);
            assert(n>0&&(size_t)n<WDG_MAP_RESPONSE_MAX+1-used); used+=(size_t)n;
        }
        assert(used+3<WDG_MAP_RESPONSE_MAX); data[used++]=']'; data[used++]='}';
    }
    data[used++]=']'; data[used]=0; assert(used>512*1024); return data;
}
static void world_fetch_checks(WdgMapFetch* world,WdgMapSnapshot* territories) {
    WdgMapSnapshot* previous=malloc(sizeof(*previous)); assert(previous);
    for(unsigned z=0;z<6;++z) {
        reset(); response=overview_json; world->zoom=(uint8_t)z;
        assert(wdg_map_fetch_territories(world)&&territories->hull_count==1&&territories->cells_skipped&&!territories->cell_count&&territories->zoom==z&&key_headers==1);
        assert(!strcmp(url,"https://wdgwars.pl/api/territories"));
        assert(territories->bounds.west==-180&&territories->bounds.east==180&&territories->bounds.south== -85.051128&&territories->bounds.north==85.051128);
    }
    *previous=*territories;
    reset(); status=401;
    assert(!wdg_map_fetch_territories(world)&&world->status==WdgMapClientAuth&&world->http_status==401&&!memcmp(territories,previous,sizeof(*previous)));
    reset(); world->api_key=NULL;
    assert(!wdg_map_fetch_territories(world)&&world->status==WdgMapClientAuth&&!opens&&!memcmp(territories,previous,sizeof(*previous))); world->api_key=key;
    reset(); response=overview_json; cancel_flag=true;
    assert(!wdg_map_fetch_territories(world)&&world->status==WdgMapClientCancelled&&!opens&&!memcmp(territories,previous,sizeof(*previous)));
    reset(); response=overview_json; cancel_after_reads=2;
    assert(!wdg_map_fetch_territories(world)&&world->status==WdgMapClientCancelled&&cleanups==1&&!memcmp(territories,previous,sizeof(*previous)));
    reset(); response=overview_json; expected=0; complete=false;
    assert(!wdg_map_fetch_territories(world)&&world->status==WdgMapClientNetwork&&!memcmp(territories,previous,sizeof(*previous)));
    reset(); response="[{\"gang_id\":7,\"name\":\"bad\",\"color\":\"#3366cc\",\"hull\":[[91,0],[0,0],[1,1]]}]";
    assert(!wdg_map_fetch_territories(world)&&world->status==WdgMapClientSchema&&!memcmp(territories,previous,sizeof(*previous)));
    reset(); response=good; world->zoom=6;
    assert(wdg_map_fetch_territories(world)&&territories->cell_count==1&&!territories->cells_skipped);
    assert(strstr(url,"https://wdgwars.pl/api/member-territories?compact=1&bbox=")==url&&strstr(url,"&zoom=6"));
    char* large=many_gang_hulls(); reset(); response=large; chunk_size=1021; world->zoom=0;
    assert(wdg_map_fetch_territories(world)&&world->bytes_received>512*1024&&territories->hull_count==1500&&territories->point_count==24000&&territories->gang_count==600&&key_headers==1);
    assert(territories->hulls[0].gang_id==territories->hulls[600].gang_id);
    printf("Synthetic world through real fetch/client: %u gangs, %u hulls, %u points, %u bytes\n",territories->gang_count,territories->hull_count,territories->point_count,(unsigned)world->bytes_received);
    free(large); free(previous);
}
static char* read_private(const char* path) {
    FILE* file=fopen(path,"rb"); assert(file); char* live=malloc(WDG_MAP_RESPONSE_MAX+1); assert(live);
    size_t length=fread(live,1,WDG_MAP_RESPONSE_MAX,file); assert(!ferror(file)&&fgetc(file)==EOF); fclose(file); live[length]=0; return live;
}
int main(int argc,char** argv) {
    assert(http_unit_main()==0);
    WdgMapBasemap* map=calloc(1,sizeof(*map)),*saved=calloc(1,sizeof(*saved)); assert(map&&saved);
    const WdgMapBounds bounds={51.476,-0.006,51.480,0.006};
    WdgBasemapFetch f={.bounds=bounds,.zoom=15,.downloaded_at=123,.map=map,.cancelled=cancel};
    reset(); response=street_json;
    assert(wdg_basemap_fetch(&f)); assert(map->feature_count==1&&map->point_count==2&&f.status==WdgMapClientOk&&f.http_status==200);
    assert(map->downloaded_at==123&&f.bytes_received==strlen(street_json)&&!key_headers);
    assert(strstr(url,"https://overpass-api.de/api/interpreter?data=")==url);
    assert(strstr(url,"%5Bout%3Ajson%5D")&&!strchr(url,' ')&&!strstr(url,key));
    *saved=*map;
    for(unsigned z=WDG_BASE_MIN_ZOOM;z<=20;++z) {
        reset(); response=street_json; f.zoom=(uint8_t)z; f.bounds=bounds;
        assert(wdg_basemap_fetch(&f)&&f.status==WdgMapClientOk&&map->feature_count&&opens==1&&!key_headers);
        assert(strlen(url)<2048); printf("Integrated Z:%u encoded path: %u bytes\n",z,(unsigned)(strlen(url)-strlen("https://overpass-api.de")));
    }
    reset(); f.zoom=15; response=street_json; header_delay=12000000; content_type="Application/JSON; charset=utf-8";
    assert(wdg_basemap_fetch(&f)&&map->feature_count==1&&now==12000000);
    reset(); response=street_json; status=429; retry_after="180";
    assert(!wdg_basemap_fetch(&f)&&f.status==WdgMapClientRateLimit&&f.http_status==429&&f.retry_after_s==180&&!memcmp(map,saved,sizeof(*map)));
    reset(); response=street_json; status=403;
    assert(!wdg_basemap_fetch(&f)&&f.status==WdgMapClientHttp&&strstr(f.message,"Basemap")&&!strstr(f.message,"key")&&!memcmp(map,saved,sizeof(*map)));
    reset(); response=street_json; connect_error=0x8006;
    assert(!wdg_basemap_fetch(&f)&&f.status==WdgMapClientNetwork&&strstr(f.message,"OSM connect 0x8006")&&!memcmp(map,saved,sizeof(*map)));
    reset(); response=street_json; header_error=true;
    assert(!wdg_basemap_fetch(&f)&&f.status==WdgMapClientNetwork&&strstr(f.message,"OSM no response")&&!memcmp(map,saved,sizeof(*map)));
    reset(); response=street_json; complete=false; expected=0;
    assert(!wdg_basemap_fetch(&f)&&f.status==WdgMapClientNetwork&&!memcmp(map,saved,sizeof(*map)));
    reset(); response="{\"remark\":\"runtime error: Query timed out\",\"elements\":[]}";
    assert(!wdg_basemap_fetch(&f)&&f.status==WdgMapClientSchema&&!memcmp(map,saved,sizeof(*map)));
    reset(); response="{\"elements\":[]}";
    assert(wdg_basemap_fetch(&f)&&!map->feature_count&&!map->point_count); /* Empty is explicit, never usable streets. */
    WdgMapSnapshot* territories=calloc(1,sizeof(*territories)); assert(territories);
    WdgMapFetch world={.api_key=key,.bounds={-85.05112878,-180,85.05112878,180},.zoom=0,.downloaded_at=100,.map=territories,.cancelled=cancel};
    world_fetch_checks(&world,territories);
    if(argc>=2) {
        FILE* file=fopen(argv[1],"rb"); assert(file); char* live=malloc(WDG_MAP_RESPONSE_MAX+1); assert(live);
        size_t length=fread(live,1,WDG_MAP_RESPONSE_MAX,file); assert(!ferror(file)&&fgetc(file)==EOF); fclose(file); live[length]=0;
        reset(); response=live; f.bounds=bounds; f.zoom=15;
        assert(wdg_basemap_fetch(&f)&&map->feature_count>0&&map->point_count>0&&!key_headers);
        printf("Captured public response through real fetch/client: %u features, %u points, partial=%u\n",map->feature_count,map->point_count,map->partial);
        free(live);
    }
    if(argc>=3) {
        char* live=read_private(argv[2]); reset(); response=live; chunk_size=1021; world.zoom=0;
        assert(wdg_map_fetch_territories(&world)&&territories->hull_count>128&&territories->point_count>4096&&territories->gang_count>64&&key_headers==1);
        assert(!strcmp(url,"https://wdgwars.pl/api/territories")&&world.bytes_received==strlen(live)&&territories->cells_skipped);
        printf("Private captured overview through real fetch/client: %u gangs, %u hulls, %u points, %u bytes\n",territories->gang_count,territories->hull_count,territories->point_count,(unsigned)world.bytes_received);
        free(live);
    }
    if(argc==4) {
        char* live=read_private(argv[3]);
        char error[80]; WdgMapSnapshot* parsed=calloc(1,sizeof(*parsed)); assert(parsed);
        assert(wdg_map_parse_territories(live,strlen(live),parsed,error,sizeof(error))&&wdg_map_bounds_valid(&parsed->bounds));
        world.bounds=parsed->bounds; free(parsed);
        reset(); response=live; chunk_size=1021; world.zoom=14;
        assert(wdg_map_fetch_territories(&world)&&territories->cell_count==5&&territories->hull_count==2&&territories->point_count==72&&territories->gang_count<583&&key_headers==1);
        assert(strstr(url,"https://wdgwars.pl/api/member-territories?compact=1&bbox=")==url&&strstr(url,"&zoom=14")&&!territories->cells_skipped);
        printf("Private captured local through real fetch/client: %u gangs, %u cells, %u hulls, %u points, %u bytes\n",territories->gang_count,territories->cell_count,territories->hull_count,territories->point_count,(unsigned)world.bytes_received);
        free(live);
    }
    free(territories); free(map); free(saved); puts("WDG map adapter integration: all checks passed"); return 0;
}
