#include "radio_harness.h"
static void test_replay(RtlRadio *r) {
  RtlFrame *f = calloc(1, sizeof(*f)), *parsed = calloc(1, sizeof(*parsed));
  assert(f && parsed);
  f->pulses.sample_rate = 1000000;
  f->pulses.num_pulses = 8;
  for (unsigned i = 0; i < 8; i++) {
    f->pulses.pulse[i] = 120 + i * 10;
    f->pulses.gap[i] = 180 + i * 10;
  }
  f->pulses.gap[7] = RTL_GAP_US;
  f->tail_known = true; f->tail_gap_us = 250;
  for (unsigned profile = 0; profile < 6; profile++) {
    f->meta = (RtlMeta){.frequency = 433920000, .fsk = profile >= 2,
                       .profile = profile, .profile_known = true};
    char *text = rtl_pulse_text(f);
    assert(text && rtl_pulse_parse(text, parsed)); free(text);
    assert(parsed->meta.profile_known && parsed->meta.profile == profile);
    assert(parsed->tail_known && parsed->tail_gap_us == 250);
    assert(!rtl_radio_replay(r, parsed));
    assert(tx_active && loaded_hz == 433920000);
    assert(loaded_preset == (FuriHalSubGhzPreset)profile);
    RtlSettings settings = {433920000, 1, -90, false};
    assert(!rtl_radio_start(r, &settings));
    assert(!rtl_radio_offline(r, parsed));
    assert(rtl_radio_replay(r, parsed));
    /* Releasing the caller's file cannot invalidate the callback's copy. */
    memset(parsed, 0, sizeof(*parsed));
    for (unsigned edge = 0; edge < 15; edge++) {
      LevelDuration ld = tx_callback(tx_context);
      assert(!level_duration_is_reset(ld));
      assert(level_duration_get_level(ld) == !(edge & 1));
      assert(level_duration_get_duration(ld) == (unsigned)(edge & 1 ?
             f->pulses.gap[edge / 2] : f->pulses.pulse[edge / 2]));
    }
    if (profile >= 2) {
      LevelDuration tail = tx_callback(tx_context);
      assert(!level_duration_get_level(tail) && level_duration_get_duration(tail) == 250);
      LevelDuration flush = tx_callback(tx_context);
      assert(level_duration_get_level(flush) && level_duration_get_duration(flush) == 1);
    }
    assert(level_duration_is_reset(tx_callback(tx_context)));
    assert(level_duration_is_reset(tx_callback(tx_context)));
    assert(!rtl_radio_replay_poll(r));
    tx_complete = true;
    assert(rtl_radio_replay_poll(r) == 1 && !tx_active && !r->tx_frame);
  }
  assert(!rtl_radio_replay(r, f));
  rtl_radio_stop(r); assert(!tx_active && !r->tx_frame); /* cancel */
  assert(!rtl_radio_replay(r, f));
  atomic_fetch_add(&clock_offset, 7000000);
  assert(rtl_radio_replay_poll(r) == -1 && !tx_active); /* timeout */
  tx_blocked = true;
  assert(rtl_radio_replay(r, f) && !r->tx_frame && !tx_active);
  tx_blocked = false;
  f->meta.profile_known = false;
  assert(rtl_radio_replay_check(f, NULL)); /* old FSK cannot guess deviation */
  f->meta.fsk = false;
  assert(!rtl_radio_replay_check(f, NULL)); /* legacy OOK */
  f->pulses.gap[0] = 32768;
  assert(rtl_radio_replay_check(f, NULL));
  f->pulses.gap[0] = 0;
  assert(rtl_radio_replay_check(f, NULL));
  assert(tx_count == tx_stops);
  free(f); free(parsed);
  puts("PASS: raw file round-trip and replay timing for all 6 profiles, one burst, cancellation, timeout, TX rejection, RX exclusion and legacy FSK guard");
}
unsigned rtl_engine_decode(RtlEngine *e, const pulse_data_t *p, const RtlMeta *m) {
  assert(e && p && p->num_pulses >= 8 && p->num_pulses <= PD_MAX_PULSES);
  if (!m->offline) {
    if(m->fsk) {
      /* The first eight pairs arrived while RSSI still read below threshold. */
      assert(p->num_pulses==32);
      for(unsigned i=0;i<32;i++)assert(p->pulse[i]==56+(int)i && p->gap[i]==(i==31?RTL_GAP_US:56));
      atomic_fetch_add(&fsk_decodes,1);
      return 1;
    }
    assert(!m->fsk && m->rssi == -115);
    atomic_fetch_add(&live_decodes, 1);
    return 1;
  }
  assert(p->num_pulses == 8);
  atomic_fetch_add(&decodes, 1);
  return 1;
}
static void wait_rx(unsigned count) {
  uint32_t start = furi_get_tick();
  while (atomic_load(&rx_starts) < count) {
    assert(furi_get_tick() - start < 2000);
    usleep(1000);
  }
}
static void wait_live(unsigned count) {
  uint32_t start = furi_get_tick();
  while (atomic_load(&live_decodes) < count) {
    assert(furi_get_tick() - start < 1500);
    usleep(1000);
  }
}
static void inject_pairs(unsigned n) {
  for (unsigned i = 0; i < n; i++) {
    rx_callback(true, 360, rx_context);
    rx_callback(false, 1070, rx_context);
    if (i % 64 == 63) usleep(1000);
  }
}
static void test_receive_stream(RtlRadio *r) {
  atomic_store(&test_rssi, -115);
  RtlSettings settings = {433920000, 1, -90, false};
  unsigned started = atomic_load(&rx_starts);
  assert(rtl_radio_start(r, &settings)); wait_rx(started + 1); usleep(10000);
  unsigned before = atomic_load(&live_decodes);
  inject_pairs(16); wait_live(before + 1);
  /* At -115 dBm the old OOK software gate discarded this complete frame. */
  before = atomic_load(&live_decodes);
  inject_pairs(PD_MAX_PULSES + 300); wait_live(before + 1);
  assert(atomic_load(&r->windows));
  assert(!atomic_load(&r->edge_drops));
  usleep(120000);
  /* Exercise periodic windows without quiet or a full 1200-pair buffer. */
  before = atomic_load(&live_decodes);
  for (unsigned i = 0; i < 170; i++) {
    inject_pairs(1); usleep(2000);
  }
  assert(atomic_load(&live_decodes) > before);
  usleep(120000);
  before = atomic_load(&live_decodes);
  /* Invalid duration must recover on subsequent pulses, without long idle. */
  rx_callback(true, 200000, rx_context);
  inject_pairs(16); wait_live(before + 1);
  assert(atomic_load(&r->invalid));
  usleep(120000);
  /* Report a queue discontinuity, then send new input after it is drained. */
  unsigned invalid = atomic_load(&r->invalid);
  atomic_fetch_add(&r->edge_drops, 1);
  uint32_t start = furi_get_tick();
  while (atomic_load(&r->invalid) == invalid) {
    assert(furi_get_tick() - start < 1000); usleep(1000);
  }
  before = atomic_load(&live_decodes);
  inject_pairs(16); wait_live(before + 1);
  rtl_radio_stop(r);
  assert(atomic_load(&rx_starts) == started + 1); /* fixed mode never hops */
  atomic_store(&test_rssi, -60);
  puts("PASS: actual receiver worker accepts weak OOK below FSK gate, publishes full/periodic windows, recovers invalid edges and queue loss without requiring quiet");
}
static void test_fsk_prefix(RtlRadio *r) {
  RtlSettings settings={433920000,5,-90,false};
  atomic_store(&test_rssi,-115);
  unsigned started=atomic_load(&rx_starts);
  assert(rtl_radio_start(r,&settings));wait_rx(started+1);usleep(10000);
  for(unsigned i=0;i<8;i++) {
    rx_callback(true,56+i,rx_context);rx_callback(false,56,rx_context);
  }
  /* Wait for the capture worker to retain these while its carrier gate is off. */
  usleep(1200);
  atomic_store(&test_rssi,-50);
  usleep(2500);
  for(unsigned i=8;i<32;i++) {
    rx_callback(true,56+i,rx_context);rx_callback(false,56,rx_context);
  }
  atomic_store(&test_rssi,-115);
  uint32_t start=furi_get_tick();
  while(!atomic_load(&fsk_decodes)) {
    assert(furi_get_tick()-start<1000);usleep(1000);
  }
  rtl_radio_stop(r);atomic_store(&test_rssi,-60);
  puts("PASS: FSK pretrigger retains exact 32-pair input including eight pairs before RSSI trigger, then drains carrier tail before close");
}
static void test_scan(RtlRadio *r) {
  RtlSettings saved={433920000,1,-95,true}, copy=saved;
  RtlSettings automatic=rtl_receiver_settings(&saved,true),fixed=rtl_receiver_settings(&saved,false);
  assert(!memcmp(&saved,&copy,sizeof saved));
  assert(automatic.profile==6 && automatic.hop);
  assert(fixed.profile==1 && !fixed.hop);
  automatic.profile=RTL_PROFILE_ALL;
  unsigned started=atomic_load(&rx_starts),seen=0;
  assert(rtl_radio_start(r,&automatic));wait_rx(started+1);
  for(unsigned i=0;i<7;i++) {
    if(i) {usleep(4000);atomic_fetch_add(&clock_offset,330000);wait_rx(started+i+1);}
    unsigned p=atomic_load(&r->current_profile);assert(p==rtl_scan_profile(RTL_PROFILE_ALL,i));seen|=1u<<p;
    assert(atomic_load(&r->current_frequency)==(i<6?433920000u:868350000u));
  }
  assert(seen==63);rtl_radio_stop(r);
  puts("PASS: actual worker fast-scans all six profiles before band hop; session selection preserves saved settings and FIXED never hops");
}
static void test_scan_burst(RtlRadio *r) {
  RtlSettings automatic={433920000,RTL_PROFILE_ALL,-90,false};
  atomic_store(&test_rssi,-115);
  unsigned started=atomic_load(&rx_starts),before=atomic_load(&live_decodes);
  assert(rtl_radio_start(r,&automatic));wait_rx(started+1);
  usleep(190000);
  /* A weak OOK burst straddles the 250 ms probe deadline. Keep complete
   * pulses coming beyond that deadline and check the real worker stays put. */
  for(unsigned i=0;i<5;i++) {
    inject_pairs(4);usleep(40000);
    assert(atomic_load(&rx_starts)==started+1);
    assert(atomic_load(&r->current_profile)==1);
  }
  assert(atomic_load(&r->scan_holding));
  rx_callback(false,RTL_GAP_US,rx_context);
  wait_rx(started+2);wait_live(before+1);
  assert(atomic_load(&r->current_profile)==5);
  RtlFrame *last=malloc(sizeof(*last));assert(last);
  assert(rtl_radio_last(r,last));
  assert(last->pulses.num_pulses==20 && last->meta.profile==1);
  free(last);
  rtl_radio_stop(r);atomic_store(&test_rssi,-60);
  puts("PASS: actual worker retains weak 20-pair OOK burst across fast-scan deadline, then advances after packet end with original profile metadata");
}
static void test_original_window(RtlRadio *r) {
  RtlSettings original={433920000,6,-90,false};
  unsigned started=atomic_load(&rx_starts),before=atomic_load(&live_decodes);
  atomic_store(&test_rssi,-115);
  assert(rtl_radio_start(r,&original));wait_rx(started+1);
  usleep(500000);
  /* The first build was still listening for OOK here. v1.1.4 had already
   * switched to FSK, so this valid late-arriving OOK burst was missed. */
  assert(atomic_load(&r->current_profile)==1);
  assert(atomic_load(&rx_starts)==started+1);
  uint32_t sent=furi_get_tick();
  inject_pairs(16);wait_live(before+1);
  assert(furi_get_tick()-sent<500); /* decode promptly, not after 2 s dwell */
  assert(atomic_load(&rx_starts)==started+1);
  RtlFrame *frame=malloc(sizeof(*frame));assert(frame);
  assert(rtl_radio_last(r,frame) && frame->pulses.num_pulses==16);
  assert(frame->meta.profile==1 && !frame->meta.fsk);
  free(frame);
  atomic_fetch_add(&clock_offset,1600000);
  wait_rx(started+2);
  assert(atomic_load(&r->current_profile)==5);
  rtl_radio_stop(r);atomic_store(&test_rssi,-60);
  puts("PASS: restored original 2-second OOK/FSK dwell captures weak OOK at 500 ms that v1.1.4 missed, then switches to FSK 40");
}
int main(int argc, char **argv) {
  RtlRadio r;
  RtlEngine engine = {0};
  assert(rtl_radio_init(&r, &engine));
  if (argc > 1 && !strcmp(argv[1], "original-window")) {
    test_original_window(&r);rtl_radio_free(&r);return 0;
  }
  if (argc > 1 && !strcmp(argv[1], "offline")) {
    RtlFrame *f = calloc(1, sizeof(*f));
    assert(f);
    f->pulses.num_pulses = 8;
    assert(rtl_radio_offline(&r, f));
    rtl_radio_stop(&r);
    rtl_radio_free(&r);
    return 0;
  }
  for (unsigned round = 0; round < 32; round++) {
    RtlSettings s = {433920000, round % 8, -90, false};
    unsigned before = atomic_load(&rx_starts);
    assert(rtl_radio_start(&r, &s));
    wait_rx(before + 1);
    assert(!rtl_radio_start(&r, &s));
    if (s.profile >= RTL_PROFILE_AUTO) {
      /* Advance only the radio clock to exercise automatic retuning. */
      usleep(4000);
      atomic_fetch_add(&clock_offset, 3100000);
      wait_rx(before + 2);
    }
    rtl_radio_stop(&r);
    rtl_radio_stop(&r);
    assert(!r.rx_thread && !r.decode_thread && !atomic_load(&r.running));
    assert(atomic_load(&thread_count) == 0);
    assert(atomic_load(&rx_starts) == atomic_load(&rx_stops));
  }
  RtlSettings invalid = {433920000, 8, -90, false};
  assert(!rtl_radio_start(&r, &invalid));
  invalid.profile = 1;
  invalid.frequency = 1;
  assert(!rtl_radio_start(&r, &invalid));
  for (unsigned i = 0; i < 4; i++) {
    RtlFrame *f = calloc(1, sizeof(*f));
    assert(f);
    f->pulses.num_pulses = 8;
    assert(rtl_radio_offline(&r, f));
    rtl_radio_stop(&r);
    assert(atomic_load(&r.decode_done));
    assert(!atomic_load(&r.decoding));
    assert(atomic_load(&r.queue_us) < 1000000);
  }
  assert(atomic_load(&decodes) == 4);
  assert(atomic_load(&thread_count) == 0);
  test_receive_stream(&r);
  test_fsk_prefix(&r);
  test_scan(&r);
  test_scan_burst(&r);
  test_original_window(&r);
  test_replay(&r);
  rtl_radio_free(&r);
  assert(registry_count == 0);
  printf("PASS: receive startup with ESP32 stack limit (max requested %u bytes), all 8 modes, 32 start/stop cycles, auto-retune and 4 offline decodes\n", largest_stack);
  return 0;
}
