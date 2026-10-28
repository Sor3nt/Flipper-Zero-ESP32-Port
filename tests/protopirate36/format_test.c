#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { char text[1024]; size_t pos; } Stream;
typedef Stream FlipperFormat;
typedef struct { char text[1024]; } FuriString;
#define StreamOffsetFromStart 0
static Stream* flipper_format_get_raw_stream(FlipperFormat* ff) { return ff; }
static FuriString* furi_string_alloc(void) { return calloc(1,sizeof(FuriString)); }
static const char* furi_string_get_cstr(FuriString* s) { return s->text; }
static void furi_string_free(FuriString* s) { free(s); }
static size_t stream_tell(Stream* s) { return s->pos; }
static bool stream_seek(Stream* s,size_t offset,int origin) { if(offset>strlen(s->text))return false;s->pos=offset;return true; }
static bool stream_read_line(Stream* s,FuriString* line) {
    size_t n=0; while(s->text[s->pos]) { char c=s->text[s->pos++];if(c!='\r')line->text[n++]=c;if(c=='\n')break; }
    line->text[n]=0;return n>0;
}
static bool stream_delete(Stream* s,size_t n) { size_t size=strlen(s->text);assert(s->pos+n<=size);memmove(s->text+s->pos,s->text+s->pos+n,size-s->pos-n+1);return true; }
#include "format.inc"
int main(void) {
    Stream s={.text="Header: 1\r\n# Raw: keep\r\nRawExtra: keep\r\nRaw: 00 FF\r\nSerial: 7\r\n"};
    assert(pp_flipper_delete_key(&s,"Raw"));
    assert(strcmp(s.text,"Header: 1\r\n# Raw: keep\r\nRawExtra: keep\r\nSerial: 7\r\n")==0);
    s.pos=0; assert(!pp_flipper_delete_key(&s,"Absent"));
    strcpy(s.text,"Raw: first\nRaw: second");s.pos=0;
    assert(pp_flipper_delete_key(&s,"Raw"));assert(strcmp(s.text,"Raw: second")==0);
    assert(pp_flipper_delete_key(&s,"Raw"));assert(s.text[0]==0);
    assert(!pp_flipper_delete_key(&s,"Raw"));
    assert(!pp_flipper_delete_key(NULL,"Raw"));
    puts("PASS format compatibility: exact key, comments/prefixes preserved, CRLF, duplicate keys and EOF without newline");
}
