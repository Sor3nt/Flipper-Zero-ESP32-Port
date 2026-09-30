#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ENABLE_MODELS_DATABASE
#define APP_ASSETS_PATH(x) x
#define FURI_LOG_E(...)
#define FURI_LOG_W(...)
#define FURI_LOG_D(...)
#define FURI_LOG_I(...)
#define RECORD_STORAGE "storage"
typedef struct { char* text; } FuriString;
typedef int Storage;
typedef int FlipperFormat;
typedef int SubGhzSetting;
typedef struct { FuriString* name; uint32_t frequency; uint8_t* data; size_t data_size; } SubGhzRadioPreset;
typedef struct { uint16_t index; char* name; SubGhzRadioPreset* preset; int16_t last_preset_index; } ProtoPirateCarModel;
static int mode, records, open_files, strings, sentinel;
static uint8_t preset[]={0x02,0x0d,0,0,0xc0,0,0,0,0,0,0,0};
static const char* model_name="Model %n \xc3\xa9";
static FuriString* furi_string_alloc(void) { FuriString* s=malloc(sizeof(*s));s->text=strdup("");strings++;return s; }
static void furi_string_set_str(FuriString* s,const char* text) { char* copy=strdup(text);free(s->text);s->text=copy; }
static const char* furi_string_get_cstr(FuriString* s) { return s->text; }
static int furi_string_cmp_str(FuriString* s,const char* t) { return strcmp(s->text,t); }
static FuriString* furi_string_alloc_set(FuriString* s) { FuriString* copy=furi_string_alloc();furi_string_set_str(copy,s->text);return copy; }
static void furi_string_free(FuriString* s) { free(s->text);free(s);strings--; }
static Storage* furi_record_open(const char* name) { records++;return &sentinel; }
static void furi_record_close(const char* name) { records--; }
static FlipperFormat* flipper_format_file_alloc(Storage* s) { open_files++;return &sentinel; }
static void flipper_format_free(FlipperFormat* f) { open_files--; }
static bool flipper_format_file_open_existing(FlipperFormat* f,const char* p) { return mode!=1; }
static bool flipper_format_read_header(FlipperFormat* f,FuriString* h,uint32_t* v) { furi_string_set_str(h,"ProtoPirate Car Models Database");*v=mode==2?99:2;return true; }
static bool flipper_format_read_string(FlipperFormat* f,const char* key,FuriString* out) {
    if(strncmp(key,"ModelName",9)==0) { if(mode==3)return false; furi_string_set_str(out,model_name); }
    else furi_string_set_str(out,mode==9?"UnknownPreset":mode>=6?"FuriHalSubGhzPresetCustom":"AM650");
    return true;
}
static bool flipper_format_read_uint32(FlipperFormat* f,const char* key,uint32_t* out,size_t n) { *out=strcmp(key,"ModelCount")==0?(mode==5?100000:11):(mode==4?1:433920000);return true; }
static bool flipper_format_get_value_count(FlipperFormat* f,const char* key,uint32_t* n) { *n=sizeof(preset);return true; }
static bool flipper_format_read_hex(FlipperFormat* f,const char* key,uint8_t* out,size_t n) { memcpy(out,preset,n);if(mode==7)memset(out,1,n);return mode!=8; }
static void flipper_format_rewind(FlipperFormat* f) {}
static size_t subghz_setting_get_preset_count(SubGhzSetting* s) { return 1; }
static const char* subghz_setting_get_preset_name(SubGhzSetting* s,size_t i) { assert(i==0);return "AM650"; }
static uint8_t* subghz_setting_get_preset_data(SubGhzSetting* s,size_t i) { assert(i==0);return preset; }
static size_t subghz_setting_get_preset_data_size(SubGhzSetting* s,size_t i) { return sizeof(preset); }
#include "models.inc"
int main(void) {
    ProtoPirateCarModel m={0};
    for(int repeat=0;repeat<100;repeat++) {
        mode=0;assert(car_model_get_count()==11);
        assert(car_model_get_by_index(&m,1,11,&sentinel));
        assert(m.index==1 && strcmp(m.name,model_name)==0 && m.preset->data!=preset);
        assert(m.preset->data_size==sizeof(preset));
        for(mode=1;mode<=4;mode++) { assert(!car_model_get_by_index(&m,1,11,&sentinel));assert(!m.index&&!m.preset); }
        mode=5;assert(car_model_get_count()==0);
        mode=6;assert(car_model_get_by_index(&m,1,11,&sentinel));
        mode=7;assert(!car_model_get_by_index(&m,1,11,&sentinel));
        mode=8;assert(!car_model_get_by_index(&m,1,11,&sentinel));
        mode=9;assert(car_model_get_by_index(&m,1,11,&sentinel));
        assert(strcmp(m.preset->name->text,"AM650")==0);
        mode=0;assert(!car_model_get_by_index(&m,65535,11,&sentinel));
        assert(!m.index&&!m.preset&&!records&&!open_files&&!strings);
    }
    free(m.name);
    puts("PASS models: UTF-8/percent labels, absent/corrupt files, invalid indices/frequencies/counts, truncated custom presets and repeated selection/reset");
}
