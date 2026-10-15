#include "rtl_engine.h"
#include "rtl_capture.h"
#include "rtl_file.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static char *last;static unsigned messages;
void test_fields(void);
void test_tpms(RtlEngine *, RtlFrame *);
void save_test(const char *, const char *);
void test_schedule(void);
void test_doorbell(void);
void test_catalog(void);
static void output(void *ctx,const r_device *d,const data_t *data,const RtlMeta *m){(void)ctx;free(last);last=rtl_json(d,data,m);assert(last);messages++;}
static void only(RtlEngine *e,const char *needle){bool found=false;for(unsigned i=0;i<e->count;i++){e->enabled[i]=strstr(e->devices[i]->name,needle)!=NULL;found|=e->enabled[i];}assert(found);}
int main(void){RtlEngine e;assert(rtl_engine_init(&e,output,NULL,NULL));assert(e.count==377);
    RtlFrame *f=calloc(1,sizeof(*f)),*copy=calloc(1,sizeof(*copy));RtlCapture c;rtl_capture_reset(&c,f);
    /* Independent Prologue PPM fixture: ID 0x12, channel 1, 23.4C, 56% RH. */
    unsigned char b[]={0x91,0x28,0x0e,0xa3,0x80};
    for(unsigned rep=0;rep<7;rep++){for(unsigned bit=0;bit<36;bit++){assert(!rtl_capture_feed(&c,f,true,500));assert(!rtl_capture_feed(&c,f,false,(b[bit/8]&(0x80>>(bit%8)))?4000:2000));}rtl_capture_feed(&c,f,true,500);rtl_capture_feed(&c,f,false,9000);}
    assert(rtl_capture_finish(&c,f,100000));f->meta=(RtlMeta){433920000,123,-55,false,true};only(&e,"Prologue");assert(rtl_engine_decode(&e,&f->pulses,&f->meta)==1);assert(strstr(last,"Prologue-TH"));assert(strstr(last,"\"temperature_C\":23.4"));assert(strstr(last,"\"humidity\":56"));puts(last);
    char *s=rtl_pulse_text(f);assert(s&&rtl_pulse_parse(s,copy));assert(copy->pulses.num_pulses==f->pulses.num_pulses);assert(!memcmp(f->pulses.pulse,copy->pulses.pulse,sizeof(f->pulses.pulse)));free(s);
    assert(!rtl_pulse_parse("RTL433-PULSE 1\n433920000 0 -50 1\n-1 50\n",copy));assert(!rtl_pulse_parse("RTL433-PULSE 1\n433920000 0 -50 1\n999999999999 1\n",copy));assert(!rtl_pulse_parse("RTL433-PULSE 1\n433920000 0 -50 1\n10 20\nEXTRA",copy));
    /* Published WH31E fixture in upstream protocol documentation, no generated CRC. */
    const unsigned char wh[]={0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0x2d,0xd4,0x30,0xc3,0x81,0xd5,0x5c,0x2a,0xcf,0x08,0x35,0x44,0x2c};
    rtl_capture_reset(&c,f);bool level=true;unsigned duration=0;
    for(unsigned bit=0;bit<sizeof(wh)*8;bit++){bool next=(wh[bit/8]&(0x80>>(bit%8)))!=0;if(next!=level){if(duration)rtl_capture_feed(&c,f,level,duration);duration=0;level=next;}duration+=56;}rtl_capture_feed(&c,f,level,duration);assert(rtl_capture_finish(&c,f,100000));f->meta=(RtlMeta){915000000,456,-60,true,true};only(&e,"Ambient Weather WH31E");assert(rtl_engine_decode(&e,&f->pulses,&f->meta)==1);assert(strstr(last,"AmbientWeather-WH31E"));assert(strstr(last,"\"id\":195"));assert(strstr(last,"\"humidity\":92"));puts(last);
    test_catalog(); test_schedule(); test_doorbell(); test_fields(); test_tpms(&e,f); save_test("toyota.json",last);
    char *pretty=rtl_pretty_json(last);assert(pretty);save_test("toyota-pretty.json",pretty);free(pretty);puts(last);
    /* Dropped/oversized bursts cannot be decoded as complete captures. */
    rtl_capture_reset(&c,f);for(unsigned i=0;i<PD_MAX_PULSES+1;i++){rtl_capture_feed(&c,f,true,200);rtl_capture_feed(&c,f,false,200);}assert(c.invalid);assert(!rtl_capture_finish(&c,f,100000));
    /* All default-enabled decoders see deterministic noisy bursts under sanitizers. */
    for(unsigned i=0;i<e.count;i++)e.enabled[i]=rtl_catalog_get(i)->disabled==0;
    uint32_t rng=0x77c0ffee;for(unsigned run=0;run<30;run++){memset(f,0,sizeof(*f));f->pulses.sample_rate=1000000;f->pulses.num_pulses=32+run*17;for(unsigned i=0;i<f->pulses.num_pulses;i++){rng=rng*1664525+1013904223;f->pulses.pulse[i]=50+(rng%4000);rng=rng*1664525+1013904223;f->pulses.gap[i]=50+(rng%9000);}f->pulses.gap[f->pulses.num_pulses-1]=100000;f->meta.fsk=run%2;rtl_engine_decode(&e,&f->pulses,&f->meta);}
    free(last);free(copy);free(f);rtl_engine_free(&e);puts("PASS: OOK + FSK end-to-end, pulse serialization/rejection, overflow, noisy inputs");return 0;}
