#include "wardriver_oui.h"
#include <furi.h>
#include <storage/storage.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <stdlib.h>
/* Sor3nt/Momentum wlan_oui text format; streaming parser, fixed memory cap. */
typedef struct { uint32_t prefix; char name[29]; } OuiEntry;
struct WardriverOui {
    OuiEntry* entries;
    size_t count,capacity,length,total;
    Storage* storage;
    File* file;
    char line[160];
    bool overflow,complete,failed;
};
static int hex_digit(char c) {
    if(c>='0'&&c<='9') return c-'0';
    if(c>='A'&&c<='F') return c-'A'+10;
    if(c>='a'&&c<='f') return c-'a'+10;
    return -1;
}
static void parse(WardriverOui* t,char* line) {
    if(t->count==t->capacity || *line=='#') return;
    uint32_t prefix=0; unsigned digits=0;
    while(*line && digits<6) {
        int h=hex_digit(*line++);
        if(h>=0) { prefix=(prefix<<4)|(unsigned)h; ++digits; }
        else if(line[-1]!=':'&&line[-1]!='-'&&line[-1]!=' '&&line[-1]!='\t') return;
    }
    if(digits!=6) return;
    if(*line!=','&&*line!=';'&&*line!=' '&&*line!='\t') return;
    while(*line==','||*line==';'||*line==' '||*line=='\t') ++line;
    if(!*line) return;
    OuiEntry* e=&t->entries[t->count++]; e->prefix=prefix;
    size_t n=0;
    while(*line && *line!=',' && *line!='\t' && n<28) e->name[n++]=*line++;
    while(n && e->name[n-1]==' ') --n;
    e->name[n]=0;
}
static int compare(const void* a,const void* b) {
    uint32_t x=((const OuiEntry*)a)->prefix,y=((const OuiEntry*)b)->prefix;
    return (x>y)-(x<y);
}
WardriverOui* wardriver_oui_open(void) {
    Storage* s=furi_record_open(RECORD_STORAGE);
    File* f=storage_file_alloc(s); WardriverOui* t=NULL;
    if(!f || !storage_file_open(f,"/ext/apps_data/wifi/mac-vendor.txt",FSAM_READ,FSOM_OPEN_EXISTING)) goto done;
    t=calloc(1,sizeof(*t)); if(!t) goto done;
    for(size_t n=65536;n>=2048;n/=2) {
        t->entries=heap_caps_malloc(n*sizeof(OuiEntry),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(t->entries) { t->capacity=n; break; }
    }
    if(!t->entries) { free(t); t=NULL; goto done; }
    t->storage=s; t->file=f;
    return t;
done:
    if(f) { storage_file_close(f); storage_file_free(f); }
    furi_record_close(RECORD_STORAGE); return t;
}
bool wardriver_oui_step(WardriverOui* t) {
    if(!t || t->complete) return true;
    char buffer[4096];
    size_t got=storage_file_read(t->file,buffer,sizeof(buffer));
    if(storage_file_get_error(t->file)!=FSE_OK) { t->failed=true; got=0; }
    t->total+=got;
    for(size_t i=0;i<got;++i) {
        char c=buffer[i];
        if(c=='\n') {
            t->line[t->length]=0;
            if(!t->overflow) parse(t,t->line);
            t->length=0; t->overflow=false;
        } else if(c!='\r') {
            if(t->length<sizeof(t->line)-1) t->line[t->length++]=c;
            else t->overflow=true;
        }
    }
    if(got && t->total<8U*1024*1024 && t->count<t->capacity) return false;
    if(t->length && !t->overflow) { t->line[t->length]=0; parse(t,t->line); }
    qsort(t->entries,t->count,sizeof(OuiEntry),compare);
    storage_file_close(t->file); storage_file_free(t->file); t->file=NULL;
    furi_record_close(RECORD_STORAGE); t->storage=NULL; t->complete=true;
    return true;
}
bool wardriver_oui_ready(const WardriverOui* t) { return t && t->complete && !t->failed && t->count; }
void wardriver_oui_lookup(const WardriverOui* t,const uint8_t mac[6],char out[29]) {
    out[0]=0;
    if(mac[0]&2) { strcpy(out,"Local / randomized MAC"); return; }
    if(!wardriver_oui_ready(t)) return;
    uint32_t prefix=((uint32_t)mac[0]<<16)|((uint32_t)mac[1]<<8)|mac[2];
    size_t lo=0,hi=t->count;
    while(lo<hi) {
        size_t mid=lo+(hi-lo)/2;
        if(t->entries[mid].prefix<prefix) lo=mid+1; else hi=mid;
    }
    if(lo<t->count && t->entries[lo].prefix==prefix) memcpy(out,t->entries[lo].name,29);
}
void wardriver_oui_free(WardriverOui* t) {
    if(t) {
        if(t->file) { storage_file_close(t->file); storage_file_free(t->file); }
        if(t->storage) furi_record_close(RECORD_STORAGE);
        free(t->entries); free(t);
    }
}
