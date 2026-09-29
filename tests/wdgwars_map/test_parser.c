#include "wdgwars_map_json.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const char compact[]="{\"ok\":true,\"grid_lat\":0.02,\"grid_lng\":0.02,\"cells\":[{\"lat\":41.0,\"lng\":-81.0,\"gang_id\":7,\"count\":23}],\"gang_hulls\":[{\"gang_id\":7,\"hull\":[[41,-81],[41.02,-81],[41,-80.98]]}],\"gangs\":{\"7\":{\"name\":\"Gorillas\",\"color\":\"#123AbC\"}},\"bbox\":[40.9,-81.1,41.1,-80.9]}";
static void reject(const char* text) {
    WdgMapSnapshot* m=malloc(sizeof(*m)); assert(m); memset(m,0xa5,sizeof(*m));
    char error[80]; assert(!wdg_map_parse_territories(text,strlen(text),m,error,sizeof(error))); assert(error[0]);
    for(size_t i=0;i<sizeof(*m);++i) assert(((unsigned char*)m)[i]==0xa5); free(m);
}
static bool (*parse)(const char*,size_t,WdgMapSnapshot*,char*,size_t)=wdg_map_parse_territories;
static void reject_diagnostic(const char* text,const char* field,const char* reason) {
    WdgMapSnapshot* m=malloc(sizeof(*m)); assert(m); memset(m,0x5a,sizeof(*m));
    char error[96]; assert(!parse(text,strlen(text),m,error,sizeof(error)));
    assert(strstr(error,"@B")&&strstr(error,field)&&strstr(error,reason));
    assert(!strstr(error,"PRIVATE_LABEL"));
    for(size_t i=0;i<sizeof(*m);++i) assert(((unsigned char*)m)[i]==0x5a);
    free(m);
}
static void appendf(char* text,size_t cap,size_t* used,const char* format,...) {
    va_list args; va_start(args,format); int n=vsnprintf(text+*used,cap-*used,format,args); va_end(args);
    assert(n>=0&&(size_t)n<cap-*used); *used+=(size_t)n;
}
static void dictionary_tests(void) {
    const unsigned count=WDG_MAP_MAX_GANGS+80;
    const size_t cap=(size_t)count*100+2000;
    char* text=malloc(cap); WdgMapSnapshot* m=malloc(sizeof(*m)); assert(text&&m); char error[96];
    const char geometry[]="\"cells\":[{\"lat\":1,\"lng\":2,\"gang_id\":1},{\"lat\":1.1,\"lng\":2,\"gang_id\":2}],\"gang_hulls\":[{\"gang_id\":3,\"hull\":[[1,2],[2,2],[1,3]]}]";
    /* More metadata than the bounded model can hold is normal. JSON order must
     * not matter; referenced geometry dictates which metadata is retained. */
    for(unsigned order=0;order<3;++order) {
        size_t used=0; appendf(text,cap,&used,"{\"ok\":true,\"grid_lat\":0.02,\"grid_lng\":0.02,");
        if(order==0) appendf(text,cap,&used,"%s,",geometry);
        appendf(text,cap,&used,"\"gangs\":{");
        for(unsigned i=1;i<=count;++i) appendf(text,cap,&used,"%s\"%u\":{\"name\":\"Team %u\",\"color\":\"#123456\"}",i==1?"":",",i,i);
        appendf(text,cap,&used,"}");
        if(order==1) appendf(text,cap,&used,",%s",geometry);
        if(order==2) appendf(text,cap,&used,",\"cells\":[]");
        appendf(text,cap,&used,"}");
        assert(wdg_map_parse_territories(text,used,m,error,sizeof(error)));
        assert(m->gang_count==(order==2?0:3));
        if(order!=2) { assert(m->cell_count==2&&m->hull_count==1); assert(m->hulls[0].color_rgb==0x123456); assert(!strcmp(m->gangs[2].name,"Team 3")); }
    }
    reject_diagnostic("{\"ok\":true,\"cells\":[],\"gangs\":{\"1\":{\"color\":\"#123456\"},\"01\":{\"color\":\"#123456\"}}}","gangs[].id","duplicate ID");
    reject_diagnostic("{\"ok\":true,\"cells\":[],\"gangs\":{\"1\":{\"name\":\"PRIVATE_LABEL\",\"color\":\"red\"}}}","gangs[].color","#RRGGBB");
    reject_diagnostic("{\"ok\":true,\"cells\":[],\"gangs\":{\"1\":{\"name\":\"PRIVATE_LABEL\"}}}","gangs[].color","missing field");
    char long_name[300]; snprintf(long_name,sizeof(long_name),"{\"ok\":true,\"cells\":[],\"gangs\":{\"1\":{\"name\":\"%0*d\",\"color\":\"#123456\"}}}",WDG_MAP_NAME_SIZE,0);
    reject_diagnostic(long_name,"gangs[].name","string too long");
    reject_diagnostic("{\"ok\":true,\"grid_lat\":0.02,\"grid_lng\":0.02,\"cells\":[{\"lat\":91,\"lng\":0,\"gang_id\":1,\"color\":\"#abcdef\"}]}","cells[].lat","out of range");
    reject_diagnostic("{\"ok\":true,\"cells\":[],\"cells\":[]}","cells","duplicate field");
    free(text); free(m);
}
static void overview_tests(void) {
    WdgMapSnapshot* m=malloc(sizeof(*m)); assert(m); char error[96]; parse=wdg_map_parse_overview;
    const char two[]="[{\"gang_id\":7,\"name\":\"Team A\",\"color\":\"#123456\",\"logo\":null,\"members\":3,\"points\":20,\"rank\":1,\"hull\":[[1,2],[3,4],[5,6]]},{\"hull\":[[-1,-2],[-3,-4],[-5,-6]],\"name\":\"Team A\",\"gang_id\":7,\"color\":\"#123456\",\"logo\":\"/logo.png\"}]";
    assert(parse(two,strlen(two),m,error,sizeof(error)));
    assert(m->hull_count==2&&m->point_count==6&&m->gang_count==1&&!m->cell_count&&m->cells_skipped);
    assert(m->hulls[0].first_point==0&&m->hulls[1].first_point==3&&m->hulls[0].point_count==3&&m->hulls[1].point_count==3);
    assert(m->points[0].lat_e6==1000000&&m->points[5].lon_e6==-6000000);
    assert(m->hulls[1].gang_id==7&&m->hulls[1].color_rgb==0x123456&&!strcmp(m->hulls[1].name,"Team A"));
    assert(wdg_map_bounds_valid(&m->bounds)&&m->bounds.west==-180&&m->bounds.east==180);
    assert(parse("[]",2,m,error,sizeof(error))&&!m->hull_count&&!m->gang_count&&m->cells_skipped);
    for(size_t n=0;n<strlen(two);++n) { memset(m,0x66,sizeof(*m)); assert(!parse(two,n,m,error,sizeof(error))); assert(((unsigned char*)m)[0]==0x66); }
    reject_diagnostic("{}","overview","array expected");
    reject_diagnostic("[{\"gang_id\":1,\"name\":\"PRIVATE_LABEL\",\"color\":\"#123456\"}]","overview[].hull","missing field");
    reject_diagnostic("[{\"gang_id\":1,\"gang_id\":1}]","overview[].gang_id","duplicate field");
    reject_diagnostic("[{\"gang_id\":null}]","overview[].gang_id","invalid value");
    reject_diagnostic("[{\"hull\":[[0,0],[0,1]]}]","hull","fewer than 3");
    reject_diagnostic("[{\"hull\":[[0,181],[0,1],[1,1]]}]","hull point","out of range");
    reject_diagnostic("[{\"members\":-1}]","overview[].count","invalid value");
    const size_t cap=WDG_MAP_RESPONSE_MAX; char* text=malloc(cap); assert(text);
    size_t used=0; appendf(text,cap,&used,"[");
    for(unsigned i=0;i<=WDG_MAP_MAX_HULLS;++i) appendf(text,cap,&used,"%s{\"gang_id\":1,\"name\":\"Team A\",\"color\":\"#123456\",\"hull\":[[1,2],[3,4],[5,6]]}",i?",":"");
    appendf(text,cap,&used,"]"); reject_diagnostic(text,"overview hulls","limit");
    used=0; appendf(text,cap,&used,"[");
    for(unsigned i=0;i<=WDG_MAP_MAX_GANGS;++i) appendf(text,cap,&used,"%s{\"gang_id\":%u,\"name\":\"Team %u\",\"color\":\"#123456\",\"hull\":[[1,2],[3,4],[5,6]]}",i?",":"",i+1,i+1);
    appendf(text,cap,&used,"]"); reject_diagnostic(text,"gangs","limit");
    used=0; appendf(text,cap,&used,"[{\"gang_id\":1,\"name\":\"Team A\",\"color\":\"#123456\",\"hull\":[");
    for(unsigned i=0;i<=WDG_MAP_MAX_POINTS;++i) appendf(text,cap,&used,"%s[1,2]",i?",":"");
    appendf(text,cap,&used,"]}]"); reject_diagnostic(text,"hull points","limit");
    memset(m,0x66,sizeof(*m)); assert(!parse("[]",WDG_MAP_RESPONSE_MAX+1,m,error,sizeof(error))&&strstr(error,"exceeds")); assert(((unsigned char*)m)[0]==0x66);
    free(text); free(m); parse=wdg_map_parse_territories;
}
int main(void) {
    WdgMapSnapshot* m=calloc(1,sizeof(*m)); assert(m); char error[80];
    assert(wdg_map_parse_territories(compact,strlen(compact),m,error,sizeof(error)));
    assert(m->cell_count==1&&m->hull_count==1&&m->gang_count==1&&m->point_count==3);
    assert(m->cells[0].lat_e6==41000000&&m->cells[0].lon_e6==-81000000&&m->cells[0].color_rgb==0x123abc);
    assert(m->hulls[0].color_rgb==0x123abc&&m->grid_lat_e6==20000&&m->grid_lon_e6==20000);
    assert(m->cell_anchor==WdgMapCellAnchorUnknown&&!m->source_time_valid);
    assert(!strcmp(m->gangs[0].name,"Gorillas")&&m->bounds.west==-81.1);
    const char expanded[]="{\"ok\":true,\"grid_lat\":0.02,\"grid_lng\":0.02,\"cells\":[{\"lat\":-0.01,\"lng\":-0.01,\"gang_id\":1,\"color\":\"#FF0000\",\"gang\":\"A\"}],\"gangs\":{\"1\":{\"name\":\"A\",\"color\":\"#ff0000\"}},\"unknown\":{\"x\":[null,true,false,{\"s\":\"\\u0000\\n\"}]}}";
    assert(wdg_map_parse_territories(expanded,strlen(expanded),m,error,sizeof(error)));
    assert(m->cells[0].lat_e6==-10000&&m->gang_count==1);
    const char solo[]="{\"ok\":true,\"grid_lat\":0.02,\"grid_lng\":0.02,\"cells\":[{\"lat\":41,\"lng\":-81,\"gang_id\":null,\"color\":\"#ff0000\",\"gang\":null},{\"lat\":42,\"lng\":-80,\"gang_id\":null,\"color\":\"#00ff00\",\"gang\":null}],\"gang_hulls\":[{\"gang\":\"Named team\",\"color\":\"#123456\",\"hull\":[[41,-81],[41,-80],[42,-81]]}]}";
    assert(wdg_map_parse_territories(solo,strlen(solo),m,error,sizeof(error)));
    assert(!m->gang_count&&m->cells[0].color_rgb==0xff0000&&m->cells[1].color_rgb==0x00ff00);
    assert(m->hull_count==1&&!m->hulls[0].gang_id&&!strcmp(m->hulls[0].name,"Named team"));
    reject("<html>Login</html>"); reject("{\"ok\":false,\"cells\":[]}"); reject("{\"ok\":true,\"cells\":[],\"cells\":[]}");
    reject("{\"ok\":true,\"cells\":[],}"); reject("{\"ok\":true,\"cells\":[{\"lat\":41,\"lng\":-81,\"gang_id\":1}]}");
    reject("{\"ok\":true,\"grid_lat\":0.02,\"grid_lng\":0.02,\"cells\":[{\"lat\":91,\"lng\":0,\"gang_id\":1,\"color\":\"#abcdef\"}]}");
    reject("{\"ok\":true,\"grid_lat\":0.02,\"grid_lng\":0.02,\"cells\":[{\"lat\":0,\"lng\":0,\"gang_id\":4294967296,\"color\":\"#abcdef\"}]}");
    reject("{\"ok\":true,\"cells\":[],\"unknown\":NaN}"); reject("{\"ok\":true,\"cells\":[],\"unknown\":01}");
    reject("{\"ok\":true,\"cells\":[],\"unknown\":1e999}"); reject("{\"ok\":true,\"cells\":[],\"unknown\":\"\\ud800\"}");
    reject("{\"ok\":true,\"cells\":[],\"unknown\":\"\xc0\x80\"}");
    reject("{\"ok\":true,\"cells\":[],\"gang_hulls\":[{\"gang_id\":1,\"color\":\"#abcdef\",\"hull\":[[0,0],[0,1]]}]}");
    reject("{\"ok\":true,\"cells\":[],\"gangs\":{\"1\":{\"name\":\"A\",\"color\":\"#aabbcc\"},\"1\":{\"name\":\"B\",\"color\":\"#ff0000\"}}}");
    for(size_t n=0;n<strlen(compact);++n) { memset(m,0x66,sizeof(*m)); assert(!wdg_map_parse_territories(compact,n,m,error,sizeof(error))); assert(((unsigned char*)m)[0]==0x66); }
    char* dense=malloc((WDG_MAP_MAX_CELLS+1)*100+200); assert(dense); strcpy(dense,"{\"ok\":true,\"grid_lat\":0.02,\"grid_lng\":0.02,\"cells\":[");
    for(unsigned i=0;i<=WDG_MAP_MAX_CELLS;++i) strcat(dense,i?",{\"lat\":0,\"lng\":0,\"gang_id\":1,\"color\":\"#ffffff\"}":"{\"lat\":0,\"lng\":0,\"gang_id\":1,\"color\":\"#ffffff\"}");
    strcat(dense,"]}"); reject(dense); free(dense);
    WdgMapAccount a={0}; const char account[]="{\"ok\":true,\"username\":\"Salik\",\"gang\":null,\"gang_id\":null,\"wifi\":50,\"ble\":10,\"total\":60,\"badges\":[\"abc\"]}";
    assert(wdg_map_parse_account(account,strlen(account),&a,error,sizeof(error))); assert(a.wifi==50&&a.total==60&&a.gang_id==0&&!strcmp(a.username,"Salik"));
    WdgMapAccount previous=a; const char failure[]="{\"ok\":true,\"username\":\"Bad\",\"wifi\":-1}";
    assert(!wdg_map_parse_account(failure,strlen(failure),&a,error,sizeof(error))); assert(!memcmp(&a,&previous,sizeof(a)));
    WdgJson j; char label[5]; const char* unicode="\"A\\u20acBmore\"";
    wdg_json_init(&j,unicode,strlen(unicode)); assert(wdg_json_string_truncated(&j,label,sizeof(label))&&wdg_json_finish(&j)); assert(!strcmp(label,"A\xe2\x82\xac"));
    wdg_json_init(&j,unicode,strlen(unicode)); assert(!wdg_json_string(&j,label,sizeof(label)));
    wdg_json_init(&j,"[1,]",4); assert(!wdg_json_skip(&j));
    wdg_json_init(&j,"[1,2] junk",10); assert(wdg_json_skip(&j)&&!wdg_json_finish(&j));
    dictionary_tests(); overview_tests();
    free(m); puts("WDG map parser: all checks passed"); return 0;
}
