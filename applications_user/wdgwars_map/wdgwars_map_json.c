#include "wdgwars_map_json.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif
#define WDG_STRINGIFY_INNER(x) #x
#define WDG_STRINGIFY(x) WDG_STRINGIFY_INNER(x)
static bool bad_reason(WdgJson* j,const char* reason) {
    if(!j->error) { j->reason=reason; j->error_field=j->field; j->error_offset=j->offset; }
    j->error=true; return false;
}
static bool bad(WdgJson* j) { return bad_reason(j,"invalid value"); }
static bool at(WdgJson* j,const char* field) { j->field=field; return true; }
static void ws(WdgJson* j) { while(j->offset<j->length && (j->data[j->offset]==' ' || j->data[j->offset]=='\r' || j->data[j->offset]=='\n' || j->data[j->offset]=='\t')) ++j->offset; }
static bool take(WdgJson* j,char c) {
    ws(j); if(j->error) return false;
    if(j->offset==j->length) return bad_reason(j,"unexpected end");
    if(j->data[j->offset]!=c) return bad_reason(j,c=='{'?"object expected":c=='['?"array expected":c=='"'?"string expected":c==':'?"colon expected":c==','?"comma expected":"delimiter expected");
    ++j->offset; return true;
}
void wdg_json_init(WdgJson* j,const char* data,size_t length) { *j=(WdgJson){.data=data,.length=length,.error=!data || !length,.field="response"}; }
static bool container(WdgJson* j,char c) { if(j->depth>=20) return bad_reason(j,"nesting >20"); if(!take(j,c)) return false; ++j->depth; return true; }
bool wdg_json_object(WdgJson* j) { return container(j,'{'); }
bool wdg_json_array(WdgJson* j) { return container(j,'['); }
static int hex(char c) { return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1; }
static bool codeunit(WdgJson* j,unsigned* value) { if(j->length-j->offset<4) return bad(j); *value=0; for(unsigned i=0;i<4;++i) { int h=hex(j->data[j->offset++]); if(h<0) return bad(j); *value=*value*16+(unsigned)h; } return true; }
static bool append(WdgJson* j,char* text,size_t capacity,size_t* used,unsigned code,bool truncate,bool* full) {
    unsigned char b[4]; size_t n;
    if(code<0x80) { b[0]=(unsigned char)code; n=1; }
    else if(code<0x800) { b[0]=0xc0|(code>>6); b[1]=0x80|(code&63); n=2; }
    else if(code<0x10000) { b[0]=0xe0|(code>>12); b[1]=0x80|((code>>6)&63); b[2]=0x80|(code&63); n=3; }
    else { b[0]=0xf0|(code>>18); b[1]=0x80|((code>>12)&63); b[2]=0x80|((code>>6)&63); b[3]=0x80|(code&63); n=4; }
    if(!code || code<32 || code==127) return bad_reason(j,"control in label");
    if(text) { if(*full) return true; if(*used+n>=capacity) { if(!truncate) return bad_reason(j,"string too long"); *full=true; return true; } memcpy(text+*used,b,n); } *used+=n; return true;
}
static bool string_read(WdgJson* j,char* text,size_t capacity,bool truncate) {
    if(!take(j,'"')) return false;
    size_t used=0; bool full=false;
    while(j->offset<j->length) {
        unsigned c=(unsigned char)j->data[j->offset++];
        if(c<32) return bad(j);
        if(c=='"') { if(text) { if(used>=capacity) return bad(j); text[used]=0; } return true; }
        if(c=='\\') {
            if(j->offset==j->length) return bad(j);
            c=(unsigned char)j->data[j->offset++];
            if(c=='u') {
                if(!codeunit(j,&c)) return false;
                if(c>=0xd800 && c<=0xdbff) { unsigned low; if(j->length-j->offset<6 || j->data[j->offset++]!='\\' || j->data[j->offset++]!='u' || !codeunit(j,&low) || low<0xdc00 || low>0xdfff) return bad(j); c=0x10000+((c-0xd800)<<10)+(low-0xdc00); }
                else if(c>=0xdc00 && c<=0xdfff) return bad(j);
                if(c<32 && !text) continue;
            } else if(c!='"' && c!='\\' && c!='/') {
                /* JSON escapes for controls are valid in skipped text, never labels. */
                if(c!='b' && c!='f' && c!='n' && c!='r' && c!='t') return bad(j);
                if(text) return bad(j);
                continue;
            }
        } else if(c>=0x80) {
            unsigned n,min;
            if(c>=0xc2 && c<=0xdf) { n=1; min=0x80; c&=31; }
            else if(c>=0xe0 && c<=0xef) { n=2; min=0x800; c&=15; }
            else if(c>=0xf0 && c<=0xf4) { n=3; min=0x10000; c&=7; }
            else return bad(j);
            for(unsigned i=0;i<n;++i) { if(j->offset==j->length) return bad(j); unsigned b=(unsigned char)j->data[j->offset++]; if((b&0xc0)!=0x80) return bad(j); c=(c<<6)|(b&63); }
            if(c<min || c>0x10ffff || (c>=0xd800&&c<=0xdfff)) return bad(j);
        }
        if(!append(j,text,capacity,&used,c,truncate,&full)) return false;
    }
    return bad(j);
}
bool wdg_json_string(WdgJson* j,char* text,size_t capacity) { return string_read(j,text,capacity,false); }
bool wdg_json_string_truncated(WdgJson* j,char* text,size_t capacity) { return text&&capacity&&string_read(j,text,capacity,true); }
static int next(WdgJson* j,bool* first,char end) {
    ws(j); if(j->error || j->offset==j->length) { bad(j); return -1; }
    if(j->data[j->offset]==end) { ++j->offset; if(!j->depth) { bad(j); return -1; } --j->depth; return 0; }
    if(!*first && !take(j,',')) return -1;
    *first=false; return 1;
}
int wdg_json_member(WdgJson* j,bool* first,char* key,size_t cap) { int n=next(j,first,'}'); if(n==1 && (!wdg_json_string(j,key,cap)||!take(j,':'))) return -1; return n; }
int wdg_json_member_truncated(WdgJson* j,bool* first,char* key,size_t cap) { int n=next(j,first,'}'); if(n==1 && (!wdg_json_string_truncated(j,key,cap)||!take(j,':'))) return -1; return n; }
int wdg_json_element(WdgJson* j,bool* first) { return next(j,first,']'); }
bool wdg_json_number(WdgJson* j,double* result) {
    ws(j); size_t start=j->offset; if(start==j->length) return bad(j);
    if(j->data[j->offset]=='-') ++j->offset;
    if(j->offset==j->length) return bad(j);
    if(j->data[j->offset]=='0') ++j->offset;
    else { if(j->data[j->offset]<'1'||j->data[j->offset]>'9') return bad(j); do { ++j->offset; } while(j->offset<j->length && j->data[j->offset]>='0'&&j->data[j->offset]<='9'); }
    if(j->offset<j->length && j->data[j->offset]=='.') { size_t digits=++j->offset; while(j->offset<j->length && j->data[j->offset]>='0'&&j->data[j->offset]<='9') ++j->offset; if(digits==j->offset) return bad(j); }
    if(j->offset<j->length && (j->data[j->offset]=='e'||j->data[j->offset]=='E')) { ++j->offset; if(j->offset<j->length && (j->data[j->offset]=='+'||j->data[j->offset]=='-')) ++j->offset; size_t digits=j->offset; while(j->offset<j->length && j->data[j->offset]>='0'&&j->data[j->offset]<='9') ++j->offset; if(digits==j->offset) return bad(j); }
    size_t n=j->offset-start; char text[64]; if(!n || n>=sizeof(text)) return bad(j); memcpy(text,j->data+start,n); text[n]=0;
    double value=strtod(text,NULL); if(!isfinite(value)) return bad(j); if(result) *result=value; return true;
}
bool wdg_json_uint(WdgJson* j,uint32_t* result) { double n; if(!wdg_json_number(j,&n) || n<0 || n>UINT32_MAX || n!=(uint32_t)n) return bad(j); *result=(uint32_t)n; return true; }
static bool literal(WdgJson* j,const char* s) { ws(j); size_t n=strlen(s); if(j->length-j->offset<n || memcmp(j->data+j->offset,s,n)) return bad(j); j->offset+=n; return true; }
bool wdg_json_null(WdgJson* j) { return literal(j,"null"); }
bool wdg_json_bool(WdgJson* j,bool* result) { ws(j); if(j->offset<j->length && j->data[j->offset]=='t') { *result=true; return literal(j,"true"); } *result=false; return literal(j,"false"); }
bool wdg_json_skip(WdgJson* j) {
    ws(j); if(j->offset==j->length) return bad(j); char c=j->data[j->offset];
    if(c=='"') return wdg_json_string(j,NULL,0);
    if(c=='n') return wdg_json_null(j);
    if(c=='t'||c=='f') { bool b; return wdg_json_bool(j,&b); }
    if(c=='{' || c=='[') {
        bool first=true; int more; if(!container(j,c)) return false;
        while((more=next(j,&first,c=='{'?'}':']'))==1) { if(c=='{' && (!wdg_json_string(j,NULL,0)||!take(j,':'))) return false; if(!wdg_json_skip(j)) return false; }
        return more==0;
    }
    return wdg_json_number(j,NULL);
}
bool wdg_json_finish(WdgJson* j) { ws(j); return !j->error && !j->depth && j->offset==j->length; }
bool wdg_map_bounds_valid(const WdgMapBounds* b) { return b && isfinite(b->south)&&isfinite(b->north)&&isfinite(b->west)&&isfinite(b->east)&&b->south>=-85.051129&&b->north<=85.051129&&b->west>=-180&&b->east<=180&&b->south<b->north&&b->west<b->east; }
static bool coordinate(WdgJson* j,int32_t* out,double max) { double n; if(!wdg_json_number(j,&n)) return false; if(n< -max||n>max) return bad_reason(j,"out of range"); *out=(int32_t)(n*1000000.0+(n<0?-0.5:0.5)); return true; }
static bool rgb(WdgJson* j,uint32_t* color) { char s[8]; if(!wdg_json_string(j,s,sizeof(s))) return false; if(strlen(s)!=7||s[0]!='#') return bad_reason(j,"#RRGGBB expected"); uint32_t n=0; for(unsigned i=1;i<7;++i) { int h=hex(s[i]); if(h<0) return bad(j); n=(n<<4)|(unsigned)h; } *color=n; return true; }
static bool once(WdgJson* j,unsigned* mask,unsigned bit) { if(*mask&bit) return bad_reason(j,"duplicate field"); *mask|=bit; return true; }
static bool nullable_id(WdgJson* j,uint32_t* id) { ws(j); if(j->offset<j->length && j->data[j->offset]=='n') { *id=0; return wdg_json_null(j); } return wdg_json_uint(j,id); }
static bool nullable_name(WdgJson* j,char* text,size_t cap) { ws(j); if(j->offset<j->length && j->data[j->offset]=='n') { text[0]=0; return wdg_json_null(j); } return wdg_json_string(j,text,cap); }
static WdgMapGang* gang_find(WdgMapSnapshot* m,uint32_t id) { for(unsigned i=0;i<m->gang_count;++i) if(m->gangs[i].id==id) return &m->gangs[i]; return NULL; }
static bool add_gang(WdgJson* j,WdgMapSnapshot* m,uint32_t id,const char* name,uint32_t color,bool has_color) {
    /* No gang is not one shared team: unaffiliated members may have different colors. */
    if(!id) return true;
    WdgMapGang* gang=gang_find(m,id);
    if(gang) { if(name[0] && gang->name[0] && strcmp(name,gang->name)) { at(j,"gangs[].name"); return bad_reason(j,"conflicting values"); } if(name[0]) strcpy(gang->name,name); if(has_color) { if(gang->color_rgb!=0xffffffffU && gang->color_rgb!=color) { at(j,"gangs[].color"); return bad_reason(j,"conflicting values"); } gang->color_rgb=color; } return true; }
    if(m->gang_count>=WDG_MAP_MAX_GANGS) { at(j,"gangs"); return bad_reason(j,"limit " WDG_STRINGIFY(WDG_MAP_MAX_GANGS) " reached"); }
    gang=&m->gangs[m->gang_count++]; gang->id=id; gang->color_rgb=has_color?color:0xffffffffU; strcpy(gang->name,name); return true;
}
static bool gang_referenced(const WdgMapSnapshot* m,uint32_t id) {
    if(!id) return false;
    for(unsigned i=0;i<m->cell_count;++i) if(m->cells[i].gang_id==id) return true;
    for(unsigned i=0;i<m->hull_count;++i) if(m->hulls[i].gang_id==id) return true;
    return false;
}
static int compare_ids(const void* a,const void* b) {
    uint32_t x=*(const uint32_t*)a,y=*(const uint32_t*)b;
    return x>y?1:x<y?-1:0;
}
static bool gangs_read(WdgJson* j,WdgMapSnapshot* m,uint32_t* ids,size_t capacity) {
    bool first=true; char id_text[16]; size_t id_count=0; int more;
    if(!at(j,"gangs")||!wdg_json_object(j)) return false;
    while((more=wdg_json_member(j,&first,id_text,sizeof(id_text)))==1) {
        uint64_t id=0; at(j,"gangs[].id"); if(!id_text[0]) return bad_reason(j,"integer key expected");
        for(const char* p=id_text;*p;++p) { if(*p<'0'||*p>'9') return bad_reason(j,"integer key expected"); id=id*10+(unsigned)(*p-'0'); if(id>UINT32_MAX) return bad_reason(j,"out of range"); }
        if(id_count==capacity) return bad_reason(j,"dictionary size changed");
        ids[id_count++]=(uint32_t)id;
        bool item=true; char key[64],name[WDG_MAP_NAME_SIZE]=""; unsigned seen=0; uint32_t color=0; int n;
        at(j,"gangs[]"); if(!wdg_json_object(j)) return false;
        while((n=wdg_json_member(j,&item,key,sizeof(key)))==1) {
            if(!strcmp(key,"name")||!strcmp(key,"gang")) { at(j,"gangs[].name"); if(!once(j,&seen,1)||!wdg_json_string(j,name,sizeof(name))) return false; }
            else if(!strcmp(key,"color")) { at(j,"gangs[].color"); if(!once(j,&seen,2)||!rgb(j,&color)) return false; }
            else { at(j,"gangs[].metadata"); if(!wdg_json_skip(j)) return false; }
        }
        if(n<0) return false;
        if(!(seen&2)) { at(j,"gangs[].color"); return bad_reason(j,"missing field"); }
        if(gang_referenced(m,(uint32_t)id)&&!add_gang(j,m,(uint32_t)id,name,color,true)) return false;
        at(j,"gangs[].id");
    }
    if(more!=0) return false;
    if(id_count>1) {
        qsort(ids,id_count,sizeof(*ids),compare_ids);
        for(size_t i=1;i<id_count;++i) if(ids[i]==ids[i-1]) { at(j,"gangs[].id"); return bad_reason(j,"duplicate ID"); }
    }
    return true;
}
static bool gangs(WdgJson* j,WdgMapSnapshot* m) {
    /* The API supplies a global dictionary even for a tiny viewport. Validate it
     * all, but retain only metadata referenced by returned geometry. Count first
     * to bound duplicate-ID scratch storage without a limit on unused teams. */
    WdgJson count=*j; bool first=true; size_t entries=0; int more;
    if(!at(&count,"gangs")||!wdg_json_object(&count)) { *j=count; return false; }
    while((more=wdg_json_member(&count,&first,NULL,0))==1) {
        ++entries; if(!wdg_json_skip(&count)) { *j=count; return false; }
    }
    if(more!=0) { *j=count; return false; }
    uint32_t* ids=NULL;
    if(entries) {
#ifdef ESP_PLATFORM
        ids=heap_caps_malloc(entries*sizeof(*ids),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
#else
        ids=malloc(entries*sizeof(*ids));
#endif
        if(!ids) { at(j,"gangs"); return bad_reason(j,"not enough memory"); }
    }
    bool valid=gangs_read(j,m,ids,entries); free(ids); return valid;
}
static bool cells(WdgJson* j,WdgMapSnapshot* m) {
    bool first=true; int more; if(!wdg_json_array(j)) return false;
    while((more=wdg_json_element(j,&first))==1) {
        if(m->cell_count==WDG_MAP_MAX_CELLS) { at(j,"cells"); return bad_reason(j,"limit " WDG_STRINGIFY(WDG_MAP_MAX_CELLS) " reached"); }
        WdgMapCell* c=&m->cells[m->cell_count++]; c->color_rgb=0xffffffffU;
        bool item=true; char key[64],name[65]=""; unsigned seen=0; int n; at(j,"cells[]"); if(!wdg_json_object(j)) return false;
        while((n=wdg_json_member(j,&item,key,sizeof(key)))==1) {
            if(!strcmp(key,"lat")) { at(j,"cells[].lat"); if(!once(j,&seen,1)||!coordinate(j,&c->lat_e6,90)) return false; }
            else if(!strcmp(key,"lng")) { at(j,"cells[].lng"); if(!once(j,&seen,2)||!coordinate(j,&c->lon_e6,180)) return false; }
            else if(!strcmp(key,"gang_id")) { at(j,"cells[].gang_id"); if(!once(j,&seen,4)||!nullable_id(j,&c->gang_id)) return false; }
            else if(!strcmp(key,"color")) { at(j,"cells[].color"); if(!once(j,&seen,8)||!rgb(j,&c->color_rgb)) return false; }
            else if(!strcmp(key,"count")) { at(j,"cells[].count"); if(!once(j,&seen,16)||!wdg_json_uint(j,&c->count)) return false; }
            else if(!strcmp(key,"gang")) { at(j,"cells[].gang"); if(!once(j,&seen,32)||!nullable_name(j,name,sizeof(name))) return false; }
            else if(!wdg_json_skip(j)) return false;
        }
        if(n<0) return false;
        if((seen&7)!=7) { at(j,!(seen&1)?"cells[].lat":!(seen&2)?"cells[].lng":"cells[].gang_id"); return bad_reason(j,"missing field"); }
        if((seen&40) && !add_gang(j,m,c->gang_id,name,c->color_rgb,(seen&8)!=0)) return false;
    }
    return more==0;
}
static bool hull_points(WdgJson* j,WdgMapSnapshot* m,WdgMapHull* h) {
    if(!wdg_json_array(j)) return false;
    bool points=true; int p;
    while((p=wdg_json_element(j,&points))==1) {
        if(m->point_count==WDG_MAP_MAX_POINTS) { at(j,"hull points"); return bad_reason(j,"limit " WDG_STRINGIFY(WDG_MAP_MAX_POINTS) " reached"); }
        at(j,"hull point"); if(!wdg_json_array(j)) return false;
        bool pair=true; WdgMapPoint* v=&m->points[m->point_count++];
        if(wdg_json_element(j,&pair)!=1 || !coordinate(j,&v->lat_e6,90) || wdg_json_element(j,&pair)!=1 || !coordinate(j,&v->lon_e6,180) || wdg_json_element(j,&pair)!=0) return bad(j);
        ++h->point_count;
    }
    if(p<0) return false;
    if(h->point_count<3) { at(j,"hull"); return bad_reason(j,"fewer than 3 points"); }
    return true;
}
static bool hulls(WdgJson* j,WdgMapSnapshot* m) {
    bool first=true; int more; if(!wdg_json_array(j)) return false;
    while((more=wdg_json_element(j,&first))==1) {
        if(m->hull_count==WDG_MAP_MAX_HULLS) { at(j,"gang_hulls"); return bad_reason(j,"limit " WDG_STRINGIFY(WDG_MAP_MAX_HULLS) " reached"); }
        WdgMapHull* h=&m->hulls[m->hull_count++]; h->color_rgb=0xffffffffU; h->first_point=m->point_count;
        bool item=true; char key[64],name[65]=""; unsigned seen=0; int n; at(j,"gang_hulls[]"); if(!wdg_json_object(j)) return false;
        while((n=wdg_json_member(j,&item,key,sizeof(key)))==1) {
            if(!strcmp(key,"gang_id")) { at(j,"gang_hulls[].gang_id"); if(!once(j,&seen,1)||!nullable_id(j,&h->gang_id)) return false; }
            else if(!strcmp(key,"color")) { at(j,"gang_hulls[].color"); if(!once(j,&seen,2)||!rgb(j,&h->color_rgb)) return false; }
            else if(!strcmp(key,"gang")) { at(j,"gang_hulls[].gang"); if(!once(j,&seen,4)||!nullable_name(j,name,sizeof(name))) return false; }
            else if(!strcmp(key,"hull")) {
                at(j,"gang_hulls[].hull");
                if(!once(j,&seen,8)||!hull_points(j,m,h)) return false;
            } else if(!wdg_json_skip(j)) return false;
        }
        if(n<0 || !(seen&8) || (!(seen&1)&&!(seen&2))) return bad(j);
        strcpy(h->name,name);
        if((seen&1) && (seen&6) && !add_gang(j,m,h->gang_id,name,h->color_rgb,(seen&2)!=0)) return false;
    }
    return more==0;
}
static bool bbox(WdgJson* j,WdgMapBounds* b) {
    if(!wdg_json_array(j)) return false;
    bool first=true; double* values[]={&b->south,&b->west,&b->north,&b->east};
    for(unsigned i=0;i<4;++i) if(wdg_json_element(j,&first)!=1||!wdg_json_number(j,values[i])) return false;
    if(wdg_json_element(j,&first)!=0) return bad_reason(j,"expected four coordinates");
    return wdg_map_bounds_valid(b)||bad_reason(j,"invalid bounds/order");
}
static void message(char* out,size_t n,const char* text) { if(out&&n) snprintf(out,n,"%s",text); }
bool wdg_map_parse_territories(const char* data,size_t length,WdgMapSnapshot* out,char* error,size_t cap) {
    message(error,cap,"Unsupported territory data; old map kept");
    if(!out || !data || !length) return false;
    if(length>WDG_MAP_RESPONSE_MAX) { if(error&&cap) snprintf(error,cap,"Territories: response exceeds %lu bytes",(unsigned long)WDG_MAP_RESPONSE_MAX); return false; }
#ifdef ESP_PLATFORM
    WdgMapSnapshot* m=heap_caps_calloc(1,sizeof(*m),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
#else
    WdgMapSnapshot* m=calloc(1,sizeof(*m));
#endif
    if(!m) { message(error,cap,"Not enough map memory"); return false; }
    WdgJson j; wdg_json_init(&j,data,length); bool first=true,ok=false; unsigned seen=0; int n=-1; char key[64]; size_t gangs_start=0; bool valid=wdg_json_object(&j);
    while(valid && (n=wdg_json_member(&j,&first,key,sizeof(key)))==1) {
        if(!strcmp(key,"ok")) valid=at(&j,"ok")&&once(&j,&seen,1)&&wdg_json_bool(&j,&ok)&&ok;
        else if(!strcmp(key,"grid_lat")) valid=at(&j,"grid_lat")&&once(&j,&seen,2)&&coordinate(&j,&m->grid_lat_e6,10)&&m->grid_lat_e6>0;
        else if(!strcmp(key,"grid_lng")) valid=at(&j,"grid_lng")&&once(&j,&seen,4)&&coordinate(&j,&m->grid_lon_e6,10)&&m->grid_lon_e6>0;
        else if(!strcmp(key,"cells")) valid=at(&j,"cells")&&once(&j,&seen,8)&&cells(&j,m);
        else if(!strcmp(key,"gang_hulls")) valid=at(&j,"gang_hulls")&&once(&j,&seen,16)&&hulls(&j,m);
        else if(!strcmp(key,"gangs")) { gangs_start=j.offset; valid=at(&j,"gangs")&&once(&j,&seen,32)&&wdg_json_skip(&j); }
        else if(!strcmp(key,"bbox")) valid=at(&j,"bbox")&&once(&j,&seen,64)&&bbox(&j,&m->bounds);
        else if(!strcmp(key,"zoom_skipped_cells")) valid=at(&j,"zoom_skipped_cells")&&once(&j,&seen,128)&&wdg_json_bool(&j,&m->cells_skipped);
        else valid=at(&j,"metadata")&&wdg_json_skip(&j);
    }
    if(valid && (n!=0||!wdg_json_finish(&j))) valid=bad_reason(&j,"invalid JSON ending");
    if(valid && (seen&9)!=9) { at(&j,!(seen&1)?"ok":"cells"); valid=bad_reason(&j,"missing field"); }
    if(valid && m->cell_count && (seen&6)!=6) { at(&j,!(seen&2)?"grid_lat":"grid_lng"); valid=bad_reason(&j,"missing field"); }
    if(valid && m->cells_skipped && m->cell_count) { at(&j,"zoom_skipped_cells"); valid=bad_reason(&j,"conflicts with cells"); }
    if(valid && (seen&32)) {
        /* Revisit the original buffer so diagnostic offsets remain absolute. */
        WdgJson metadata; wdg_json_init(&metadata,data,length); metadata.offset=gangs_start;
        valid=gangs(&metadata,m); if(!valid) j=metadata;
    }
    for(unsigned i=0;valid&&i<m->cell_count;++i) {
        WdgMapCell* c=&m->cells[i]; WdgMapGang* g=gang_find(m,c->gang_id);
        if(c->color_rgb==0xffffffffU && g) c->color_rgb=g->color_rgb;
        if(c->color_rgb==0xffffffffU) { at(&j,"cells[].color"); valid=bad_reason(&j,"missing ownership color"); }
    }
    for(unsigned i=0;valid&&i<m->hull_count;++i) {
        WdgMapHull* h=&m->hulls[i]; WdgMapGang* g=gang_find(m,h->gang_id);
        if(h->color_rgb==0xffffffffU && g) h->color_rgb=g->color_rgb;
        if(h->color_rgb==0xffffffffU) { at(&j,"gang_hulls[].color"); valid=bad_reason(&j,"missing ownership color"); }
    }
    if(valid) { *out=*m; message(error,cap,""); }
    else {
        if(!j.error) bad_reason(&j,"invalid value");
        if(error&&cap) snprintf(error,cap,"Territories @B%lu: %s %s",
            (unsigned long)j.error_offset,j.error_field?j.error_field:"response",j.reason?j.reason:"invalid JSON");
    }
    free(m); return valid;
}
bool wdg_map_parse_overview(const char* data,size_t length,WdgMapSnapshot* out,char* error,size_t cap) {
    message(error,cap,"Invalid overview response");
    if(!out||!data||!length) return false;
    if(length>WDG_MAP_RESPONSE_MAX) { if(error&&cap) snprintf(error,cap,"Territories: response exceeds %lu bytes",(unsigned long)WDG_MAP_RESPONSE_MAX); return false; }
#ifdef ESP_PLATFORM
    WdgMapSnapshot* m=heap_caps_calloc(1,sizeof(*m),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
#else
    WdgMapSnapshot* m=calloc(1,sizeof(*m));
#endif
    if(!m) { message(error,cap,"Not enough map memory"); return false; }
    m->cells_skipped=true;
    m->bounds=(WdgMapBounds){-85.051128,-180,85.051128,180};
    WdgJson j; wdg_json_init(&j,data,length); bool first=true; int more=-1;
    bool valid=at(&j,"overview")&&wdg_json_array(&j);
    while(valid&&(more=wdg_json_element(&j,&first))==1) {
        if(m->hull_count==WDG_MAP_MAX_HULLS) { at(&j,"overview hulls"); valid=bad_reason(&j,"limit " WDG_STRINGIFY(WDG_MAP_MAX_HULLS) " reached"); break; }
        WdgMapHull* h=&m->hulls[m->hull_count++]; h->first_point=m->point_count;
        bool item=true; unsigned seen=0; int n=-1; char key[64];
        valid=at(&j,"overview[]")&&wdg_json_object(&j);
        while(valid&&(n=wdg_json_member(&j,&item,key,sizeof(key)))==1) {
            if(!strcmp(key,"gang_id")) valid=at(&j,"overview[].gang_id")&&once(&j,&seen,1)&&wdg_json_uint(&j,&h->gang_id);
            else if(!strcmp(key,"name")) valid=at(&j,"overview[].name")&&once(&j,&seen,2)&&wdg_json_string(&j,h->name,sizeof(h->name));
            else if(!strcmp(key,"color")) valid=at(&j,"overview[].color")&&once(&j,&seen,4)&&rgb(&j,&h->color_rgb);
            else if(!strcmp(key,"hull")) valid=at(&j,"overview[].hull")&&once(&j,&seen,8)&&hull_points(&j,m,h);
            else if(!strcmp(key,"points")||!strcmp(key,"members")||!strcmp(key,"rank")) {
                unsigned bit=!strcmp(key,"points")?16:!strcmp(key,"members")?32:64;
                uint32_t count; valid=at(&j,"overview[].count")&&once(&j,&seen,bit)&&wdg_json_uint(&j,&count);
            } else valid=at(&j,"overview[].metadata")&&wdg_json_skip(&j);
        }
        if(valid&&n!=0) valid=bad_reason(&j,"invalid object ending");
        if(valid&&(seen&15)!=15) {
            at(&j,!(seen&1)?"overview[].gang_id":!(seen&2)?"overview[].name":!(seen&4)?"overview[].color":"overview[].hull");
            valid=bad_reason(&j,"missing field");
        }
        if(valid) valid=add_gang(&j,m,h->gang_id,h->name,h->color_rgb,true);
    }
    if(valid&&(more!=0||!wdg_json_finish(&j))) valid=bad_reason(&j,"invalid JSON ending");
    if(valid) { *out=*m; message(error,cap,""); }
    else {
        if(!j.error) bad_reason(&j,"invalid value");
        if(error&&cap) snprintf(error,cap,"Territories @B%lu: %s %s",(unsigned long)j.error_offset,
            j.error_field?j.error_field:"overview",j.reason?j.reason:"invalid JSON");
    }
    free(m); return valid;
}
bool wdg_map_parse_account(const char* data,size_t length,WdgMapAccount* out,char* error,size_t cap) {
    message(error,cap,"Unsupported account response"); if(!out||!data||!length||length>32768) return false;
    WdgMapAccount a={0}; WdgJson j; wdg_json_init(&j,data,length); bool first=true,ok=false; int n; unsigned seen=0; char key[64]; if(!wdg_json_object(&j)) return false;
    while((n=wdg_json_member(&j,&first,key,sizeof(key)))==1) {
        unsigned bit=0; uint32_t* value=NULL;
        if(!strcmp(key,"ok")) { if(!once(&j,&seen,1)||!wdg_json_bool(&j,&ok)) return false; }
        else if(!strcmp(key,"username")) { if(!once(&j,&seen,2)||!wdg_json_string(&j,a.username,sizeof(a.username))) return false; }
        else if(!strcmp(key,"gang")) { if(!once(&j,&seen,4)||!nullable_name(&j,a.gang,sizeof(a.gang))) return false; }
        else {
            if(!strcmp(key,"gang_id")) { bit=8; value=&a.gang_id; }
            else if(!strcmp(key,"wifi")) { bit=16; value=&a.wifi; }
            else if(!strcmp(key,"ble")) { bit=32; value=&a.ble; }
            else if(!strcmp(key,"aircraft")) { bit=64; value=&a.aircraft; }
            else if(!strcmp(key,"mesh")) { bit=128; value=&a.mesh; }
            else if(!strcmp(key,"total")) { bit=256; value=&a.total; }
            if(value) { if(!once(&j,&seen,bit)||!(bit==8?nullable_id(&j,value):wdg_json_uint(&j,value))) return false; }
            else if(!wdg_json_skip(&j)) return false;
        }
    }
    if(n<0||!wdg_json_finish(&j)||!ok||(seen&3)!=3||!a.username[0]) return false;
    *out=a; message(error,cap,""); return true;
}
