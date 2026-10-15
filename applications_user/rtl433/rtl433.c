/* Native RTL433 app for T-Embed Native Tools ESP32-S3. GPL-3.0-or-later. */
#include "rtl_file.h"
#include "rtl_radio.h"
#include "rtl_text.h"
#include "lib/rtl433/logger.h"
#include "ui/native_ui.h"
#include <dialogs/dialogs.h>
#include <stdio.h>
#include <stdlib.h>
#include <storage/storage.h>
#include <string.h>
#define ROOT "/ext/apps_data/rtl433"
#define HISTORY 64
#define HISTORY_BYTES (1024 * 1024)
typedef struct {
  char model[40], id[32], channel[16];
  char *json, *key;
  RtlMeta meta;
  unsigned protocol, hits;
} Record;
typedef enum {
  Home,
  Live,
  History,
  Detail,
  Settings,
  Catalog,
  Pulses,
  Replay,
  About
} Screen;
typedef struct {
  NativeUi ui;
  RtlEngine engine;
  RtlRadio radio;
  RtlSettings settings;
  FuriMessageQueue *events;
  Storage *storage;
  File *log;
  Record *records[HISTORY];
  size_t bytes;
  unsigned count, selected, menu, scroll, catalog;
  atomic_uint output_drops;
  atomic_uint diagnostics;
  unsigned logged, log_bytes;
  bool logging, exit;
  Screen screen;
  uint32_t updated;
  char log_path[100];
  char *detail;
  unsigned detail_lines;
  RtlFrame *replay;
  uint32_t decoder_yielded;
  int8_t signal[48];
  unsigned signal_count, resume_profile;
  uint32_t signal_at;
} App;
static void release_record(Record *r) {
  if (r) {
    free(r->json);
    free(r->key);
    free(r);
  }
}
static size_t record_bytes(const Record *r) {
  return strlen(r->json) + strlen(r->key) + sizeof(*r) + 2;
}
static void decoded(void *ctx, const r_device *d, const data_t *data,
                    const RtlMeta *meta) {
  App *a = ctx;
  Record *r = calloc(1, sizeof(*r));
  if (!r) {
    atomic_fetch_add(&a->output_drops, 1);
    return;
  }
  r->json = rtl_json(d, data, meta);
  r->key = rtl_identity(d, data);
  if (!r->json || !r->key) {
    release_record(r);
    atomic_fetch_add(&a->output_drops, 1);
    return;
  }
  rtl_value_text(rtl_field(data, "model"), r->model, sizeof r->model);
  rtl_value_text(rtl_field(data, "id"), r->id, sizeof r->id);
  rtl_value_text(rtl_field(data, "channel"), r->channel, sizeof r->channel);
  r->protocol = d->protocol_num;
  r->meta = *meta;
  r->hits = 1;
  if (furi_message_queue_put(a->events, &r, 0) != FuriStatusOk) {
    release_record(r);
    atomic_fetch_add(&a->output_drops, 1);
  }
}
static void decoder_yield(void *ctx) {
  App *a = ctx;
  uint32_t now = furi_get_tick();
  /* Keep the UI and idle task responsive on the single-core firmware without
   * sleeping once per catalog block even when almost everything is disabled. */
  if (now - a->decoder_yielded >= furi_ms_to_ticks(4)) {
    furi_delay_tick(1);
    a->decoder_yielded = furi_get_tick();
  } else furi_delay_tick(0);
}
static void decoder_log(log_level_t level, const char *src, const char *msg, void *ctx) {
  App *a = ctx;
  if (level <= LOG_WARNING) atomic_fetch_add(&a->diagnostics, 1);
  /* Repeated noisy-input warnings must not block the decoder on serial I/O.
   * Keep real errors visible on serial; the UI counts warning diagnostics. */
  if (level <= LOG_ERROR) printf("RTL433 %s: %s\n", src, msg);
}
static bool write_all(File *f, const char *s) {
  size_t n = strlen(s);
  return storage_file_write(f, s, n) == n;
}
static bool new_file(App *a, File *f, const char *prefix, const char *ext,
                     char *path, size_t cap) {
  storage_simply_mkdir(a->storage, "/ext/apps_data");
  storage_simply_mkdir(a->storage, ROOT);
  for (unsigned i = 0; i < 1000; i++) {
    snprintf(path, cap, ROOT "/%s-%lu-%u.%s", prefix,
             (unsigned long)furi_get_tick(), i, ext);
    if (!storage_file_exists(a->storage, path))
      return storage_file_open(f, path, FSAM_WRITE, FSOM_CREATE_NEW);
  }
  return false;
}
static void close_log(App *a) {
  if (storage_file_is_open(a->log)) {
    bool ok = storage_file_sync(a->log);
    storage_file_close(a->log);
    if (!ok)
      nui_status(&a->ui, "SD sync failed: check log file");
  }
}
static void log_record(App *a, Record *r) {
  if (!a->logging)
    return;
  if (!storage_file_is_open(a->log)) {
    if (!new_file(a, a->log, "session", "jsonl", a->log_path,
                  sizeof a->log_path)) {
      a->logging = false;
      nui_status(&a->ui, "SD logging failed to open");
      return;
    }
    a->log_bytes = a->logged = 0;
  }
  size_t n = strlen(r->json);
  if (a->log_bytes + n > 5 * 1024 * 1024) {
    close_log(a);
    a->logging = false;
    nui_status(&a->ui, "5 MiB log limit reached; logging stopped");
    return;
  }
  bool ok = write_all(a->log, r->json) && write_all(a->log, "\n");
  a->log_bytes += n + 1;
  if (++a->logged % 16 == 0)
    ok = storage_file_sync(a->log) && ok;
  if (!ok) {
    close_log(a);
    a->logging = false;
    nui_status(&a->ui, "SD write failed; logging stopped");
  }
}
static void drain(App *a) {
  Record *r;
  unsigned drained = 0;
  while (drained++ < 8 && furi_message_queue_get(a->events, &r, 0) == FuriStatusOk) {
    log_record(a, r);
    unsigned i = 0;
    while (i < a->count && strcmp(a->records[i]->key, r->key))
      i++;
    if (i < a->count) {
      r->hits += a->records[i]->hits;
      a->bytes -= record_bytes(a->records[i]);
      release_record(a->records[i]);
      a->records[i] = r;
    } else {
      if (a->count == HISTORY) {
        a->bytes -= record_bytes(a->records[0]);
        release_record(a->records[0]);
        memmove(a->records, a->records + 1, (--a->count) * sizeof(Record *));
      }
      a->records[a->count++] = r;
    }
    a->bytes += record_bytes(r);
    while (a->bytes > HISTORY_BYTES && a->count > 1) {
      a->bytes -= record_bytes(a->records[0]);
      release_record(a->records[0]);
      memmove(a->records, a->records + 1, (--a->count) * sizeof(Record *));
    }
    if (a->selected >= a->count)
      a->selected = a->count - 1;
    a->ui.dirty = true;
  }
}
static char *read_text(App *a, const char *path, size_t max) {
  File *f = storage_file_alloc(a->storage);
  char *s = NULL;
  if (storage_file_open(f, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
    uint64_t n = storage_file_size(f);
    if (n && n <= max) {
      s = malloc(n + 1);
      if (s) {
        if (storage_file_read(f, s, n) != n) {
          free(s);
          s = NULL;
        } else
          s[n] = 0;
      }
    }
    storage_file_close(f);
  }
  storage_file_free(f);
  return s;
}
static void config(App *a, bool save) {
  if (save) {
    File *f = storage_file_alloc(a->storage);
    storage_simply_mkdir(a->storage, ROOT);
    char head[100];
    bool ok = storage_file_open(f, ROOT "/settings.tmp", FSAM_WRITE,
                                FSOM_CREATE_ALWAYS);
    snprintf(head, sizeof head, "RTL433-CFG 1\n%lu %u %d %u\n",
             (unsigned long)a->settings.frequency, a->settings.profile,
             a->settings.threshold, a->settings.hop);
    if (ok) {
      ok = write_all(f, head);
      for (unsigned i = 0; i < a->engine.count && ok; i++)
        ok = write_all(f, a->engine.enabled[i] ? "1" : "0");
      ok = storage_file_sync(f) && ok;
      storage_file_close(f);
    }
    storage_file_free(f);
    if (ok)
      ok = storage_common_rename(a->storage, ROOT "/settings.tmp",
                                 ROOT "/settings.txt") == FSE_OK;
    if (!ok)
      nui_status(&a->ui, "Settings not saved: check SD card");
  } else {
    char *s = read_text(a, ROOT "/settings.txt", 4096);
    if (!s)
      return;
    unsigned long hz;
    unsigned p, h;
    int t, n = 0;
    if (sscanf(s, "RTL433-CFG 1\n%lu %u %d %u\n%n", &hz, &p, &t, &h, &n) == 4 &&
        p < 7 && h < 2 && t >= -120 && t <= -20 &&
        subghz_devices_is_frequency_valid(a->radio.device, hz) &&
        strlen(s + n) == a->engine.count) {
      bool valid = true;
      for (unsigned i = 0; i < a->engine.count; i++)
        if (s[n + i] != '0' && s[n + i] != '1')
          valid = false;
      if (valid) {
        a->settings = (RtlSettings){hz, p < 6 ? p : 1, t, h};
        for (unsigned i = 0; i < a->engine.count; i++)
          a->engine.enabled[i] =
              s[n + i] == '1' && rtl_decoder_runnable(a->engine.devices[i]);
      }
    }
    free(s);
  }
}
static void save_pulse(App *a) {
  RtlFrame *f = malloc(sizeof(*f));
  if (!f) {
    nui_status(&a->ui, "No memory");
    return;
  }
  if (!rtl_radio_last(&a->radio, f)) {
    free(f);
    nui_status(&a->ui, "No captured burst yet");
    return;
  }
  char *s = rtl_pulse_text(f);
  free(f);
  File *out = storage_file_alloc(a->storage);
  char path[100];
  bool ok = s && new_file(a, out, "pulse", "rtl", path, sizeof path);
  if (ok)
    ok = write_all(out, s) && storage_file_sync(out);
  if (storage_file_is_open(out))
    storage_file_close(out);
  storage_file_free(out);
  free(s);
  nui_status(&a->ui,
             ok ? "Pulse saved to apps_data/rtl433/" : "Pulse save failed");
}
static void preview_replay(App *a, RtlFrame *owned) {
  free(a->replay);
  a->replay = owned;
  a->screen = Replay;
  const char *error = rtl_radio_replay_check(owned, NULL);
  nui_status(&a->ui, "%s", error ? error : "Ready: hold OK to replay once");
}
static void open_pulse(App *a, bool replay) {
  rtl_radio_stop(&a->radio);
  DialogsApp *dialogs = furi_record_open(RECORD_DIALOGS);
  FuriString *path = furi_string_alloc_set_str(ROOT);
  DialogsFileBrowserOptions o;
  dialog_file_browser_set_basic_options(&o, ".rtl", NULL);
  o.base_path = ROOT;
  nui_suspend(&a->ui);
  bool chosen = dialog_file_browser_show(dialogs, path, path, &o);
  nui_resume(&a->ui);
  if (chosen) {
    char *s = read_text(a, furi_string_get_cstr(path), 40000);
    RtlFrame *f = malloc(sizeof(*f));
    bool ok = s && f && rtl_pulse_parse(s, f);
    free(s);
    if (ok && replay) {
      preview_replay(a, f);
      f = NULL;
    } else if (ok)
      ok = rtl_radio_offline(&a->radio, f);
    if (!ok)
      free(f);
    if (!replay || !ok)
      nui_status(&a->ui, ok ? "Decoding pulse file..."
                            : "Invalid pulse file or no memory");
    if (ok && !replay)
      a->screen = Live;
  }
  furi_string_free(path);
  furi_record_close(RECORD_DIALOGS);
}
static void export_history(App *a) {
  File *j = storage_file_alloc(a->storage), *c = storage_file_alloc(a->storage);
  char jp[100], cp[100];
  bool ok = new_file(a, j, "history", "jsonl", jp, sizeof jp) &&
            new_file(a, c, "history", "csv", cp, sizeof cp);
  if (ok)
    ok = write_all(c, "model,id,channel,messages,frequency_hz,rssi_dbm,json\n");
  for (unsigned i = 0; i < a->count && ok; i++) {
    Record *r = a->records[i];
    char *row = rtl_csv_record(r->model, r->id, r->channel, r->hits, &r->meta, r->json);
    ok = row && write_all(j, r->json) && write_all(j, "\n") && write_all(c, row);
    free(row);
  }
  if (storage_file_is_open(j)) {
    ok = storage_file_sync(j) && ok;
    storage_file_close(j);
  }
  if (storage_file_is_open(c)) {
    ok = storage_file_sync(c) && ok;
    storage_file_close(c);
  }
  storage_file_free(j);
  storage_file_free(c);
  nui_status(&a->ui, ok ? "JSONL + CSV saved to apps_data/rtl433/"
                        : "Export failed: check SD card");
}
static const char *menus[] = {"Start receiver (original)",
                              "Start ALL profiles (scan)",
                              "Device history",
                              "Radio settings",
                              "Decoder catalog",
                              "Last raw pulse / save",
                              "Decode saved pulse",
                              "Export history JSONL + CSV",
                              "SD session logging",
                              "Replay last raw capture",
                              "Replay saved pulse",
                              "Start FIXED receiver",
                              "About / limits"};
#define MENU_COUNT (sizeof menus / sizeof menus[0])
static void render(App *a) {
  NativeUi *u = &a->ui;
  nui_clear(u);
  snprintf(u->badge, sizeof u->badge, "%s",
           a->radio.transmitting          ? "TX"
           : atomic_load(&a->radio.running) ? "RX"
           : a->radio.offline             ? "FILE"
                                          : "IDLE");
  snprintf(u->foot, sizeof u->foot, "TURN SELECT   OK OPEN   BACK RETURN");
  if (a->screen == Home) {
    unsigned start = a->menu < 12 ? 0 : a->menu - 11;
    for (unsigned i = start; i < MENU_COUNT && i < start + 12; i++)
      nui_line(u, i - start, "%c %s%s", a->menu == i ? '>' : ' ', menus[i],
               i == 8 ? (a->logging ? " [ON]" : " [OFF]") : "");
    u->highlight = a->menu - start;
    unsigned active=0;for(unsigned i=0;i<a->engine.count;i++)active+=a->engine.enabled[i];
    nui_line(u, 12, "%u enabled / %u catalog entries",active,(unsigned)a->engine.count);
  } else if (a->screen == Live || a->screen == History) {
    RtlDashboard *v=&u->live;
    v->enabled=true; v->receiving=atomic_load(&a->radio.running);
    v->busy=atomic_load(&a->radio.decoding);
    v->frequency=atomic_load(&a->radio.current_frequency);
    v->rssi=atomic_load(&a->radio.rssi); v->devices=a->count; v->selected=a->selected;
    v->messages=atomic_load(&a->radio.decoded);
    v->drops=atomic_load(&a->radio.edge_drops)+atomic_load(&a->radio.frame_drops)+atomic_load(&a->output_drops);
    v->decode_ms=(atomic_load(&a->radio.decode_us)+999)/1000;
    v->wait_ms=(atomic_load(&a->radio.queue_us)+999)/1000;
    unsigned profile=atomic_load(&a->radio.current_profile);
    snprintf(v->profile,sizeof v->profile,"%s",rtl_profile_name(profile));
    unsigned mode=a->radio.settings.profile, count=rtl_scan_count(mode);
    unsigned scan=0;while(scan<count&&rtl_scan_profile(mode,scan)!=profile)scan++;
    if(mode>=RTL_PROFILE_AUTO)
      snprintf(v->mode,sizeof v->mode,"%s %u/%u",
               atomic_load(&a->radio.scan_holding)?"HOLD":
               mode==RTL_PROFILE_ALL?"ALL":"AUTO",scan+1,count);
    else snprintf(v->mode,sizeof v->mode,"FIXED");
    uint32_t now=furi_get_tick();
    if(v->receiving&&now-a->signal_at>=furi_ms_to_ticks(250)) {
      if(a->signal_count==48)memmove(a->signal,a->signal+1,47);
      else a->signal_count++;
      int value=v->rssi;if(value < -127)value=-127;if(value>20)value=20;
      a->signal[a->signal_count-1]=value;a->signal_at=now;
    }
    v->signal_count=a->signal_count;memcpy(v->signal,a->signal,sizeof v->signal);
    unsigned start = a->selected < 3 ? 0 : a->selected - 2;
    for (unsigned row = 0; row < 3 && start + row < a->count; row++) {
      Record *r = a->records[start + row];
      RtlDeviceCard *card=&v->card[row];
      snprintf(card->name,sizeof card->name,"%.30s",r->model);
      snprintf(card->detail,sizeof card->detail,"ID %.14s CH %.4s  %ddBm",r->id,r->channel,r->meta.rssi);
      card->hits=r->hits;card->rssi=r->meta.rssi;card->selected=a->selected==start+row;v->cards++;
    }
    if(now-u->status_at>furi_ms_to_ticks(3000))
      snprintf(u->status,sizeof u->status,"%s | RAW %u | Q %ums | Bad %u",
             a->radio.settings.hop?"MULTI BAND":"ONE BAND",
             atomic_load(&a->radio.bursts),(unsigned)v->wait_ms,
             atomic_load(&a->radio.invalid));
    snprintf(u->foot, sizeof u->foot, "%s", !v->receiving?
             "TURN SELECT   OK FIELDS   BACK MENU":
             a->radio.settings.profile>=RTL_PROFILE_AUTO?
             "TURN SELECT  OK FIELDS  HOLD OK LOCK  BACK STOP":
             "TURN SELECT  OK FIELDS  HOLD OK AUTO  BACK STOP");
  } else if (a->screen == Detail && a->detail) {
    nui_page(u, a->detail, a->scroll);
    snprintf(u->badge, sizeof u->badge, "%u/%u", a->scroll + 1, a->detail_lines);
    snprintf(u->foot, sizeof u->foot, "TURN SCROLL  HOLD OK EXPORT  BACK LIST");
  } else if (a->screen == Settings) {
    nui_line(u, 0, "%c Frequency: %lu Hz", a->menu == 0 ? '>' : ' ',
             (unsigned long)a->settings.frequency);
    nui_line(u, 2, "%c Fixed profile: %s", a->menu == 1 ? '>' : ' ',
             rtl_profile_name(a->settings.profile));
    nui_line(u, 4, "%c FSK carrier minimum: %d dBm", a->menu == 2 ? '>' : ' ',
             a->settings.threshold);
    nui_line(u, 6, "%c Frequency hopping: %s", a->menu == 3 ? '>' : ' ',
             a->settings.hop ? "ON" : "OFF");
    nui_line(u, 8, "OOK: no software RSSI cutoff.");
    nui_line(u, 9, "AUTO: original OOK/FSK. ALL: six profiles.");
    nui_line(u, 10, "Weak OOK: try 270k; detuned sensors: 650k.");
    nui_line(u, 11, "Hopping: 315 / 433.92 / 868.35 / 915 MHz");
    u->highlight = a->menu * 2;
  } else if (a->screen == Catalog) {
    unsigned start = a->catalog < 10 ? 0 : a->catalog - 9;
    for (unsigned row = 0; row < 11 && start + row < a->engine.count; row++) {
      unsigned i = start + row;
      const r_device *d = rtl_catalog_get(i);
      nui_line(u, row, "%c%03u [%c] %.40s", i == a->catalog ? '>' : ' ', i + 1,
               !rtl_decoder_runnable(a->engine.devices[i]) ? '-'
               : a->engine.enabled[i] ? 'x'
                                      : ' ',
               d->name);
    }
    u->highlight = a->catalog - start;
    const r_device *chosen=a->engine.devices[a->catalog];
    nui_line(u,11,!strcmp(chosen->name,"ANT and ANT+ devices")?
        "2.4 GHz protocol: outside CC1101 RF range":
        "Catalog code does not imply hardware validation.");
    unsigned enabled=0;for(unsigned i=0;i<a->engine.count;i++)enabled+=a->engine.enabled[i];
    nui_line(u, 12, "%s | %s | %u on", rtl_modulation_name(chosen->modulation)?
             rtl_modulation_name(chosen->modulation):"No handler",
             chosen->disabled==3?"Template":chosen->disabled?"Optional":"Standard",enabled);
    snprintf(u->foot, sizeof u->foot, "TURN SELECT OK TOGGLE HOLD OK DEFAULTS BACK SAVE");
  } else if (a->screen == Pulses) {
    RtlFrame *f = malloc(sizeof(*f));
    if (f && rtl_radio_last(&a->radio, f)) {
      nui_line(u, 0, "Last %s burst: %u pairs, %ddBm",
               f->meta.fsk ? "FSK" : "OOK", f->pulses.num_pulses, f->meta.rssi);
      nui_line(u, 1, "INDEX    PULSE us       GAP us");
      for (unsigned row = 0; row < 10 && a->scroll + row < f->pulses.num_pulses;
           row++)
        nui_line(u, row + 2, "%4u     %8d     %8d", a->scroll + row,
                 f->pulses.pulse[a->scroll + row],
                 f->pulses.gap[a->scroll + row]);
    } else
      nui_line(u, 2, "No burst captured yet");
    free(f);
    snprintf(u->foot, sizeof u->foot, "TURN SCROLL  OK SAVE PULSE  BACK MENU");
  } else if (a->screen == Replay && a->replay) {
    const RtlFrame *f = a->replay;
    uint32_t us = 0;
    const char *error = rtl_radio_replay_check(f, &us);
    nui_line(u, 0, "RAW CAPTURE REPLAY - one burst");
    nui_line(u, 2, "Frequency: %lu Hz", (unsigned long)f->meta.frequency);
    nui_line(u, 3, "Modulation: %s", f->meta.fsk ? "2-FSK" : "OOK");
    nui_line(u, 4, "Profile: %s", f->meta.profile_known ?
             rtl_profile_name(f->meta.profile) : "legacy OOK");
    nui_line(u, 5, "Pulse pairs: %u", f->pulses.num_pulses);
    nui_line(u, 6, "Duration: %lu ms", (unsigned long)((us + 999) / 1000));
    nui_line(u, 8, "%s", error ? error : "Recorded timings; values are not editable.");
    nui_line(u, 10, "Receiver remains stopped during replay.");
    nui_line(u, 11, "Radio completion is not a receiver ACK.");
    snprintf(u->foot, sizeof u->foot, "%s", a->radio.transmitting ?
             "TRANSMITTING ONCE   BACK STOP" : "HOLD OK REPLAY ONCE   BACK MENU");
  } else if (a->screen == About) {
    nui_line(u, 0, "RTL433 1.1.5 / T-Embed Native Tools ESP32-S3");
    nui_line(u, 2, "rtl_433_ESP decoder snapshot f0a81569b58a");
    nui_line(u, 3, "377 catalog entries; 333 enabled by default");
    nui_line(u, 4, "376 runnable decoders / 12 coding types");
    nui_line(u, 5, "CC1101 OOK + 2-FSK decode / raw replay.");
    nui_line(u, 6, "Receiver sensitivity and timing need RF testing.");
    nui_line(u, 7, "No SDR I/Q, rtl_tcp, MQTT or Linux CLI.");
    nui_line(u, 9, "History: latest 64 model/ID/channel groups.");
    nui_line(u, 10, "IDs may not uniquely identify physical devices.");
    nui_line(u, 11, "All decoded fields retained in JSON/CSV.");
    nui_line(u, 12, "GPL-3.0-or-later; upstream credits in source.");
  }
}
static void action(App *a, InputEvent *e) {
  int d = nui_delta(e);
  bool ok = e->key == InputKeyOk && e->type == InputTypeShort;
  bool held = e->key == InputKeyOk && e->type == InputTypeLong;
  if (e->key == InputKeyBack) {
    if (a->screen == Home) {
      a->exit = true;
      return;
    }
    if (a->screen == Detail) {
      free(a->detail);
      a->detail = NULL;
      a->screen = History;
      a->scroll = 0;
      return;
    }
    rtl_radio_stop(&a->radio);
    free(a->replay);
    a->replay = NULL;
    drain(a);
    close_log(a);
    if (a->screen == Settings || a->screen == Catalog)
      config(a, true);
    a->screen = Home;
    a->menu = 0;
    return;
  }
  if (a->screen == Home) {
    if (d)
      a->menu = (a->menu + MENU_COUNT + d) % MENU_COUNT;
    if (ok) {
      switch (a->menu) {
      case 0:
        rtl_radio_stop(&a->radio);
        {
        RtlSettings session=rtl_receiver_settings(&a->settings,true);
        if (rtl_radio_start(&a->radio, &session)) {
          a->screen = Live;
          a->signal_count=0;
          a->resume_profile=RTL_PROFILE_AUTO;
          nui_status(&a->ui,"Original OOK/FSK reception; fast decoding");
        } else
          nui_status(&a->ui, "Receiver could not start");
        }
        break;
      case 1:
        rtl_radio_stop(&a->radio);
        {
        RtlSettings session=rtl_receiver_settings(&a->settings,true);
        session.profile=RTL_PROFILE_ALL;
        if (rtl_radio_start(&a->radio,&session)) {
          a->screen=Live; a->signal_count=0; a->resume_profile=RTL_PROFILE_ALL;
          nui_status(&a->ui,"ALL scan: less listening time per profile");
        } else nui_status(&a->ui,"Receiver could not start");
        }
        break;
      case 2:
        a->screen = History;
        break;
      case 3:
        a->screen = Settings;
        a->menu = 0;
        break;
      case 4:
        a->screen = Catalog;
        break;
      case 5:
        a->screen = Pulses;
        a->scroll = 0;
        break;
      case 6:
        open_pulse(a, false);
        break;
      case 7:
        export_history(a);
        break;
      case 8:
        a->logging = !a->logging;
        if (!a->logging)
          close_log(a);
        break;
      case 9:
        rtl_radio_stop(&a->radio);
        {
          RtlFrame *f = malloc(sizeof(*f));
          if (f && rtl_radio_last(&a->radio, f)) preview_replay(a, f);
          else { free(f); nui_status(&a->ui, "No captured burst yet"); }
        }
        break;
      case 10:
        open_pulse(a, true);
        break;
      case 11:
        rtl_radio_stop(&a->radio);
        {
        RtlSettings session=rtl_receiver_settings(&a->settings,false);
        if (rtl_radio_start(&a->radio, &session)) {
          a->screen = Live;
          a->signal_count=0;
          nui_status(&a->ui,"FIXED receiver; HOLD OK resumes scan");
        } else nui_status(&a->ui, "Receiver could not start");
        }
        break;
      case 12:
        a->screen = About;
        break;
      }
    }
  } else if (a->screen == History || a->screen == Live) {
    if(held && atomic_load(&a->radio.running)) {
      RtlSettings session=a->radio.settings;
      session.frequency=atomic_load(&a->radio.current_frequency);
      bool to_auto=session.profile<RTL_PROFILE_AUTO;
      if(!to_auto)a->resume_profile=session.profile;
      session.profile=to_auto?a->resume_profile:atomic_load(&a->radio.current_profile);
      session.hop=to_auto?a->settings.hop:false;
      rtl_radio_stop(&a->radio);
      if(!rtl_radio_start(&a->radio,&session))nui_status(&a->ui,"Receiver could not restart");
      else nui_status(&a->ui,to_auto?"Automatic reception resumed":
                          "FIXED current profile; HOLD OK resumes scan");
      a->signal_count=0;
      return;
    }
    if (d && a->count)
      a->selected = (a->selected + a->count + d) % a->count;
    if (ok && a->count) {
      free(a->detail);
      a->detail = rtl_pretty_json(a->records[a->selected]->json);
      if (a->detail) {
        a->detail_lines = rtl_text_page(a->detail, NUI_COLS, 0, 0, NULL, 0);
        a->screen = Detail;
        a->scroll = 0;
        nui_status(&a->ui, "Record snapshot; turn to see all fields");
      } else nui_status(&a->ui, "Details unavailable; export JSON to SD");
    }
  } else if (a->screen == Detail) {
    if (d) {
      unsigned max = a->detail_lines > NUI_ROWS ? a->detail_lines - NUI_ROWS : 0;
      if (d < 0 && a->scroll)
        a->scroll--;
      if (d > 0 && a->scroll < max)
        a->scroll++;
    }
    if (held)
      export_history(a);
  } else if (a->screen == Replay) {
    if (held && !a->radio.transmitting && a->replay) {
      const char *error = rtl_radio_replay(&a->radio, a->replay);
      nui_status(&a->ui, "%s", error ? error : "Transmitting capture once...");
    }
  } else if (a->screen == Pulses) {
    if (d < 0 && a->scroll)
      a->scroll--;
    if (d > 0 && a->scroll < PD_MAX_PULSES - 1)
      a->scroll++;
    if (ok)
      save_pulse(a);
  } else if (a->screen == Catalog) {
    if (d)
      a->catalog = (a->catalog + a->engine.count + d) % a->engine.count;
    if (ok && rtl_decoder_runnable(a->engine.devices[a->catalog]))
      a->engine.enabled[a->catalog] = !a->engine.enabled[a->catalog];
    if (held) {
      for(unsigned i=0;i<a->engine.count;i++)
        a->engine.enabled[i]=rtl_catalog_get(i)->disabled==0&&rtl_decoder_runnable(a->engine.devices[i]);
      nui_status(&a->ui,"Default decoder selection restored");
    }
  } else if (a->screen == Settings) {
    if (d)
      a->menu = (a->menu + 4 + d) % 4;
    if (ok) {
      if (a->menu == 0) {
        char hz[11];
        snprintf(hz, sizeof hz, "%lu", (unsigned long)a->settings.frequency);
        if (nui_edit(&a->ui, "Frequency in Hz", hz, sizeof hz, "0123456789 ")) {
          char *end;
          unsigned long value = strtoul(hz, &end, 10);
          if (!*end &&
              subghz_devices_is_frequency_valid(a->radio.device, value))
            a->settings.frequency = value;
          else
            nui_status(&a->ui, "Unsupported CC1101 frequency");
        }
      } else if (a->menu == 1)
        a->settings.profile = (a->settings.profile + 1) % 6;
      else if (a->menu == 2) {
        a->settings.threshold += 5;
        if (a->settings.threshold > -20)
          a->settings.threshold = -120;
      } else
        a->settings.hop = !a->settings.hop;
    }
  }
}
int32_t rtl433_app(void *ctx) {
  UNUSED(ctx);
  App *a = calloc(1, sizeof(*a));
  if (!a)
    return -1;
  a->events = furi_message_queue_alloc(8, sizeof(Record *));
  if (!a->events) {
    free(a);
    return -1;
  }
  bool engine = rtl_engine_init(&a->engine, decoded, decoder_yield, a);
  bool radio = engine && rtl_radio_init(&a->radio, &a->engine);
  if (!radio) {
    rtl_radio_free(&a->radio);
    rtl_engine_free(&a->engine);
    furi_message_queue_free(a->events);
    free(a);
    return -1;
  }
  if (!nui_open(&a->ui, "RTL433 1.1.6")) {
    rtl_radio_free(&a->radio);
    rtl_engine_free(&a->engine);
    furi_message_queue_free(a->events);
    free(a);
    return -1;
  }
  a->storage = furi_record_open(RECORD_STORAGE);
  a->log = storage_file_alloc(a->storage);
  a->settings = (RtlSettings){433920000, 1, -90, false};
  a->resume_profile=RTL_PROFILE_AUTO;
  config(a, false);
  r_logger_set_log_handler(decoder_log, a);
  nui_status(&a->ui, "CC1101 sensor receiver / compact display");
  while (!a->exit) {
    InputEvent e;
    if (nui_poll(&a->ui, &e, furi_ms_to_ticks(5))) {
      action(a, &e);
      a->ui.dirty = true;
    }
    drain(a);
    int replay_result = rtl_radio_replay_poll(&a->radio);
    if (replay_result)
      nui_status(&a->ui, replay_result > 0 ? "Replay sent once; radio stopped" :
                 "Replay timeout; radio stopped");
    if (a->radio.offline && atomic_load(&a->radio.decode_done)) {
      rtl_radio_stop(&a->radio);
      drain(a);
      nui_status(&a->ui, "File decoding complete; see history");
    }
    uint32_t elapsed = furi_get_tick() - a->updated;
    if ((a->ui.dirty && elapsed >= furi_ms_to_ticks(40)) ||
        elapsed >= furi_ms_to_ticks(250)) {
      render(a);
      nui_draw(&a->ui);
      a->updated = furi_get_tick();
    }
  }
  rtl_radio_stop(&a->radio);
  drain(a);
  close_log(a);
  config(a, true);
  storage_file_free(a->log);
  furi_record_close(RECORD_STORAGE);
  for (unsigned i = 0; i < a->count; i++)
    release_record(a->records[i]);
  rtl_radio_free(&a->radio);
  r_logger_set_log_handler(NULL, NULL);
  free(a->detail);
  free(a->replay);
  rtl_engine_free(&a->engine);
  furi_message_queue_free(a->events);
  nui_close(&a->ui);
  free(a);
  return 0;
}
