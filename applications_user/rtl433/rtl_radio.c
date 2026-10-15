/* Native CC1101 receive and raw replay adapter. GPL-3.0-or-later. */
#include "rtl_radio.h"
#include <esp_timer.h>
#include <lib/subghz/devices/cc1101_int/cc1101_int_interconnect.h>
#include <stdlib.h>
#include <string.h>

/* The deployed Furi port caps stack bytes at UINT16_MAX * sizeof(StackType_t).
 * Xtensa ESP-IDF uses uint8_t StackType_t, so the previous 96 KiB request
 * aborts in furi_thread_set_stack_size before either RX worker can start.
 * Retain generous decoder headroom while fitting the existing firmware API.
 * Use the same budget for live and saved-pulse decoding.
 */
#define RTL_DECODER_STACK_BYTES (60u * 1024u)
_Static_assert(RTL_DECODER_STACK_BYTES <= UINT16_MAX,
               "RTL decoder stack exceeds the ESP32 Furi byte limit");

static const char *const names[] = {
    "OOK 270kHz",    "OOK 650kHz",  "FSK dev 2.38k", "FSK dev 12.7k",
    "FSK dev 47.6k", "FSK dev 40k", "AUTO OOK/FSK", "ALL 6 profiles"};
const char *rtl_profile_name(unsigned p) { return p < 8 ? names[p] : "?"; }
RtlSettings rtl_receiver_settings(const RtlSettings *saved, bool automatic) {
  RtlSettings session=*saved;
  session.profile=automatic?RTL_PROFILE_AUTO:(saved->profile<6?saved->profile:1);
  if(!automatic)session.hop=false;
  return session;
}
static const uint32_t hop_frequencies[] = {315000000, 433920000, 868350000,
                                           915000000};
/* 2-FSK 17.2577 kbps, RX BW 270.833kHz, deviation 38.086kHz (CC1101 26MHz
 * crystal). Same class of settings as rtl_433_ESP's 17.24k/40k profile, rounded
 * to hardware registers. Async serial output on GDO0, no whitening, no
 * preamble/sync filtering. */
static const uint8_t fsk40[] = {
    0x02, 0x0d, 0x0b, 0x06, 0x08, 0x32, 0x07, 0x04, 0x15, 0x44, 0x14, 0x00,
    0x13, 0x02, 0x12, 0x04, 0x11, 0x5c, 0x10, 0x69, 0x18, 0x18, 0x19, 0x16,
    0x1b, 0x07, 0x1c, 0x00, 0x1d, 0x91, 0x20, 0xfb, 0x21, 0x56, 0x22, 0x10,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0};
typedef struct {
  uint32_t duration;
  uint32_t end_us;
} Edge;
static void rx_edge(bool level, uint32_t duration, void *ctx) {
  RtlRadio *r = ctx;
  Edge e = {(duration & 0x7fffffffu) | (level ? 0x80000000u : 0),
            (uint32_t)esp_timer_get_time()};
  if (furi_message_queue_put(r->edges, &e, 0) != FuriStatusOk)
    atomic_fetch_add_explicit(&r->edge_drops, 1, memory_order_relaxed);
}
static void configure(RtlRadio *r, unsigned p, uint32_t hz, bool tx) {
  subghz_devices_idle(r->device);
  subghz_devices_reset(r->device);
  if (p == 5) {
    uint8_t preset[sizeof fsk40];
    memcpy(preset, fsk40, sizeof preset);
    /* Match the native 2-FSK PA table. RX-only profile previously had an
     * all-zero table, which cannot emit a usable replay carrier. */
    if (tx) preset[sizeof preset - 8] = 0xc0;
    subghz_devices_load_preset(r->device, FuriHalSubGhzPresetCustom,
                               preset);
  } else {
    const FuriHalSubGhzPreset presets[] = {
        FuriHalSubGhzPresetOok270Async, FuriHalSubGhzPresetOok650Async,
        FuriHalSubGhzPreset2FSKDev238Async, FuriHalSubGhzPreset2FSKDev12KAsync,
        FuriHalSubGhzPreset2FSKDev476Async};
    subghz_devices_load_preset(r->device, presets[p], NULL);
  }
  uint32_t actual = subghz_devices_set_frequency(r->device, hz);
  atomic_store(&r->current_frequency, actual);
  atomic_store(&r->current_profile, p);
}
static void tune(RtlRadio *r, unsigned p, uint32_t hz, bool restart) {
  if (restart)
    subghz_devices_stop_async_rx(r->device);
  configure(r, p, hz, false);
  subghz_devices_flush_rx(r->device);
  Edge e;
  while (furi_message_queue_get(r->edges, &e, 0) == FuriStatusOk) {
  }
  subghz_devices_start_async_rx(r->device, rx_edge, r);
}
static bool publish_frame(RtlRadio *r, RtlFrame *f, bool window) {
  f->queued_us = (uint32_t)esp_timer_get_time();
  f->window = window;
  atomic_fetch_add(&r->bursts, 1);
  furi_mutex_acquire(r->last_lock, FuriWaitForever);
  *r->last = *f;
  atomic_store(&r->last_valid, true);
  furi_mutex_release(r->last_lock);
  RtlFrame *copy = malloc(sizeof(*copy));
  if (copy)
    *copy = *f;
  if (window) atomic_store(&r->window_pending, true);
  if (!copy || furi_message_queue_put(r->frames, &copy, 0) != FuriStatusOk) {
    free(copy);
    atomic_fetch_add(&r->frame_drops, 1);
    if (window) atomic_store(&r->window_pending, false);
    return false;
  }
  return true;
}
static void publish_window(RtlRadio *r, RtlCapture *c, RtlFrame *f) {
  /* Preview work must not occupy every queue slot while a slow decoder is
   * busy. Coalesce unfinished snapshots until the previous one completes;
   * keep the live capture growing, and reserve slots for completed bursts. */
  if (atomic_load(&r->window_pending)) return;
  if (rtl_capture_window(c, f)) {
    if (publish_frame(r, f, true)) atomic_fetch_add(&r->windows, 1);
  }
}
static void consume_edge(RtlRadio *r, RtlCapture *c, RtlFrame *f,
                         unsigned p, Edge e, uint32_t *window_at) {
  uint32_t us = e.duration & 0x7fffffffu;
  bool level = (e.duration >> 31) != 0;
  if (!f->pulses.num_pulses && !c->high)
    f->meta = (RtlMeta){.frequency = atomic_load(&r->current_frequency),
        .time_ms = (uint64_t)furi_get_tick() * 1000u / furi_kernel_get_tick_frequency(),
        .rssi = atomic_load(&r->rssi), .fsk = p >= 2, .profile = p,
        .profile_known = true};
  bool ready = rtl_capture_feed(c, f, level, us);
  if (ready) {
    publish_frame(r, f, false);
    rtl_capture_reset(c, f);
    *window_at = e.end_us;
  } else if (!level && us >= RTL_GAP_US) {
    rtl_capture_reset(c, f);
    *window_at = e.end_us;
  } else if (c->invalid) {
    atomic_fetch_add(&r->invalid, 1);
    rtl_capture_reset(c, f);
    *window_at = e.end_us;
  } else if (f->pulses.num_pulses == PD_MAX_PULSES) {
    publish_window(r, c, f);
    rtl_capture_rollover(c, f);
    *window_at = e.end_us;
  }
}
static int32_t capture_thread(void *ctx) {
  RtlRadio *r = ctx;
  RtlCapture c;
  RtlFrame *f = malloc(sizeof(*f));
  if (!f) {
    atomic_store(&r->running, false);
    return -1;
  }
  rtl_capture_reset(&c, f);
  bool automatic = r->settings.profile >= RTL_PROFILE_AUTO;
  RtlScan scan;
  rtl_scan_init(&scan, 0, r->settings.profile);
  unsigned p = automatic ? rtl_scan_profile(scan.mode, scan.step) : r->settings.profile, hop = 0;
  /* RSSI is polled after radio edges have already started. Retain a bounded
   * 4 ms prefix instead of throwing away the start of short FSK packets. */
  Edge pretrigger[256];
  unsigned pre_head = 0, pre_count = 0;
  uint32_t hz = r->settings.frequency;
  for (unsigned i = 0; i < 4; i++)
    if (hop_frequencies[i] == hz)
      hop = i;
  tune(r, p, hz, false);
  uint32_t last_edge = (uint32_t)esp_timer_get_time(), last_poll = last_edge,
           low_since = 0, hop_at = last_edge, window_at = last_edge;
  rtl_scan_enter(&scan, last_edge);
  unsigned drops = atomic_load(&r->edge_drops);
  bool gated = false, close_pending = false;
  while (atomic_load(&r->running)) {
    uint32_t now = (uint32_t)esp_timer_get_time();
    if (now - last_poll >= 2000) {
      last_poll = now;
      int dbm = (int)subghz_devices_get_rssi(r->device);
      atomic_store(&r->rssi, dbm);
      bool above = dbm >= r->settings.threshold;
      if (above)
        low_since = 0;
      else if (!low_since)
        low_since = now;
      /* OOK has real low gaps; don't gate it by an instantaneous RSSI read.
       * FSK idle data is noise, so use carrier RSSI with a short release hold.
       */
      if (p >= 2) {
        bool open = above || (gated && low_since && now - low_since < 5000);
        close_pending = gated && !open;
        if (!gated && open) {
          for (unsigned i = 0; i < pre_count; i++) {
            Edge prior = pretrigger[(pre_head + 256 - pre_count + i) % 256];
            if (now - prior.end_us <= 4000)
              consume_edge(r, &c, f, p, prior, &window_at);
          }
          pre_head = pre_count = 0;
          gated = true;
        }
      }
      if (dbm > f->meta.rssi)
        f->meta.rssi = dbm;
    }
    if (atomic_load(&r->edge_drops) != drops) {
      drops = atomic_load(&r->edge_drops);
      /* There is a discontinuity somewhere in the pending queue. Discard
       * it and restart cleanly; don't remain blind until a quiet channel. */
      Edge discarded;
      while (furi_message_queue_get(r->edges, &discarded, 0) == FuriStatusOk) {}
      rtl_capture_reset(&c, f);
      pre_head = pre_count = 0;
      last_edge = window_at = (uint32_t)esp_timer_get_time();
      atomic_fetch_add(&r->invalid, 1);
    }
    Edge e;
    unsigned batch = 0;
    while (batch++ < 256 &&
           furi_message_queue_get(r->edges, &e, 0) == FuriStatusOk) {
      last_edge = e.end_us;
      if (p >= 2 && !gated) {
        pretrigger[pre_head] = e;
        pre_head = (pre_head + 1) % 256;
        if (pre_count < 256) pre_count++;
        continue;
      }
      /* Queued edges were captured even if this worker was delayed. Keep
       * them, as in the first receiver: applying the 4 ms pretrigger age
       * bound here discards real packet prefixes under scheduling load.
       * That bound applies only to the closed-gate noise history above. */
      consume_edge(r, &c, f, p, e, &window_at);
    }
    now = (uint32_t)esp_timer_get_time();
    /* A bounded drain is not evidence of radio silence while queued edges
     * remain. The next iteration continues before finalizing/retuning. */
    bool drained = batch <= 256;
    if (drained && close_pending) {
      /* Finish only after queued carrier edges have been consumed. */
      if (rtl_capture_finish(&c, f, RTL_GAP_US)) publish_frame(r, f, false);
      rtl_capture_reset(&c, f);
      gated = close_pending = false;
      pre_head = pre_count = 0;
      window_at = now;
    }
    if (drained && now - last_edge >= RTL_GAP_US) {
      if (rtl_capture_finish(&c, f, now - last_edge))
        publish_frame(r, f, false);
      rtl_capture_reset(&c, f);
      last_edge = window_at = now;
    } else if (drained && (p < 2 || gated) && now - window_at >= 250000) {
      publish_window(r, &c, f);
      window_at = now;
    }
    /* Retain any in-progress OOK frame, including weak frames below the FSK
     * gate. FSK pretrigger noise is not a packet. The 3 s ceiling prevents a
     * noisy mode (even a perpetually busy edge queue) starving all others. */
    bool burst = (p < 2 || gated) && (f->pulses.num_pulses || c.high) &&
                 now - last_edge < RTL_GAP_US;
    atomic_store(&r->scan_holding,
                 automatic && rtl_scan_holding(&scan, now, burst));
    bool retune = automatic ? rtl_scan_due(&scan, now, burst, drained) :
        (r->settings.hop && drained && now - hop_at >= 2000000 &&
         (!burst || now - hop_at >= RTL_SCAN_MAX_US));
    if (retune) {
        publish_window(r, &c, f);
        bool band_boundary = !automatic;
        if (automatic) {
          band_boundary = rtl_scan_advance(&scan);
          p = rtl_scan_profile(scan.mode, scan.step);
        }
        if (r->settings.hop && band_boundary) {
          hop = (hop + 1) % 4;
          hz = hop_frequencies[hop];
        }
        rtl_capture_reset(&c, f);
        tune(r, p, hz, true);
        gated = close_pending = false;
        pre_head = pre_count = 0;
        low_since = 0;
        last_edge = last_poll = hop_at = window_at = (uint32_t)esp_timer_get_time();
        rtl_scan_enter(&scan, hop_at);
        atomic_store(&r->scan_holding, false);
    }
    furi_delay_tick(1);
  }
  subghz_devices_stop_async_rx(r->device);
  subghz_devices_sleep(r->device);
  free(f);
  return 0;
}
static int32_t decoder_thread(void *ctx) {
  RtlRadio *r = ctx;
  while (true) {
    RtlFrame *f = NULL;
    if (furi_message_queue_get(r->frames, &f, FuriWaitForever) != FuriStatusOk)
      continue;
    if (!f)
      break;
    uint32_t began = (uint32_t)esp_timer_get_time();
    atomic_store(&r->queue_us, began - f->queued_us);
    atomic_store(&r->decoding, true);
    unsigned n = rtl_engine_decode(r->engine, &f->pulses, &f->meta);
    atomic_store(&r->decode_us, (uint32_t)esp_timer_get_time() - began);
    atomic_store(&r->decoding, false);
    atomic_fetch_add(&r->decoded, n);
    if (f->window) atomic_store(&r->window_pending, false);
    free(f);
  }
  atomic_store(&r->decode_done, true);
  return 0;
}
bool rtl_radio_init(RtlRadio *r, RtlEngine *engine) {
  memset(r, 0, sizeof(*r));
  r->engine = engine;
  r->edges = furi_message_queue_alloc(4096, sizeof(Edge));
  r->frames = furi_message_queue_alloc(3, sizeof(RtlFrame *));
  r->last_lock = furi_mutex_alloc(FuriMutexTypeNormal);
  r->last = calloc(1, sizeof(*r->last));
  if (!r->edges || !r->frames || !r->last_lock || !r->last)
    return false;
  subghz_devices_init();
  r->registry = true;
  r->device = subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_INT_NAME);
  atomic_store(&r->rssi, -127);
  return r->device != NULL;
}
bool rtl_radio_start(RtlRadio *r, const RtlSettings *s) {
  if (r->transmitting || r->rx_thread || r->decode_thread || s->profile >= 8 ||
      !subghz_devices_is_frequency_valid(r->device, s->frequency))
    return false;
  r->settings = *s;
  atomic_store(&r->decode_us, 0);
  atomic_store(&r->queue_us, 0);
  atomic_store(&r->scan_holding, false);
  atomic_store(&r->window_pending, false);
  r->decode_thread =
      furi_thread_alloc_ex("RTLDecode", RTL_DECODER_STACK_BYTES, decoder_thread, r);
  r->rx_thread = furi_thread_alloc_ex("RTLCapture", 8192, capture_thread, r);
  if (!r->rx_thread || !r->decode_thread) {
    rtl_radio_stop(r);
    return false;
  }
  furi_thread_set_priority(r->rx_thread, FuriThreadPriorityHigh);
  atomic_store(&r->running, true);
  atomic_store(&r->decode_done, false);
  r->offline = false;
  furi_thread_start(r->decode_thread);
  r->decode_started = true;
  furi_thread_start(r->rx_thread);
  r->rx_started = true;
  return true;
}
void rtl_radio_stop(RtlRadio *r) {
  if (r->transmitting) {
    subghz_devices_stop_async_tx(r->device);
    subghz_devices_sleep(r->device);
    r->transmitting = false;
  }
  free(r->tx_frame);
  r->tx_frame = NULL;
  atomic_store(&r->running, false);
  if (r->rx_thread) {
    if (r->rx_started)
      furi_thread_join(r->rx_thread);
    furi_thread_free(r->rx_thread);
    r->rx_thread = NULL;
    r->rx_started = false;
  }
  if (r->decode_thread) {
    if (r->decode_started) {
      if (!r->offline) {
        RtlFrame *end = NULL;
        furi_message_queue_put(r->frames, &end, FuriWaitForever);
      }
      furi_thread_join(r->decode_thread);
    }
    furi_thread_free(r->decode_thread);
    r->decode_thread = NULL;
    r->decode_started = false;
  }
  if (r->frames) {
    RtlFrame *f;
    while (furi_message_queue_get(r->frames, &f, 0) == FuriStatusOk)
      free(f);
  }
  r->offline = false;
}
void rtl_radio_free(RtlRadio *r) {
  rtl_radio_stop(r);
  if (r->registry) {
    subghz_devices_deinit();
    r->registry = false;
  }
  if (r->edges)
    furi_message_queue_free(r->edges);
  if (r->frames) {
    RtlFrame *f;
    while (furi_message_queue_get(r->frames, &f, 0) == FuriStatusOk)
      free(f);
    furi_message_queue_free(r->frames);
  }
  if (r->last_lock)
    furi_mutex_free(r->last_lock);
  free(r->last);
}
bool rtl_radio_last(RtlRadio *r, RtlFrame *f) {
  if (!atomic_load(&r->last_valid))
    return false;
  furi_mutex_acquire(r->last_lock, FuriWaitForever);
  *f = *r->last;
  furi_mutex_release(r->last_lock);
  return true;
}
bool rtl_radio_offline(RtlRadio *r, RtlFrame *f) {
  if (r->transmitting || r->rx_thread || r->decode_thread)
    return false;
  f->meta.offline = true;
  f->window = false;
  f->queued_us = (uint32_t)esp_timer_get_time();
  r->decode_thread =
      furi_thread_alloc_ex("RTLFileDecode", RTL_DECODER_STACK_BYTES, decoder_thread, r);
  if (!r->decode_thread)
    return false;
  furi_message_queue_put(r->frames, &f, 0);
  RtlFrame *end = NULL;
  furi_message_queue_put(r->frames, &end, 0);
  atomic_store(&r->decode_done, false);
  r->offline = true;
  furi_thread_start(r->decode_thread);
  r->decode_started = true;
  return true;
}

const char *rtl_radio_replay_check(const RtlFrame *f, uint32_t *duration) {
  if (!f || f->pulses.sample_rate != 1000000 ||
      f->pulses.num_pulses < 1 || f->pulses.num_pulses > PD_MAX_PULSES)
    return "Invalid raw capture";
  if (f->meta.fsk && (!f->meta.profile_known || !f->tail_known))
    return "Old FSK file: recapture with version 1.1";
  if (f->meta.profile_known && (f->meta.profile > 5 ||
      (f->meta.profile >= 2) != f->meta.fsk))
    return "Capture profile does not match modulation";
  uint64_t total = 0;
  for (unsigned i = 0; i < f->pulses.num_pulses; i++) {
    int pulse = f->pulses.pulse[i], gap = f->pulses.gap[i];
    if (pulse <= 0 || gap <= 0) return "Invalid pulse timing";
    bool last = i + 1 == f->pulses.num_pulses;
    /* The decoder's 100 ms terminal separator is not a captured FSK symbol.
     * OOK's terminal silence needs no additional transmission. */
    uint32_t low = last ? (f->meta.fsk ? f->tail_gap_us : 0) : (uint32_t)gap;
    if (last && low > (uint32_t)gap) return "Invalid terminal timing";
    /* Existing firmware RMT durations have 15 usable bits. Reject long
     * intervals instead of silently wrapping them or requiring a reflash. */
    if (pulse > 32767 || low > 32767)
      return "Replay needs intervals <= 32767 us";
    total += pulse + low;
  }
  if (total > 5000000) return "Capture exceeds 5 second replay limit";
  if (duration) *duration = (uint32_t)total + 1;
  return NULL;
}
static LevelDuration replay_yield(void *ctx) {
  RtlRadio *r = ctx;
  const RtlFrame *f = r->tx_frame;
  unsigned edge = r->tx_edge++;
  unsigned n = f->pulses.num_pulses;
  if (edge < 2 * n - 1)
    return level_duration_make(!(edge & 1), edge & 1 ?
        f->pulses.gap[edge / 2] : f->pulses.pulse[edge / 2]);
  if (f->meta.fsk && f->tail_gap_us) {
    if (edge == 2 * n - 1) return level_duration_make(false, f->tail_gap_us);
    /* Native middleware discards a final low on reset. A 1 us high flushes
     * that last measured symbol; the backend then switches to idle. */
    if (edge == 2 * n) return level_duration_make(true, 1);
  }
  return level_duration_reset();
}
const char *rtl_radio_replay(RtlRadio *r, const RtlFrame *f) {
  uint32_t duration;
  const char *error = rtl_radio_replay_check(f, &duration);
  if (error) return error;
  if (r->transmitting || r->rx_thread || r->decode_thread)
    return "Stop receiver before replay";
  if (!subghz_devices_is_frequency_valid(r->device, f->meta.frequency))
    return "Unsupported capture frequency";
  r->tx_frame = malloc(sizeof(*r->tx_frame));
  if (!r->tx_frame) return "No memory for replay";
  *r->tx_frame = *f;
  r->tx_edge = 0;
  r->tx_duration_us = duration;
  configure(r, f->meta.profile_known ? f->meta.profile : 1, f->meta.frequency, true);
  /* Native start applies the firmware's TX frequency restriction. */
  if (!subghz_devices_start_async_tx(r->device, replay_yield, r)) {
    free(r->tx_frame);
    r->tx_frame = NULL;
    subghz_devices_sleep(r->device);
    return "TX unavailable or blocked by firmware";
  }
  r->transmitting = true;
  r->tx_started_at = (uint32_t)esp_timer_get_time();
  return NULL;
}
int rtl_radio_replay_poll(RtlRadio *r) {
  if (!r->transmitting) return 0;
  bool done = subghz_devices_is_async_complete_tx(r->device);
  bool timeout = (uint32_t)esp_timer_get_time() - r->tx_started_at >
      r->tx_duration_us + 1000000u;
  if (!done && !timeout) return 0;
  rtl_radio_stop(r);
  return done ? 1 : -1;
}
