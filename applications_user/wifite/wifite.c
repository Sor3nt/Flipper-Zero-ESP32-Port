#include "font.h"
#include "icon.h"
#include <esp_heap_caps.h>
#include <furi.h>
#include <furi_hal_display.h>
#include <furi_hal_power.h>
#include <gui/gui.h>
#include <input/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <storage/storage.h>
#include <string.h>
#include <wifi/wifite.h>

#define LINES 256
#define COLS 52
#define LOG_ROWS 13
#define LOG_LINE_HEIGHT 10
typedef enum { Home, Targets, Actions, Pin, Logs, Details, About } Screen;
typedef struct {
  Gui *gui;
  FuriPubSub *pubsub;
  FuriPubSubSubscription *sub;
  FuriMessageQueue *input;
  ViewPort *view;
  uint16_t *stripe;
  WifiteJob *job;
  Screen screen;
  unsigned choice, target, count, cursor;
  WfTarget targets[WF_TARGET_MAX];
  char lines[LINES][COLS + 1];
  unsigned line_count, head, scroll;
  char pin[9], status[53];
  bool follow, dirty, exit, stopping;
  uint32_t start, draw_at;
} App;
static void input_cb(const void *value, void *ctx) {
  App *a = ctx;
  const InputEvent *e = value;
  if (e->type == InputTypeShort || e->type == InputTypeRepeat ||
      e->type == InputTypeLong)
    furi_message_queue_put(a->input, e, 0);
}
static void blank(Canvas *c, void *ctx) {
  UNUSED(ctx);
  canvas_clear(c);
}
static void append(App *a, const char *text) {
  const unsigned width = COLS;
  size_t n = strlen(text);
  if (!n)
    return;
  for (size_t pos = 0; pos < n; pos += width) {
    char *line = a->lines[a->head];
    size_t z = n - pos;
    if (z > width)
      z = width;
    for (size_t i = 0; i < z; i++) {
      unsigned c = (uint8_t)text[pos + i];
      line[i] = (c >= 32 && c < 127) ? c : '?';
    }
    line[z] = 0;
    a->head = (a->head + 1) % LINES;
    if (a->line_count < LINES)
      a->line_count++;
    if (!a->follow && a->scroll < a->line_count)
      a->scroll++;
  }
  a->dirty = true;
}
static void log_event(App *a, const char *msg) {
  char line[148];
  unsigned secs =
      (furi_get_tick() - a->start) / furi_kernel_get_tick_frequency();
  snprintf(line, sizeof(line), "[%02u:%02u] %s", secs / 60, secs % 60, msg);
  append(a, line);
}
static void status(App *a, const char *msg) {
  snprintf(a->status, sizeof(a->status), "%s", msg);
  a->dirty = true;
}
static void begin(App *a, WifiteMode mode) {
  if (a->job)
    return;
  a->line_count = a->head = a->scroll = 0;
  a->follow = true;
  a->stopping = false;
  a->start = furi_get_tick();
  if (mode == WifiteScan) {
    a->count = 0;
    a->target = 0;
  }
  a->job =
      wifite_start(mode, mode == WifiteScan ? NULL : &a->targets[a->target],
                   mode == WifiteVerifyPin ? a->pin : NULL);
  a->screen = mode == WifiteScan ? Targets : Logs;
  a->dirty = true;
  if (!a->job) {
    a->screen = Logs;
    log_event(a, "[ERR] Could not allocate/start job");
  }
}
static void save(App *a) {
  Storage *s = furi_record_open(RECORD_STORAGE);
  File *f = storage_file_alloc(s);
  char path[96];
  storage_simply_mkdir(s, "/ext/apps_data");
  storage_simply_mkdir(s, "/ext/apps_data/wifite");
  unsigned suffix = 0;
  do {
    snprintf(path, sizeof(path), "/ext/apps_data/wifite/log-%lu-%u.txt",
             (unsigned long)a->start, suffix++);
  } while (storage_file_exists(s, path) && suffix < 1000);
  bool ok =
      suffix < 1000 && storage_file_open(f, path, FSAM_WRITE, FSOM_CREATE_NEW);
  if (ok)
    for (unsigned i = 0; i < a->line_count; i++) {
      const char *p = a->lines[(a->head + LINES - a->line_count + i) % LINES];
      size_t n = strlen(p);
      if (storage_file_write(f, p, n) != n ||
          storage_file_write(f, "\n", 1) != 1) {
        ok = false;
        break;
      }
    }
  if (storage_file_is_open(f)) {
    if (!storage_file_sync(f))
      ok = false;
    storage_file_close(f);
  }
  storage_file_free(f);
  furi_record_close(RECORD_STORAGE);
  status(a, ok ? "Saved in apps_data/wifite/" : "Save failed: check SD card");
}
static void text(App *a, unsigned sy, int x, int y, const char *s,
                 unsigned scale, uint16_t c) {
  pi_text(a->stripe, sy, x, y, s, scale, c);
}
static void clipped(char *out, size_t cap, const char *p, size_t n) {
  if (n >= cap)
    n = cap - 1;
  for (size_t i = 0; i < n; i++) {
    unsigned c = (uint8_t)p[i];
    out[i] = (c >= 32 && c < 127) ? c : '?';
  }
  out[n] = 0;
}
static void draw(App *a) {
  uint16_t green = pi_rgb(0x65ff96), cyan = pi_rgb(0x66d9ff),
           muted = pi_rgb(0x75a890), amber = pi_rgb(0xffcc66);
  for (unsigned sy = 0; sy < 170; sy += 8) {
    memset(a->stripe, 0, 320 * 8 * 2);
    char buf[64];
    for (unsigned y = 0; y < 10; y++)
      for (unsigned x = 0; x < 10; x++)
        if (wifite_icon_rows[y] & (1u << x))
          pi_pixel(a->stripe, sy, 5 + x, 3 + y, green);
    text(a, sy, 20, 5, "WIFITE", 1, green);
    text(a, sy, 68, 5, "0.8.1 EXPERIMENTAL", 1, amber);
    text(a, sy, 244, 5,
         a->job ? (a->stopping ? "STOPPING" : "RUNNING") : "IDLE", 1, cyan);
    for (int x = 4; x < 316; x++)
      pi_pixel(a->stripe, sy, x, 17, pi_rgb(0x19432b));
    const char *foot = "TURN SELECT   OK OPEN   BACK EXIT";
    if (a->screen == Home) {
      const char *items[] = {"Scan networks",   "Router table",
                             "Last terminal log",
                             "Save last log to SD", "About / limits"};
      for (unsigned i = 0; i < 5; i++) {
        snprintf(buf, sizeof(buf), "%c %s", i == a->choice ? '>' : ' ',
                 items[i]);
        text(a, sy, 4, 24 + i * 17, buf, 1, i == a->choice ? cyan : green);
      }
      text(a, sy, 4, 133, a->status, 1, amber);
    } else if (a->screen == Targets) {
      if (!a->count)
        text(a, sy, 4, 43, "No APs. Run a scan first.", 1, amber);
      text(a, sy, 14, 24, "NETWORK", 1, muted);
      text(a, sy, 126, 24, "WPS", 1, muted);
      text(a, sy, 156, 24, "LOCK", 1, muted);
      text(a, sy, 210, 24, "CLI", 1, muted);
      text(a, sy, 240, 24, "CH", 1, muted);
      text(a, sy, 270, 24, "dBm", 1, muted);
      unsigned start = a->target >= 6 ? a->target - 5 : 0;
      for (unsigned row = 0; row < 6 && start + row < a->count; row++) {
        unsigned i = start + row;
        WfTarget *t = &a->targets[i];
        unsigned y = 42 + row * 18;
        char name[19];
        clipped(name, sizeof(name), t->ssid, t->ssid_len);
        text(a, sy, 4, y, i == a->target ? ">" : " ", 1, cyan);
        text(a, sy, 14, y, name[0] ? name : "<hidden>", 1,
             i == a->target ? cyan : green);
        text(a, sy, 126, y, t->wps ? "YES" : "--", 1, t->wps ? cyan : muted);
        text(a, sy, 156, y, t->locked == 1 ? "LOCK" : t->locked == 0 ? "CLEAR" : "?",
             1, t->locked == 1 ? amber : green);
        snprintf(buf, sizeof(buf), "%u", t->clients);
        text(a, sy, 210, y, buf, 1, t->clients ? cyan : muted);
        snprintf(buf, sizeof(buf), "%u", t->channel);
        text(a, sy, 240, y, buf, 1, green);
        snprintf(buf, sizeof(buf), "%d", t->rssi);
        text(a, sy, 270, y, buf, 1, green);
      }
      foot = a->job ? "SCANNING  CLI=SEEN CLIENTS  BACK STOP"
                    : "CLI=SEEN CLIENTS  HOLD OK DETAILS";
    } else if (a->screen == Actions) {
      WfTarget *t = &a->targets[a->target];
      char name[33];
      clipped(name, sizeof(name), t->ssid, t->ssid_len);
      text(a, sy, 4, 24, name, 1, cyan);
      snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X CH%u",
               t->bssid[0], t->bssid[1], t->bssid[2], t->bssid[3], t->bssid[4],
               t->bssid[5], t->channel);
      text(a, sy, 4, 40, buf, 1, muted);
      const char *items[] = {"Auto test", "Test one PIN"};
      for (unsigned i = 0; i < 2; i++) {
        snprintf(buf, sizeof(buf), "%c %s", i == a->choice ? '>' : ' ',
                 items[i]);
        text(a, sy, 4, 62 + i * 23, buf, 1, i == a->choice ? cyan : green);
      }
      text(a, sy, 4, 116, "Auto: 12345670 -> Pixie -> PIN search", 1, muted);
      text(a, sy, 4, 134, "Stops when successful. Checks AP lock.", 1, muted);

    } else if (a->screen == Pin) {
      text(a, sy, 4, 25, "VERIFY ONE WPS PIN", 1, cyan);
      text(a, sy, 30, 55, a->pin, 1, green);
      for (int x = 0; x < 5; x++)
        pi_pixel(a->stripe, sy, 30 + 6 * a->cursor + x, 64, cyan);
      text(a, sy, 4, 90, "Turn changes digit. OK next digit.", 1, muted);
      text(a, sy, 4, 106, "Hold OK starts this one PIN test.", 1, amber);
      foot = "BACK CANCEL   HOLD OK START";
    } else if (a->screen == Logs) {
      unsigned available =
          a->line_count > a->scroll ? a->line_count - a->scroll : 0;
      unsigned show = available < LOG_ROWS ? available : LOG_ROWS;
      unsigned first = (a->head + LINES - a->scroll - show) % LINES;
      for (unsigned i = 0; i < show; i++) {
        const char *line = a->lines[(first + i) % LINES];
        uint16_t color = strstr(line, "[WPS+]") ? cyan : green;
        if (strstr(line, "[LOCKED]") || strstr(line, "[WAIT]") || strstr(line, "[STOP]")) color = amber;
        text(a, sy, 4, 22 + i * LOG_LINE_HEIGHT, line, 1, color);
      }
      foot = a->job ? "TURN SCROLL  OK LIVE  BACK STOP"
                    : "TURN SCROLL  OK LIVE  BACK MENU";
    } else if (a->screen == Details) {
      WfTarget *t = &a->targets[a->target];
      snprintf(buf, sizeof(buf), "WPS: %s  version: 0x%02X",
               t->wps ? "advertised" : "not seen", t->version);
      text(a, sy, 4, 25, buf, 1, green);
      snprintf(buf, sizeof(buf), "AP PIN lock: %s",
               t->locked == 1   ? "advertised"
               : t->locked == 0 ? "not locked"
                                : "unknown");
      text(a, sy, 4, 43, buf, 1, green);
      snprintf(buf, sizeof(buf), "Clients seen: %u  Methods: 0x%04X", t->clients, t->methods);
      text(a, sy, 4, 61, buf, 1, green);
      snprintf(buf, sizeof(buf), "Vendor: %s",
               t->manufacturer[0] ? t->manufacturer : "not advertised");
      text(a, sy, 4, 79, buf, 1, green);
      snprintf(buf, sizeof(buf), "Model: %s",
               t->model[0] ? t->model : "not advertised");
      text(a, sy, 4, 97, buf, 1, green);
      text(a, sy, 4, 121, "-- = WPS not advertised, not proof off.", 1, muted);
      text(a, sy, 4, 137, "Zero clients seen does not mean none.", 1, amber);
    } else {
      const char *lines[] = {"Standalone ESP32-S3. No Raspberry Pi.",
                             "Native WPS registrar + terminal logs.",
                             "Auto: 12345670, Pixie, PIN search.",
                             "Router table: WPS / PIN lock / clients.",
                             "Clients are passive observations.",
                             "AP PIN lock: passive wait up to 30s.",
                             "This build needs hardware testing.",
                             "NO MATCH does not prove security."};
      for (unsigned i = 0; i < 8; i++)
        text(a, sy, 4, 25 + i * 15, lines[i], 1, i >= 6 ? amber : green);
    }
    for (int x = 4; x < 316; x++)
      pi_pixel(a->stripe, sy, x, 155, pi_rgb(0x19432b));
    text(a, sy, 4, 160, foot, 1, muted);
    unsigned h = 170 - sy;
    if (h > 8)
      h = 8;
    if (!furi_hal_display_draw_rgb565(0, sy, 320, h, a->stripe))
      break;
  }
  a->dirty = false;
  a->draw_at = furi_get_tick();
}
static void action(App *a, const InputEvent *e) {
  if (e->key == InputKeyBack) {
    if (a->job) {
      wifite_cancel(a->job);
      a->stopping = true;
      if (a->screen != Targets)
        a->screen = Logs;
    } else if (a->screen == Home)
      a->exit = true;
    else if (a->screen == Details)
      a->screen = Targets;
    else if (a->screen == Pin) {
      a->screen = Actions;
      a->choice = 0;
    } else if (a->screen == Actions)
      a->screen = Targets;
    else {
      a->screen = Home;
      a->choice = 0;
    }
    a->dirty = true;
    return;
  }
  int d = (e->key == InputKeyDown || e->key == InputKeyRight) ? 1
          : (e->key == InputKeyUp || e->key == InputKeyLeft)  ? -1
                                                              : 0;
  if (a->screen == Logs) {
    unsigned max = a->line_count > LOG_ROWS ? a->line_count - LOG_ROWS : 0;
    if (d < 0) {
      if (a->scroll < max)
        a->scroll++;
      a->follow = false;
    } else if (d > 0) {
      if (a->scroll)
        a->scroll--;
      if (!a->scroll)
        a->follow = true;
    }
    if (e->key == InputKeyOk) {
      a->follow = true;
      a->scroll = 0;
    }
    a->dirty = true;
    return;
  }
  if (a->screen == Pin) {
    if (d)
      a->pin[a->cursor] = '0' + (a->pin[a->cursor] - '0' + d + 10) % 10;
    if (e->key == InputKeyOk) {
      if (e->type == InputTypeLong)
        begin(a, WifiteVerifyPin);
      else if (e->type == InputTypeShort)
        a->cursor = (a->cursor + 1) % 8;
    }
    a->dirty = true;
    return;
  }
  unsigned max = a->screen == Home      ? 5
                 : a->screen == Actions ? 2
                 : a->screen == Targets ? a->count
                                        : 0;
  unsigned *which = a->screen == Targets ? &a->target : &a->choice;
  if (d && max) {
    *which = (*which + max + d) % max;
    a->dirty = true;
  }
  if (a->screen == Targets && a->job && e->key == InputKeyOk)
    return;
  if (a->screen == Targets && a->count && e->key == InputKeyOk &&
      e->type == InputTypeLong) {
    a->screen = Details;
    a->dirty = true;
    return;
  }
  if (e->key != InputKeyOk || e->type != InputTypeShort)
    return;
  if (a->screen == Home) {
    if (a->choice == 0)
      begin(a, WifiteScan);
    else if (a->choice == 1) {
      a->screen = Targets;
      a->target = 0;
    } else if (a->choice == 2)
      a->screen = Logs;
    else if (a->choice == 3)
      save(a);
    else
      a->screen = About;
  } else if (a->screen == Targets && a->count) {
    a->screen = a->targets[a->target].wps ? Actions : Details;
    a->choice = 0;
  } else if (a->screen == Actions) {
    if (a->choice == 0)
      begin(a, WifiteAuto);
    else if (a->choice == 1) {
      a->screen = Pin;
      a->cursor = 0;
      memcpy(a->pin, "12345670", 9);
    }
  }
  a->dirty = true;
}
static void process_event(App *a, const WifiteEvent *e) {
  if (e->type != WifiteTarget) {
    log_event(a, e->text);
    return;
  }
  unsigned i = 0;
  while (i < a->count && memcmp(a->targets[i].bssid, e->target.bssid, 6))
    i++;
  if (i >= WF_TARGET_MAX)
    return;
  if (i == a->count) {
    a->count++;
    char line[80], name[33];
    clipped(name, sizeof(name), e->target.ssid, e->target.ssid_len);
    snprintf(line, sizeof(line), "%s CH%u %ddBm %c %s",
             e->target.wps ? "[WPS+]" : "[AP]", e->target.channel, e->target.rssi,
             e->target.locked == 1 ? 'L' : e->target.locked == 0 ? 'U' : '?',
             name[0] ? name : "<hidden>");
    log_event(a, line);
  }
  a->targets[i] = e->target;
  a->dirty = true;
}
int32_t wifite_app(void *ctx) {
  UNUSED(ctx);
  App *a = calloc(1, sizeof(*a));
  if (!a)
    return -1;
  a->stripe =
      heap_caps_malloc(320 * 8 * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  if (!a->stripe) {
    free(a);
    return -1;
  }
  a->input = furi_message_queue_alloc(16, sizeof(InputEvent));
  a->gui = furi_record_open(RECORD_GUI);
  a->pubsub = furi_record_open(RECORD_INPUT_EVENTS);
  a->view = view_port_alloc();
  view_port_draw_callback_set(a->view, blank, a);
  gui_add_view_port(a->gui, a->view, GuiLayerFullscreen);
  gui_direct_draw_acquire(a->gui);
  a->sub = furi_pubsub_subscribe(a->pubsub, input_cb, a);
  a->follow = true;
  a->dirty = true;
  status(a, "Select one network after scanning.");
  furi_hal_power_insomnia_enter();
  while (!a->exit) {
    InputEvent in;
    if (furi_message_queue_get(a->input, &in, 25) == FuriStatusOk)
      action(a, &in);
    if (a->job) {
      WifiteEvent e;
      while (wifite_poll(a->job, &e))
        process_event(a, &e);
      if (wifite_finished(a->job)) { /* Drain after completion to avoid racing
                                        the final line. */
        while (wifite_poll(a->job, &e))
          process_event(a, &e);
        wifite_free(a->job);
        a->job = NULL;
        a->stopping = false;
        a->dirty = true;
      }
    }
    if (a->dirty && furi_get_tick() - a->draw_at >= 80)
      draw(a);
  }
  furi_pubsub_unsubscribe(a->pubsub, a->sub);
  furi_hal_power_insomnia_exit();
  gui_direct_draw_release(a->gui);
  gui_remove_view_port(a->gui, a->view);
  view_port_free(a->view);
  furi_record_close(RECORD_INPUT_EVENTS);
  furi_record_close(RECORD_GUI);
  furi_message_queue_free(a->input);
  heap_caps_free(a->stripe);
  memset(a, 0, sizeof(*a));
  free(a);
  return 0;
}
