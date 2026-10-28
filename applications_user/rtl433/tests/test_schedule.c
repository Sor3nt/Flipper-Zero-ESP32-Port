#include "rtl_engine.h"
#include "lib/rtl433/bitbuffer.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned order[32], used, yields;
static bool accepts[5];
static int fake_decode(r_device *d, bitbuffer_t *bits) {
  assert(bits->num_rows);
  order[used++] = d->protocol_num;
  if (accepts[d->protocol_num - 1]) {
    RtlEngine *e = d->output_ctx;
    e->messages++;
    return 1;
  }
  return 0;
}
static void cooperate(void *ctx) { (void)ctx; yields++; }
static void run(RtlEngine *e, pulse_data_t *p, unsigned expected) {
  RtlMeta meta = {0}; used = yields = 0;
  assert(rtl_engine_decode(e, p, &meta) == expected);
  assert(yields == used); /* skipped entries cannot insert a blocking yield */
}
void test_schedule(void) {
  r_device d[5] = {0}, *devices[5]; bool enabled[5] = {true,true,true,true,true};
  RtlEngine e = {.devices=devices, .enabled=enabled, .count=5, .yield=cooperate};
  for (unsigned i=0; i<5; i++) {
    d[i] = (r_device){.protocol_num=i+1, .name="test", .modulation=OOK_PULSE_PPM,
       .short_width=100, .long_width=200, .reset_limit=1000,
       .decode_fn=fake_decode, .output_ctx=&e}; devices[i]=&d[i];
  }
  pulse_data_t p = {.sample_rate=1000000, .num_pulses=10};
  for (unsigned i=0; i<10; i++) {p.pulse[i]=100; p.gap[i]=200;}
  p.gap[9]=10000;
  accepts[2]=accepts[4]=true;
  run(&e,&p,2); assert(used==5 && order[0]==1 && order[4]==5);
  run(&e,&p,2); assert(used==5 && order[0]==5 && order[1]==3);
  for (unsigned i=1;i<=5;i++) {unsigned n=0;for(unsigned k=0;k<used;k++)n+=order[k]==i;assert(n==1);}
  enabled[4]=false;
  run(&e,&p,1); assert(used==4 && order[0]==3);
  for(unsigned k=0;k<used;k++)assert(order[k]!=5);
  enabled[4]=true; d[4].priority=2;
  run(&e,&p,1); assert(used==4); /* cached fallback must not preempt priority 0 */
  accepts[2]=false;
  run(&e,&p,1); assert(used==5 && order[4]==5); /* fallback still runs if needed */
  memset(enabled,0,sizeof enabled);
  run(&e,&p,0); assert(!used && !yields);
  enabled[2]=true; accepts[2]=true;
  run(&e,&p,1); assert(used==1 && yields==1);
  /* DMC lookahead must not read past a capture ending on a short symbol. */
  d[2].modulation=OOK_PULSE_DMC; d[2].tolerance=10;
  p.num_pulses=PD_MAX_PULSES;
  for(unsigned i=0;i<PD_MAX_PULSES;i++){p.pulse[i]=200;p.gap[i]=200;}
  p.gap[PD_MAX_PULSES-1]=100;
  RtlMeta meta={0};
  assert(rtl_engine_decode(&e,&p,&meta)==0);
  puts("PASS: recent-match scheduling retains all eligible outputs, decoder priority, disabled entries and fallback; no sleeps for skipped entries");
  puts("PASS: DMC full-capacity final short symbol cannot read past capture");
}
