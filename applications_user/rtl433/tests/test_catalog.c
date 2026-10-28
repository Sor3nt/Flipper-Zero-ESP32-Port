#include "rtl_engine.h"
#include "lib/rtl433/logger.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
static unsigned wind;

static void discard(void *ctx, const r_device *d, const data_t *data, const RtlMeta *m) {
  (void)ctx; char *json=rtl_json(d,data,m); assert(json);
  assert(strstr(json,"\"identification_status\":\"model_candidate\"") ||
         strstr(json,"\"identification_status\":\"encoding_family\""));
  assert(strstr(json,"\"validation_reported\":"));
  if(strstr(json,"TFA-303307")) {
    assert(strstr(json,"0b0102030405"));
    assert(rtl_field(data,"wind_dir_deg")->value.v_dbl==90);
    assert(fabs(rtl_field(data,"wind_avg_m_s")->value.v_dbl-1.2)<1e-5);
    assert(fabs(rtl_field(data,"wind_max_m_s")->value.v_dbl-3.4)<1e-5);wind++;
  }
  free(json);
}
static void quiet(log_level_t l, const char *s, const char *m, void *u) {
  (void)l;(void)s;(void)m;(void)u;
}
static void rzi_wind(RtlEngine *e,pulse_data_t *p) {
  /* Independent synthetic protocol fixture, not an RF recording. Construct
   * a CRC-protected WeatherHub frame, invert its descrambler/differential
   * coding, then express the raw bits using the documented RZI run lengths. */
  unsigned char data[26]={0x4b,0x2d,0xd4,0x2b,18,0x0b,1,2,3,4,5,
                         0,0,1,0x40,0x0c,0x22,0x09};
  uint32_t crc=0xe7720ae4;
  for(unsigned i=4;i<18;i++) {
    crc^=(uint32_t)data[i]<<24;
    for(unsigned j=0;j<8;j++)crc=(crc<<1)^((crc&0x80000000u)?0x04c11db7u:0);
  }
  for(unsigned i=0;i<4;i++)data[18+i]=crc>>(24-8*i);
  memset(p,0,sizeof(*p));p->sample_rate=1000000;
  unsigned prev_nrz=0,prev_psk=0,prev_raw=0,ones=0;uint32_t lfsr=0;
  for(unsigned i=0;i<sizeof(data)*8;i++) {
    unsigned bit=(data[i/8]>>(i%8))&1;
    unsigned nrz=bit^((lfsr>>16)&1)^((lfsr>>11)&1);
    unsigned psk=nrz!=prev_nrz?prev_psk:1-prev_psk;
    unsigned raw=psk!=prev_psk?prev_raw:1-prev_raw;
    prev_nrz=nrz;prev_psk=psk;prev_raw=raw;lfsr=(lfsr<<1)|nrz;
    if(raw)ones++;
    else {
      p->pulse[p->num_pulses]=ones?ones*167:1;
      p->gap[p->num_pulses++]=30;ones=0;
    }
  }
  p->pulse[p->num_pulses]=(ones+1)*167;p->gap[p->num_pulses++]=100000;
  for(unsigned i=0;i<e->count;i++)e->enabled[i]=strstr(e->devices[i]->name,"TFA 30.3307.02")!=NULL;
  RtlMeta meta={.frequency=868350000};
  assert(rtl_engine_decode(e,p,&meta)==1 && wind==1);
  puts("PASS: RZI dispatcher/slicer plus actual TFA wind decoder, synthetic CRC fixture: ID 0b0102030405, 90 degrees, speed 1.2 m/s, gust 3.4 m/s");
}
void test_catalog(void) {
  RtlEngine e; assert(rtl_engine_init(&e,discard,NULL,NULL));
  pulse_data_t *p=calloc(1,sizeof(*p)); assert(p);
  char path[1024]; snprintf(path,sizeof path,"%s/catalog-audit.csv",getenv("RTL_TEST_OUT"));
  FILE *csv=fopen(path,"w"); assert(csv);
  fputs("catalog_id,name,modulation,default_enabled,runnable,synthetic_smoke_cases\n",csv);
  unsigned runnable=0, defaults=0, mods=0, cases=0;
  r_logger_set_log_handler(quiet,NULL);
  memset(e.enabled,0,e.count*sizeof(*e.enabled));
  for(unsigned i=0;i<e.count;i++) {
    r_device *d=e.devices[i]; bool ok=rtl_decoder_runnable(d);
    data_t *candidate=data_make("model","",DATA_STRING,"unverified",NULL);
    assert(candidate && !rtl_encoding_family(d,candidate));
    assert(!strcmp(rtl_evidence(d,candidate),"CANDIDATE"));
    RtlMeta audit_meta={0};char *audit_json=rtl_json(d,candidate,&audit_meta);
    assert(audit_json && strstr(audit_json,"\"identification_status\":\"model_candidate\""));
    free(audit_json);data_free(candidate);
    if(d->disabled!=3) assert(ok); /* no silently unhandled catalog coding */
    const char *mod=rtl_modulation_name(d->modulation);
    unsigned tested=0;
    if(ok) {
      runnable++; defaults+=rtl_catalog_get(i)->disabled==0; mods |= 1u<<d->modulation;
      e.enabled[i]=true;
      unsigned lengths[]={1,12,128,PD_MAX_PULSES};
      for(unsigned k=0;k<4;k++) {
        memset(p,0,sizeof(*p)); p->sample_rate=1000000;p->num_pulses=lengths[k];
        unsigned short_us=d->short_width>=1?d->short_width:100;
        unsigned long_us=d->long_width>=1?d->long_width:short_us*2;
        for(unsigned n=0;n<p->num_pulses;n++) {
          p->pulse[n]=(n&1)?short_us:long_us;
          p->gap[n]=(n%3)?short_us:long_us;
          if(k==2&&n%31==30)p->gap[n]=100000;
        }
        /* Keep a nonterminal final symbol to cover lookahead/window edges. */
        RtlMeta m={.frequency=433920000,.fsk=d->modulation>=FSK_DEMOD_MIN_VAL};
        rtl_engine_decode(&e,p,&m); tested++; cases++;
      }
      e.enabled[i]=false;
    }
    fprintf(csv,"%u,\"%s\",%s,%u,%u,%u\n",i+1,d->name,mod?mod:"unhandled",
            rtl_catalog_get(i)->disabled==0,ok,tested);
  }
  fclose(csv); r_logger_set_log_handler(NULL,NULL);
  assert(e.count==377 && runnable==376 && defaults==333);
  unsigned types=0;while(mods){types+=mods&1;mods>>=1;}
  printf("PASS: catalog audit %zu entries, %u runnable, %u default enabled, %u coding types; %u bounded synthetic smoke cases (not 376 device captures)\n",
      e.count,runnable,defaults,types,cases);
  rzi_wind(&e,p);
  rtl_engine_free(&e);free(p);
}
