#include "wdgwars_map_basemap.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const WdgMapBounds area = {51.476, -.006, 51.48, .006};
static const char roads[] = "{\"version\":0.6,\"elements\":["
    "{\"type\":\"way\",\"id\":9000000001,\"geometry\":[{\"lat\":51.477,\"lon\":-.003},{\"lat\":51.478,\"lon\":.003}],"
    "\"tags\":{\"highway\":\"primary\",\"name\":\"Queen's Road\"}},"
    "{\"tags\":{\"natural\":\"water\",\"name\":\"Pond\"},\"type\":\"way\","
    "\"geometry\":[{\"lat\":51.477,\"lon\":0},{\"lat\":51.478,\"lon\":0},{\"lat\":51.478,\"lon\":.001},{\"lat\":51.477,\"lon\":0}]},"
    "{\"type\":\"way\",\"tags\":{\"leisure\":\"park\"},\"geometry\":[{\"lat\":51.477,\"lon\":0},{\"lat\":51.478,\"lon\":.001}]}]}";
static char response[WDG_MAP_RESPONSE_MAX + 1U];
static WdgMapClientStatus network_status;
static unsigned calls;
static bool cancelled;
static char requested_path[WDG_BASE_PATH_SIZE];
static bool cancel_check(void* context) { assert(context == &cancelled); return cancelled; }

bool wdg_map_http_get(WdgMapHttpRequest* r) {
    ++calls;
    assert(r->service == WdgMapServiceOverpass && !r->api_key);
    assert(!strncmp(r->path, "/api/interpreter?data=", 22));
    assert(strlen(r->path) < 1800);
    snprintf(requested_path, sizeof(requested_path), "%s", r->path);
    r->status = network_status;
    r->http_status = network_status == WdgMapClientOk ? 200 : 429;
    r->retry_after_s = network_status == WdgMapClientRateLimit ? 120 : 0;
    if(network_status != WdgMapClientOk) { snprintf(r->message, sizeof(r->message), "rate limited"); return false; }
    r->length = strlen(response); assert(r->length < r->capacity);
    memcpy(r->body, response, r->length + 1);
    return true;
}
static WdgMapBasemap out, saved;
static char error[64];
static bool parse(const char* json) { return wdg_basemap_parse(json, strlen(json), &area, 1777777777, &out, error, sizeof(error)); }
static void unchanged_failure(const char* json) {
    saved = out; assert(!parse(json)); assert(!memcmp(&out, &saved, sizeof(out)));
}
static void valid_roads(void) {
    /* Synthetic deterministic boundary fixture, never treated as a live map. */
    snprintf(response, sizeof(response), "%s", roads);
    /* JSON does not allow .003; make valid decimal text explicitly. */
    char fixed[4096]; size_t used = 0;
    for(size_t i = 0; response[i]; ++i) {
        if(response[i] == '.' && (i == 0 || response[i-1] < '0' || response[i-1] > '9')) fixed[used++] = '0';
        fixed[used++] = response[i];
    }
    fixed[used] = 0; strcpy(response, fixed);
    assert(parse(response));
    assert(out.feature_count == 3 && out.point_count == 8 && !out.partial);
    assert(out.features[0].kind == WdgBaseRoadMajor && !out.features[0].closed);
    assert(!strcmp(out.features[0].name, "Queen's Road"));
    assert(out.features[1].kind == WdgBaseWater && out.features[1].closed);
    assert(out.features[2].kind == WdgBasePark && !out.features[2].closed);
    assert(out.points[0].lat_e6 == 51477000 && out.points[0].lon_e6 == -3000);
    assert(out.downloaded_at == 1777777777 && wdg_basemap_valid(&out));
}
static void parser_boundaries(void) {
    valid_roads();
    unchanged_failure("{\"elements\":[],\"remark\":\"runtime error: timeout\"}");
    unchanged_failure("{\"elements\":[],\"elements\":[]}");
    unchanged_failure("{\"elements\":[]} trailing");
    unchanged_failure("<html>proxy error</html>");
    unchanged_failure("{}");
    unchanged_failure("{\"elements\":[{\"type\":\"way\",\"tags\":{\"highway\":\"path\"},\"geometry\":[{\"lat\":999,\"lon\":0}]}]}");
    unchanged_failure("{\"elements\":[{\"type\":\"way\",\"tags\":{\"highway\":\"path\"},\"geometry\":[{\"lat\":0}]}]}");
    assert(parse("{\"elements\":[]}")); assert(!out.feature_count && !out.partial);
    assert(parse("{\"elements\":[{\"type\":\"way\",\"tags\":{\"highway\":\"path\"},\"geometry\":[{\"lat\":51.477,\"lon\":0},{\"lat\":51.478,\"lon\":0},null,{\"lat\":51.479,\"lon\":0},{\"lat\":51.48,\"lon\":0}]}]}"));
    assert(out.partial && out.feature_count == 2 && out.point_count == 4);
    assert(out.features[0].point_count == 2 && out.features[1].first_point == 2);
    assert(!out.features[0].closed && !out.features[1].closed);
    assert(parse("{\"elements\":[{\"type\":\"relation\",\"tags\":{\"natural\":\"water\"},\"members\":[]},{\"type\":\"way\",\"tags\":{\"highway\":\"construction\"},\"geometry\":[]}]}"));
    assert(out.partial && !out.feature_count);
    assert(parse("{\"elements\":[{\"type\":\"count\",\"tags\":{\"total\":\"25\"}}]}"));
    assert(out.partial && !out.feature_count);
    assert(parse("{\"elements\":[{\"type\":\"count\",\"tags\":{\"total\":\"0\"}}]}"));
    assert(!out.partial && !out.feature_count);
    unchanged_failure("{\"elements\":[{\"type\":\"count\",\"tags\":{\"total\":\"-1\"}}]}");
    unchanged_failure("{\"elements\":[{\"type\":\"count\",\"tags\":{\"total\":\"4294967296\"}}]}");
    valid_roads();
    saved = out;
    out.features[0].first_point = 4095; assert(!wdg_basemap_valid(&out)); out = saved;
    out.features[0].kind = (WdgBaseKind)99; assert(!wdg_basemap_valid(&out)); out = saved;
    out.features[0].closed = true; assert(!wdg_basemap_valid(&out)); out = saved;
    out.points[0].lat_e6 = INT32_MIN; assert(!wdg_basemap_valid(&out)); out = saved;
    memset(out.features[0].name, 'X', sizeof(out.features[0].name)); assert(!wdg_basemap_valid(&out)); out = saved;
    assert(!wdg_basemap_parse(response, WDG_MAP_RESPONSE_MAX+1, &area, 0, &out, error, sizeof(error)));
    assert(!memcmp(&out, &saved, sizeof(out)));
}
static size_t put(size_t n, const char* text) { size_t len = strlen(text); assert(n+len<sizeof(response)); memcpy(response+n,text,len+1); return n+len; }
static void limits(void) {
    size_t n = put(0,"{\"elements\":[");
    for(unsigned i=0; i<257; ++i) {
        if(i) n=put(n,",");
        n=put(n,"{\"type\":\"way\",\"tags\":{\"highway\":\"residential\"},\"geometry\":[{\"lat\":51.477,\"lon\":0},{\"lat\":51.478,\"lon\":0}]}");
    }
    put(n,"]}"); assert(parse(response)); assert(out.partial && out.feature_count == 256 && out.point_count == 512);
    n=put(0,"{\"elements\":[{\"type\":\"way\",\"tags\":{\"natural\":\"water\"},\"geometry\":[");
    for(unsigned i=0;i<513;++i) { if(i) n=put(n,","); n=put(n,"{\"lat\":51.477,\"lon\":0}"); }
    put(n,"]}]}"); assert(parse(response)); assert(out.partial && !out.feature_count && !out.point_count);
    n=put(0,"{\"elements\":[");
    for(unsigned i=0;i<9;++i) {
        if(i) n=put(n,","); n=put(n,"{\"type\":\"way\",\"tags\":{\"highway\":\"path\"},\"geometry\":[");
        for(unsigned k=0;k<512;++k) { if(k) n=put(n,","); n=put(n,"{\"lat\":51.477,\"lon\":0}"); }
        n=put(n,"]}");
    }
    put(n,"]}"); assert(parse(response)); assert(out.partial && out.point_count == 4096 && out.feature_count == 8);
    n=put(0,"{\"elements\":[{\"type\":\"way\",\"tags\":{\"highway\":\"path\",\"name\":\"");
    for(unsigned i=0;i<200;++i) n=put(n,"\xE2\x82\xAC");
    put(n,"\"},\"geometry\":[{\"lat\":51.477,\"lon\":0},{\"lat\":51.478,\"lon\":0}]}]}");
    assert(parse(response)); assert(strlen(out.features[0].name)==39); /* 13 complete UTF-8 characters. */
}
static void add_remark(const char* remark) {
    size_t n=strlen(response); assert(n && response[n-1]=='}');
    snprintf(response+n-1,sizeof(response)-(n-1),",\"remark\":\"%s\"}",remark);
}
static void server_partial_results(void) {
    valid_roads();
    add_remark("runtime error: Query timed out in \\\"query\\\" at line 1 after 20 seconds.\\n");
    assert(parse(response)); assert(out.partial && out.feature_count==3);
    assert(!strcmp(error,"Partial streets: Overpass timeout"));
    valid_roads();
    add_remark("runtime error: Query run out of memory using about 32 MB of RAM.");
    assert(parse(response)); assert(out.partial && out.feature_count==3);
    assert(!strcmp(error,"Partial streets: Overpass memory limit"));
    saved=out;
    size_t n=strlen(response); response[n-1]=0; /* Complete geometry but incomplete outer JSON. */
    assert(!parse(response) && !memcmp(&saved,&out,sizeof(out)));
    assert(!strcmp(error,"Invalid basemap response"));
    valid_roads(); add_remark("runtime error: unknown internal query failure");
    unchanged_failure(response);
    valid_roads(); add_remark("runtime error: server is too busy"); unchanged_failure(response);
    assert(!strcmp(error,"Overpass busy; retry later"));
    unchanged_failure("{\"elements\":[],\"remark\":\"runtime error: Query timed out in query after 20 seconds.\"}");
    assert(!strcmp(error,"Overpass timed out; zoom in and retry"));
    unchanged_failure("{\"elements\":[],\"remark\":\"runtime error: Query run out of memory using about 32 MB of RAM.\"}");
    assert(!strcmp(error,"Overpass memory limit; zoom in and retry"));
    unchanged_failure("{\"elements\":[],\"remark\":\"Too many requests\"}");
    assert(!strcmp(error,"Overpass rate limit; retry later"));
    unchanged_failure("{\"elements\":[{\"type\":\"way\",\"tags\":{\"highway\":\"primary\"},\"geometry\":[{\"lat\":0,\"lon\":0},{\"lat\":1,\"lon\":1}]}],\"remark\":\"Query timed out\"}");
    unchanged_failure("{\"elements\":[{\"type\":\"way\",\"tags\":{\"highway\":\"primary\"},\"geometry\":[{\"lat\":51.477,\"lon\":0},{\"lat\":51.477,\"lon\":0}]}],\"remark\":\"Query timed out\"}");
    unchanged_failure("{\"elements\":[{\"type\":\"way\",\"tags\":{\"highway\":\"primary\"},\"geometry\":[{\"lat\":999,\"lon\":0},{\"lat\":51.477,\"lon\":0}]}],\"remark\":\"Query timed out\"}");
    unchanged_failure("{\"elements\":[],\"remark\":\"Query timed out\",\"remark\":\"Query run out of memory\"}");
    /* Genuine crossing segment remains useful even with both endpoints outside. */
    assert(parse("{\"elements\":[{\"type\":\"way\",\"tags\":{\"highway\":\"primary\"},\"geometry\":[{\"lat\":51.478,\"lon\":-0.1},{\"lat\":51.478,\"lon\":0.1}]}],\"remark\":\"Query timed out\"}"));
    assert(out.partial && out.feature_count==1);
    /* A good HTTP body late in a refresh can commit a labelled partial result. */
    valid_roads(); add_remark("runtime error: Query timed out after 20 seconds.");
    network_status=WdgMapClientOk; cancelled=false;
    WdgBasemapFetch f={.bounds=area,.zoom=15,.downloaded_at=123,.map=&out};
    assert(wdg_basemap_fetch(&f) && f.status==WdgMapClientOk && out.partial);
    assert(strstr(f.message,"Partial streets") && out.downloaded_at==123);
    saved=out; strcpy(response,"{\"elements\":[],\"remark\":\"Query run out of memory\"}");
    assert(!wdg_basemap_fetch(&f) && f.status==WdgMapClientSchema && !memcmp(&saved,&out,sizeof(out)));
}
static void query_and_fetch(void) {
    WdgMapBounds view = {51.4, -.1, 51.6, .1}, b;
    assert(wdg_basemap_bounds(&view,13,&b));
    assert(fabs((b.north+b.south)*.5-51.5)<1e-9);
    assert((b.north-b.south)*111320.0 <= 4000.01);
    assert((b.east-b.west)*111320*cos(b.south*3.141592653589793/180)<=4000.01);
    assert(!wdg_basemap_bounds(&view,8,&b));
    WdgMapBounds dateline={0,179,1,-179}; assert(!wdg_basemap_bounds(&dateline,15,&b));
    WdgMapBounds nan={NAN,0,1,1}; assert(!wdg_basemap_bounds(&nan,15,&b));
    char path[WDG_BASE_PATH_SIZE]; assert(wdg_basemap_query(&area,15,path,sizeof(path)));
    assert(strstr(path,"%5Bout%3Ajson%5D") && strstr(path,"qt%20160") && strstr(path,"out%20count"));
    assert(!strchr(path,' ') && !strchr(path,'\n') && !strstr(path,"api_key"));
    assert(strstr(path,"maxsize%3A33554432") && !strstr(path,"maxsize%3A8388608"));
    assert(!strstr(path,"way.r") && !strstr(path,"-.c"));
    const char* first_output=strstr(path,".c%20out%20body");
    const char* first_difference=strstr(path,"%28._%3B%20-%20.c%3B%29");
    assert(first_output && first_difference && first_output<first_difference);
    assert(!wdg_basemap_query(&view,15,path,sizeof(path))); /* caller must clamp first */
    assert(!wdg_basemap_query(&area,15,path,10));
    const double limits[]={50000,30000,16000,8000,4000};
    WdgMapBounds huge={50,-2,53,2};
    for(unsigned z=9;z<=13;++z) {
        assert(wdg_basemap_bounds(&huge,(uint8_t)z,&b));
        assert((b.north-b.south)*111320.0<=limits[z-9]+.01);
        assert(wdg_basemap_query(&b,(uint8_t)z,path,sizeof(path)) && strlen(path)<1800);
        if(z<13) assert(!strstr(path,"natural") && !strstr(path,"residential"));
    }
    valid_roads(); network_status=WdgMapClientOk; calls=0;
    WdgBasemapFetch f={.bounds=view,.zoom=15,.downloaded_at=42,.map=&out,.cancelled=cancel_check,.context=&cancelled};
    assert(wdg_basemap_fetch(&f) && calls==1 && f.status==WdgMapClientOk);
    assert(out.downloaded_at==42 && out.bounds.south==f.bounds.south && f.bytes_received==strlen(response));
    saved=out; network_status=WdgMapClientRateLimit;
    assert(!wdg_basemap_fetch(&f) && f.retry_after_s==120 && calls==2 && !memcmp(&out,&saved,sizeof(out)));
    network_status=WdgMapClientOk; strcpy(response,"{\"remark\":\"error\",\"elements\":[]}");
    assert(!wdg_basemap_fetch(&f) && f.status==WdgMapClientSchema && !memcmp(&out,&saved,sizeof(out)));
    cancelled=true; unsigned before=calls;
    assert(!wdg_basemap_fetch(&f) && f.status==WdgMapClientCancelled && before==calls);
    cancelled=false; f.zoom=8;
    assert(!wdg_basemap_fetch(&f) && f.status==WdgMapClientInvalid && before==calls);
}
int main(int argc, char** argv) {
    if(argc>=2 && !strcmp(argv[1], "--print-query")) {
        char path[WDG_BASE_PATH_SIZE];
        WdgMapBounds query_area=area, actual; unsigned zoom=15;
        if(argc==7) {
            query_area=(WdgMapBounds){strtod(argv[2],NULL),strtod(argv[3],NULL),strtod(argv[4],NULL),strtod(argv[5],NULL)};
            zoom=(unsigned)strtoul(argv[6],NULL,10);
        }
        assert(wdg_basemap_bounds(&query_area,(uint8_t)zoom,&actual));
        assert(wdg_basemap_query(&actual, (uint8_t)zoom, path, sizeof(path)));
        puts(path); return 0;
    }
    parser_boundaries(); limits(); query_and_fetch(); server_partial_results();
    if((argc==3 || argc==7) && !strcmp(argv[1],"--parse-file")) {
        FILE* file=fopen(argv[2],"rb"); assert(file);
        size_t n=fread(response,1,sizeof(response)-1,file); assert(!ferror(file)); fclose(file); response[n]=0;
        WdgMapBounds fixture_bounds=area;
        if(argc==7) fixture_bounds=(WdgMapBounds){strtod(argv[3],NULL),strtod(argv[4],NULL),strtod(argv[5],NULL),strtod(argv[6],NULL)};
        assert(wdg_basemap_parse(response,n,&fixture_bounds,1777777777,&out,error,sizeof(error)));
        assert(out.bounds.south==fixture_bounds.south && out.bounds.west==fixture_bounds.west);
        printf("Live fixture parsed: %u features, %u points, partial=%d\n",out.feature_count,out.point_count,out.partial);
    }
    puts("PASS basemap geometry, bounds, transactional errors, limits, UTF-8 names, fixed-host fetch/cancellation");
    return 0;
}
