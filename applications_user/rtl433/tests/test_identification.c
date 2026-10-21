#include "rtl_engine.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static unsigned remote_count, rftech_count, smoke_count, other_count;
static void received(void *ctx, const r_device *d, const data_t *data, const RtlMeta *m) {
  (void)ctx; (void)d; (void)m;
  const char *model = rtl_field(data, "model")->value.v_ptr;
  if (!strcmp(model, "Princeton")) {
    remote_count++;
    assert(rtl_field(data, "bits")->value.v_int == 24);
    assert(rtl_field(data, "repeats")->value.v_int >= 2);
    assert(rtl_field(data, "identification"));
  } else if (!strcmp(model, "RF-tech")) {
    rftech_count++;
    assert(rtl_field(data, "id")->value.v_int == 0x49);
    assert(fabs(rtl_field(data, "temperature_C")->value.v_dbl - 26.4) < 0.001);
  } else if (!strcmp(model, "Smoke-GS558")) smoke_count++;
  else { other_count++; fprintf(stderr, "Additional model: %s\n", model); }
}
static void reset_counts(void) { remote_count = rftech_count = smoke_count = other_count = 0; }
static void init_pulses(pulse_data_t *p) { memset(p, 0, sizeof(*p)); p->sample_rate = 1000000; }
static void pair(pulse_data_t *p, int high, int low) {
  assert(p->num_pulses < PD_MAX_PULSES);
  p->pulse[p->num_pulses] = high; p->gap[p->num_pulses++] = low;
}
static void remote(pulse_data_t *p, uint32_t key, unsigned te, unsigned repeat, bool jitter) {
  for (unsigned r = 0; r < repeat; r++) {
    for (unsigned b = 0; b < 24; b++) {
      bool one = (key >> (23-b)) & 1;
      unsigned high = te * (one ? 3 : 1), low = te * (one ? 1 : 3);
      if (jitter) {high = high * 90 / 100; low = low * 110 / 100;}
      pair(p, high, low);
    }
    pair(p, te, 31*te);
  }
}
static void rftech(pulse_data_t *p) {
  // Published RF-tech example 49 1a 04: id 0x49, temperature 26.4 C.
  for (unsigned r=0;r<5;r++) {
    for (unsigned b=0;b<24;b++) pair(p,500,(0x491a04u >> (23-b)) & 1 ? 4000:2000);
    pair(p,500,7000);
  }
}
static void smoke(pulse_data_t *p) {
  // GS558 documented wake-up, 24 data bits, two short stop pulses.
  for (unsigned r=0;r<5;r++) {
    pair(p,436,11764);
    for (unsigned b=0;b<24;b++) {
      bool one=(0xa55a93u >> (23-b)) & 1;
      pair(p,one?1202:436,one?526:1299);
    }
    pair(p,436,436);pair(p,436,436);
  }
}
static unsigned decode(RtlEngine *e, pulse_data_t *p) {
  RtlMeta m={.frequency=433920000};
  return rtl_engine_decode(e,p,&m);
}
static void optional_only(RtlEngine *e) {
  for (unsigned i=0;i<e->count;i++) {
    const char *n=e->devices[i]->name;
    e->enabled[i]=strstr(n,"Generic Remote") || !strcmp(n,"RF-tech") || strstr(n,"Detector GS 558");
  }
}
void test_identification(void) {
  RtlEngine e; assert(rtl_engine_init(&e,received,NULL,NULL));
  pulse_data_t *p=calloc(1,sizeof(*p)); assert(p);
  for (unsigned i=0;i<e.count;i++) e.enabled[i]=rtl_decoder_runnable(e.devices[i]);
  unsigned ticks[]={280,316,350,390,430};
  for (unsigned jitter=0;jitter<2;jitter++) for (unsigned t=0;t<5;t++)
  for (unsigned button=1;button<=8;button*=2) {
    init_pulses(p); remote(p,0xa55a90 | button,ticks[t],7,jitter);
    p->gap[p->num_pulses-1]=100000;
    for (unsigned warm=0;warm<2;warm++) {
      reset_counts(); decode(&e,p);
      assert(remote_count==1 && !rftech_count && !smoke_count && !other_count);
    }
  }
  puts("PASS: 80 all-decoder Princeton timing/button/jitter/cached-order cases: one family result, no Akhan/RF-tech/GS558 false model");
  optional_only(&e);
  init_pulses(p); rftech(p);p->gap[p->num_pulses-1]=100000;
  reset_counts();decode(&e,p);assert(rftech_count==1 && !remote_count && !smoke_count);
  init_pulses(p);smoke(p);p->gap[p->num_pulses-1]=100000;
  reset_counts();decode(&e,p);assert(smoke_count==1 && !remote_count && !rftech_count);
  // A capture containing separate bursts must retain each transmitter.
  init_pulses(p);remote(p,0xa55a91,316,5,false);
  p->gap[p->num_pulses-1]=100000;rftech(p);
  p->gap[p->num_pulses-1]=100000;smoke(p);
  p->gap[p->num_pulses-1]=100000;reset_counts();decode(&e,p);
  fprintf(stderr,"Mixed capture: Princeton=%u RF-tech=%u GS558=%u other=%u\n",remote_count,rftech_count,smoke_count,other_count);
  assert(remote_count==1 && rftech_count==1 && smoke_count==1 && !other_count);
  puts("PASS: RF-tech published payload, documented GS558 framing, and mixed remote/sensor capture retain real results");
  // Isolate the family decoder to test rejection, not other optional guesses.
  for (unsigned i=0;i<e.count;i++)e.enabled[i]=strstr(e.devices[i]->name,"Generic Remote")!=NULL;
  // The real capture adapter extends the final low gap to 100 ms. A short
  // two-frame press must retain both complete payloads, for every data byte.
  for(unsigned byte=0;byte<256;byte++) {
    init_pulses(p);remote(p,0xa55a00|byte,316,2,false);
    p->gap[p->num_pulses-1]=100000;
    reset_counts();assert(decode(&e,p)==1 && remote_count==1);
  }
  init_pulses(p);remote(p,0xa55a91,316,1,false);p->gap[p->num_pulses-1]=100000;
  reset_counts();assert(!decode(&e,p));
  init_pulses(p);remote(p,0xa55a91,316,1,false);remote(p,0xa55a92,316,1,false);
  p->gap[p->num_pulses-1]=100000;reset_counts();assert(!decode(&e,p));
  init_pulses(p);remote(p,0xa55a91,316,2,false);p->num_pulses--;
  p->gap[p->num_pulses-1]=100000;reset_counts();assert(!decode(&e,p));
  puts("PASS: 256 two-frame button-byte values survive capture termination; single, changed and missing-stop terminal frames rejected");
  init_pulses(p);remote(p,0xa55a91,316,1,false);reset_counts();assert(!decode(&e,p));
  init_pulses(p);remote(p,0xa55a91,316,1,false);remote(p,0xa55a92,316,1,false);
  reset_counts();assert(!decode(&e,p));
  init_pulses(p);remote(p,0xa55a91,316,5,false);
  for(unsigned n=0;n<p->num_pulses;n++) if(n%25!=24)p->gap[n]=316; // wrong second half
  reset_counts();assert(!decode(&e,p));
  init_pulses(p);remote(p,0xa55a91,316,5,false);
  for(unsigned n=24;n<p->num_pulses;n+=25)p->gap[n]=2000; // no valid sync
  reset_counts();assert(!decode(&e,p));
  init_pulses(p);remote(p,0xa55a91,316,3,false);remote(p,0xa55a92,316,3,false);
  reset_counts();assert(decode(&e,p)==2 && remote_count==2);
  // All-zero/all-one payloads still need real framed timing and repeats.
  init_pulses(p);remote(p,0,316,3,false);remote(p,0xffffff,316,3,false);
  reset_counts();assert(decode(&e,p)==2 && remote_count==2);
  // Sample-rate conversion, noise before the frame and a clipped ending.
  init_pulses(p);pair(p,80,100000);remote(p,0xa55a91,316,4,false);
  p->num_pulses-=8;for(unsigned n=0;n<p->num_pulses;n++){p->pulse[n]*=2;p->gap[n]*=2;}
  p->sample_rate=2000000;reset_counts();assert(decode(&e,p)==1 && remote_count==1);
  puts("PASS: incomplete/single/changing/wrong-gap frames rejected; two keys and sample-rate/noise/truncation handled");
  rtl_engine_free(&e);free(p);
}
