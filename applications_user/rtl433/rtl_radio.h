#pragma once
#include "rtl_capture.h"
#include "rtl_scan.h"
#include <furi.h>
#include <lib/subghz/devices/devices.h>
#include <stdatomic.h>
#include <lib/toolbox/level_duration.h>
typedef struct {
  uint32_t frequency;
  unsigned profile; /* OOK270, OOK650, FSK2.38, FSK12.7, FSK47.6, FSK40,
                       original AUTO OOK/FSK, optional ALL six-profile scan */
  int threshold; /* FSK carrier gate; OOK is validated by pulse decoders */
  bool hop;
} RtlSettings;
/* Session modes never overwrite saved radio settings. Fixed also pins band. */
RtlSettings rtl_receiver_settings(const RtlSettings *saved, bool automatic);
typedef struct {
  const SubGhzDevice *device;
  FuriMessageQueue *edges, *frames;
  FuriThread *rx_thread, *decode_thread;
  FuriMutex *last_lock;
  RtlFrame *last;
  RtlEngine *engine;
  RtlSettings settings;
  atomic_bool running;
  atomic_uint edge_drops, frame_drops, bursts, decoded, invalid,
      current_frequency, current_profile, windows;
  atomic_int rssi;
  atomic_bool last_valid, decode_done;
  atomic_uint decode_us, queue_us;
  atomic_bool decoding, scan_holding;
  atomic_bool window_pending; /* at most one speculative snapshot in flight */
  bool registry, rx_started, decode_started, offline;
  RtlFrame *tx_frame;
  unsigned tx_edge;
  bool transmitting;
  uint32_t tx_started_at, tx_duration_us;
} RtlRadio;
bool rtl_radio_init(RtlRadio *r, RtlEngine *engine);
bool rtl_radio_start(RtlRadio *r, const RtlSettings *settings);
void rtl_radio_stop(RtlRadio *r);
void rtl_radio_free(RtlRadio *r);
bool rtl_radio_last(RtlRadio *r, RtlFrame *dst);
bool rtl_radio_offline(RtlRadio *r, RtlFrame *owned_frame);
const char *rtl_profile_name(unsigned profile);
/* NULL means replayable. Captures remain immutable until TX is stopped. */
const char *rtl_radio_replay_check(const RtlFrame *f, uint32_t *duration_us);
const char *rtl_radio_replay(RtlRadio *r, const RtlFrame *f);
/* 0 active/idle; 1 complete; -1 timed out. Frees TX state after completion. */
int rtl_radio_replay_poll(RtlRadio *r);
