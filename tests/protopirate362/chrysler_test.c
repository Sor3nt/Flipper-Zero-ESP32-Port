#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#define furi_check(x) assert(x)
#define UNUSED(x) (void)(x)
#define FURI_LOG_D(...)
#define PP_SHARED_UPLOAD_CAPACITY 512
#define DURATION_DIFF(x,y) ((x)>(y)?(x)-(y):(y)-(x))
typedef struct {const char* name;} SubGhzProtocol;
typedef struct SubGhzProtocolDecoderBase SubGhzProtocolDecoderBase;
struct SubGhzProtocolDecoderBase {const SubGhzProtocol* protocol;void (*callback)(SubGhzProtocolDecoderBase*,void*);void* context;};
typedef struct SubGhzProtocolDecoderChrysler_V0 SubGhzProtocolDecoderChrysler_V0;
typedef void SubGhzEnvironment;
static const SubGhzProtocol chrysler_protocol_v0={"Chrysler V0"};
typedef struct {char text[2048];size_t size;} FuriString;
static void furi_string_cat_printf(FuriString* s,const char* fmt,...) {
 va_list args;va_start(args,fmt);int n=vsnprintf(s->text+s->size,sizeof(s->text)-s->size,fmt,args);va_end(args);
 assert(n>=0 && (size_t)n<sizeof(s->text)-s->size);s->size+=n;
}
static void pp_u64_to_bytes_be(uint64_t data,uint8_t key[8]) {for(int i=7;i>=0;--i){key[i]=data;data>>=8;}}
#include "chrysler.inc"
static unsigned received;
static void decoded(SubGhzProtocolDecoderBase* base,void* ctx) {
 SubGhzProtocolDecoderChrysler_V0* d=(void*)base;
 assert(d->packet_bit_count==80 && d->generic.data_count_bit==80);
 assert(d->check_ok && d->decoded_button==2);
 FuriString text={0};subghz_protocol_decoder_chrysler_v0_get_string(d,&text);
 assert(strstr(text.text,"Chrysler V0 80bit") && strstr(text.text,"Unlock") && strstr(text.text,"Chk:OK"));
 assert(text.size<1024);received++;
}
static void receive_frame(SubGhzProtocolDecoderChrysler_V0* d,const uint8_t bytes[10]) {
 for(unsigned i=0;i<24;i++) {subghz_protocol_decoder_chrysler_v0_feed(d,true,300);subghz_protocol_decoder_chrysler_v0_feed(d,false,3700);}
 subghz_protocol_decoder_chrysler_v0_feed(d,true,300);subghz_protocol_decoder_chrysler_v0_feed(d,false,15600);
 for(unsigned i=0;i<80;i++) {
  bool bit=(bytes[i/8]>>(7-i%8))&1;
  subghz_protocol_decoder_chrysler_v0_feed(d,true,bit?600:300);
  subghz_protocol_decoder_chrysler_v0_feed(d,false,bit?3400:3700);
 }
}
int main(void) {
 SubGhzProtocolDecoderChrysler_V0* d=subghz_protocol_decoder_chrysler_v0_alloc(NULL);d->base.callback=decoded;
 uint8_t bytes[10]={0x80,0x11,0x22,0x33,0x44,0x11,0x73,0x55,0x66,0x54};
 for(unsigned i=0;i<1000;i++) {bytes[2]=i;receive_frame(d,bytes);assert(received==i+1);}
 subghz_protocol_decoder_chrysler_v0_reset(d);
 bytes[5]^=1;receive_frame(d,bytes);assert(received==1000);
 bytes[5]^=1;receive_frame(d,bytes);assert(received==1001);
 free(d);puts("PASS production Chrysler RX decoder: 1000 consecutive synthetic unlock frames, text formatting, reset and invalid-frame rejection");
}
