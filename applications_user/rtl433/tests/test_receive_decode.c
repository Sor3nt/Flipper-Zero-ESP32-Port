/* Full path: GPIO callback -> real capture worker -> actual catalog/slicers
 * -> decoded fields. Radio and task scheduling are simulated, not RF. */
#include "radio_harness.h"

static atomic_uint weather_messages, prologue_messages;
static atomic_bool hold_decoder, decoder_waiting;
static void output(void *ctx, const r_device *d, const data_t *data, const RtlMeta *m) {
  (void)ctx;
  char *json = rtl_json(d, data, m);
  assert(json);
  if (strstr(json, "AmbientWeather-WH31E")) {
    assert(strstr(json, "\"id\":195"));
    assert(strstr(json, "\"humidity\":92"));
    assert(m->fsk && m->frequency == 915000000);
    atomic_fetch_add(&weather_messages, 1);
  }
  if (strstr(json, "Prologue-TH")) {
    assert(strstr(json, "\"temperature_C\":23.4"));
    assert(strstr(json, "\"humidity\":56"));
    atomic_fetch_add(&prologue_messages, 1);
  }
  free(json);
}
static void await_flag(atomic_bool *flag, bool value) {
  uint32_t start = furi_get_tick();
  while (atomic_load(flag) != value) {
    assert(furi_get_tick() - start < 2000);
    usleep(100);
  }
}
static void inject_weather(void) {
  /* Same independently published WH31E packet used by test_engine.c. */
  const unsigned char wh[] = {0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0x2d,0xd4,
      0x30,0xc3,0x81,0xd5,0x5c,0x2a,0xcf,0x08,0x35,0x44,0x2c};
  bool level = true;
  unsigned duration = 0;
  for (unsigned bit = 0; bit < sizeof(wh)*8; bit++) {
    bool next = !!(wh[bit/8] & (0x80 >> (bit%8)));
    if (next != level) {
      if (duration) rx_callback(level, duration, rx_context);
      duration = 0;
      level = next;
    }
    duration += 56;
  }
  rx_callback(level, duration, rx_context);
}
static void cooperate(void *ctx) {
  (void)ctx;
  if (atomic_load(&hold_decoder)) {
    atomic_store(&decoder_waiting, true);
    while (atomic_load(&hold_decoder)) usleep(100);
    atomic_store(&decoder_waiting, false);
  }
}
static void inject_pair(unsigned high, unsigned low) {
  rx_callback(true, high, rx_context);
  rx_callback(false, low, rx_context);
}
static void test_backlog(RtlRadio *r) {
  RtlSettings s = {433920000, 1, -90, false};
  atomic_store(&test_rssi, -60);
  atomic_store(&hold_decoder, true);
  assert(rtl_radio_start(r, &s));
  await_flag(&rx_active, true);
  /* Continuous background causes periodic snapshots while decoding is slow.
   * No 100 ms quiet boundary occurs until the final valid sensor burst. */
  for (unsigned i = 0; i < 80; i++) {
    for (unsigned j = 0; j < 8; j++) inject_pair(80, 80);
    usleep(20000);
  }
  assert(atomic_load(&decoder_waiting));
  /* Discard the noisy candidate at a measured long high, then a full sensor
   * burst. The current recovery path accepts the following clean packet. */
  rx_callback(true, 200000, rx_context);
  const unsigned char b[] = {0x91,0x28,0x0e,0xa3,0x80};
  for (unsigned rep = 0; rep < 7; rep++) {
    for (unsigned bit = 0; bit < 36; bit++)
      inject_pair(500, (b[bit/8] & (0x80 >> (bit%8))) ? 4000 : 2000);
    inject_pair(500, 9000);
  }
  rx_callback(false, RTL_GAP_US, rx_context);
  usleep(20000);
  unsigned dropped = atomic_load(&r->frame_drops);
  atomic_store(&hold_decoder, false);
  await_flag(&decoder_waiting, false);
  uint32_t start = furi_get_tick();
  while (!atomic_load(&prologue_messages) && furi_get_tick()-start < 1000) usleep(1000);
  rtl_radio_stop(r);
  printf("OOK backlog: Prologue messages=%u edge_drops=%u frame_drops=%u\n",
         atomic_load(&prologue_messages), atomic_load(&r->edge_drops), dropped);
  assert(!dropped && !atomic_load(&r->edge_drops));
  assert(atomic_load(&prologue_messages) == 1);
  puts("PASS: partial noisy snapshots cannot fill the queue and discard a complete Prologue sensor burst");
}
int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  RtlEngine engine;
  RtlRadio radio;
  assert(rtl_engine_init(&engine, output, cooperate, NULL));
  assert(rtl_radio_init(&radio, &engine));
  if (argc > 1 && !strcmp(argv[1], "backlog")) {
    test_backlog(&radio);
    rtl_radio_free(&radio);
    rtl_engine_free(&engine);
    return 0;
  }
  RtlSettings s = {915000000, 5, -90, false};
  atomic_store(&test_rssi, -115);
  assert(rtl_radio_start(&radio, &s));
  await_flag(&rx_active, true);
  usleep(10000);
  atomic_store(&pause_capture, true);
  await_flag(&capture_paused, true);
  /* Edges received while the worker cannot run, but no queue overflow.
   * The old age filter discards the entire valid packet at the next poll. */
  inject_weather();
  usleep(12000);
  atomic_store(&test_rssi, -50);
  atomic_store(&pause_capture, false);
  await_flag(&capture_paused, false);
  usleep(10000);
  atomic_store(&test_rssi, -115);
  uint32_t start = furi_get_tick();
  while (!atomic_load(&weather_messages) && furi_get_tick()-start < 1000) usleep(1000);
  rtl_radio_stop(&radio);
  printf("WH31E through queued FSK capture: messages=%u edge_drops=%u frame_drops=%u\n",
         atomic_load(&weather_messages), atomic_load(&radio.edge_drops), atomic_load(&radio.frame_drops));
  assert(!atomic_load(&radio.edge_drops));
  assert(atomic_load(&weather_messages) == 1);
  rtl_radio_free(&radio);
  rtl_engine_free(&engine);
  puts("PASS: queued FSK packet survives 12 ms worker delay and is identified by real decoder with ID/humidity intact");
  return 0;
}
