#include "rtl_engine.h"
#include "rtl_capture.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned bells;
static void output(void *ctx, const r_device *d, const data_t *data, const RtlMeta *m) {
  (void)ctx;
  char *json = rtl_json(d, data, m); assert(json);
  if (strstr(json, "Quhwa-Doorbell")) {
    assert(strstr(json, "\"id\":4671"));
    bells++;
  }
  free(json);
}
static void pair(RtlEngine *e, RtlCapture *c, RtlFrame *f, unsigned high, unsigned low) {
  assert(!rtl_capture_feed(c, f, true, high));
  assert(!rtl_capture_feed(c, f, false, low));
  assert(!c->invalid);
  if (f->pulses.num_pulses == PD_MAX_PULSES) {
    assert(rtl_capture_window(c, f));
    rtl_engine_decode(e, &f->pulses, &f->meta);
    rtl_capture_rollover(c, f);
    assert(f->pulses.num_pulses == PD_MAX_PULSES / 2);
  }
}
static void bell(RtlEngine *e, RtlCapture *c, RtlFrame *f) {
  /* Independently assembled from quhwa.c's timing/format documentation.
   * Seven repetitions, inverted ID 0x123f and two fixed final bits. */
  unsigned bits = ((~0x123fu & 0xffffu) << 2);
  for (unsigned rep = 0; rep < 7; rep++) for (unsigned bit = 0; bit < 18; bit++) {
    bool one = (bits & (1u << (17 - bit))) != 0;
    pair(e, c, f, one ? 360 : 1070, bit == 17 ? 6500 : one ? 1070 : 360);
  }
}
void test_doorbell(void) {
  RtlEngine e; assert(rtl_engine_init(&e, output, NULL, NULL));
  RtlFrame *f = calloc(1, sizeof(*f)); assert(f); RtlCapture c;
  /* Run with every default decoder, both cold and recently-matched order.
   * Test background longer than capacity and packets crossing rollover. */
  const unsigned prefixes[] = {0, 1210, 1170, 2370, 0};
  for (unsigned run = 0; run < sizeof(prefixes)/sizeof(*prefixes); run++) {
    rtl_capture_reset(&c, f);
    f->meta = (RtlMeta){.frequency=433920000, .rssi=-110};
    for (unsigned i = 0; i < prefixes[run]; i++) pair(&e, &c, f, 80, 80);
    unsigned before = bells;
    bell(&e, &c, f);
    /* No 100 ms silence: periodic snapshot must already decode the bell. */
    assert(rtl_capture_window(&c, f));
    assert(f->tail_known && f->tail_gap_us == 6500);
    assert(f->pulses.gap[f->pulses.num_pulses-1] == 6500);
    rtl_engine_decode(&e, &f->pulses, &f->meta);
    assert(bells > before);
    assert(!rtl_capture_window(&c, f)); /* no repeated idle snapshots */
    assert(rtl_capture_finish(&c, f, RTL_GAP_US));
  }
  /* Rollover preserves a pending high interval and all retained durations. */
  rtl_capture_reset(&c, f);
  for (unsigned i = 0; i < PD_MAX_PULSES; i++) {
    rtl_capture_feed(&c, f, true, 80+i); rtl_capture_feed(&c, f, false, 90+i);
  }
  assert(!rtl_capture_feed(&c, f, true, 360));
  rtl_capture_rollover(&c, f);
  assert(c.high == 360 && f->pulses.pulse[0] == 80+PD_MAX_PULSES/2);
  rtl_capture_feed(&c, f, false, 1070);
  assert(!c.invalid && f->pulses.pulse[PD_MAX_PULSES/2] == 360);
  rtl_engine_free(&e); free(f);
  puts("PASS: Quhwa/1-by-One doorbell clean, warm, after continuous noise, across two rollover boundaries and without terminal silence; raw timings preserved");
}
