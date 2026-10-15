/* Independent native application, inspired by cSploit/dSploit. MIT. */
#include "net_job.h"
#include "ui/native_ui.h"
#include <esp_netif.h>
#include <stdio.h>
#include <stdlib.h>
#include <storage/storage.h>
#include <string.h>
#define ROOT "/ext/apps_data/csploit_mini"
#define LOG_LINES 256
typedef enum { Home, Config, Warning, Logs, About } Screen;
typedef struct {
  NativeUi ui;
  NetConfig config;
  NetJob *job;
  Screen screen;
  JobKind next;
  unsigned selection, head, count, scroll;
  bool exit, follow;
  char lines[LOG_LINES][53];
  uint32_t start, refresh;
} App;
static void append(App *a, const char *text) {
  size_t n = strlen(text);
  for (size_t at = 0; at < n; at += 52) {
    size_t k = n - at > 52 ? 52 : n - at;
    for (size_t i = 0; i < k; i++) {
      unsigned c = (unsigned char)text[at + i];
      a->lines[a->head][i] = c >= 32 && c < 127 ? c : '?';
    }
    a->lines[a->head][k] = 0;
    a->head = (a->head + 1) % LOG_LINES;
    if (a->count < LOG_LINES)
      a->count++;
    if (!a->follow && a->scroll < a->count)
      a->scroll++;
  }
  a->ui.dirty = true;
}
static void log_event(App *a, const char *text) {
  unsigned seconds =
      (furi_get_tick() - a->start) / furi_kernel_get_tick_frequency();
  char line[155];
  snprintf(line, sizeof line, "[%02u:%02u] %s", seconds / 60, seconds % 60,
           text);
  append(a, line);
}
static void start(App *a, JobKind kind) {
  a->head = a->count = a->scroll = 0;
  a->follow = true;
  a->start = furi_get_tick();
  a->job = net_start(kind, &a->config);
  a->screen = Logs;
  if (!a->job)
    append(a, "[ERROR] Could not allocate worker");
}
static void save_log(App *a) {
  Storage *s = furi_record_open(RECORD_STORAGE);
  storage_simply_mkdir(s, "/ext/apps_data");
  storage_simply_mkdir(s, ROOT);
  File *f = storage_file_alloc(s);
  char path[96];
  bool ok = false;
  for (unsigned i = 0; i < 1000; i++) {
    snprintf(path, sizeof path, ROOT "/log-%lu-%u.txt", (unsigned long)a->start,
             i);
    if (!storage_file_exists(s, path)) {
      ok = storage_file_open(f, path, FSAM_WRITE, FSOM_CREATE_NEW);
      break;
    }
  }
  if (ok)
    for (unsigned i = 0; i < a->count; i++) {
      const char *p =
          a->lines[(a->head + LOG_LINES - a->count + i) % LOG_LINES];
      size_t n = strlen(p);
      if (storage_file_write(f, p, n) != n ||
          storage_file_write(f, "\n", 1) != 1) {
        ok = false;
        break;
      }
    }
  if (storage_file_is_open(f)) {
    ok = storage_file_sync(f) && ok;
    storage_file_close(f);
  }
  storage_file_free(f);
  furi_record_close(RECORD_STORAGE);
  nui_status(&a->ui, ok ? "Log saved to apps_data/csploit_mini/"
                        : "Log save failed: check SD card");
}
static void defaults(App *a) {
  a->config = (NetConfig){
      .first_port = 1, .last_port = 1024, .port = 8080, .timeout_ms = 250};
  snprintf(a->config.name, sizeof a->config.name, "lab-test");
  snprintf(a->config.payload, sizeof a->config.payload,
           "Hello from T-Embed Native Tools");
  strcpy(a->config.target, "192.168.1.10");
  strcpy(a->config.reply, "192.168.1.100");
  strcpy(a->config.dns, "192.168.1.1");
  esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_ip_info_t ip;
  if (n && esp_netif_get_ip_info(n, &ip) == 0 && ip.ip.addr) {
    net_ip_text((uint8_t *)&ip.ip.addr, a->config.reply);
    net_ip_text((uint8_t *)&ip.gw.addr, a->config.dns);
  }
}
static void edit_ip(App *a, const char *label, char value[16]) {
  char b[16];
  strcpy(b, value);
  if (nui_edit(&a->ui, label, b, sizeof b, "0123456789. ")) {
    uint8_t ip[4];
    if (net_ip_parse(b, ip) && ip[0] && ip[0] < 224)
      strcpy(value, b);
    else
      nui_status(&a->ui, "Invalid IPv4 address");
  }
}
static void edit_number(App *a, const char *label, unsigned *value,
                        unsigned max) {
  char b[7];
  snprintf(b, sizeof b, "%u", *value);
  if (nui_edit(&a->ui, label, b, sizeof b, "0123456789 ")) {
    char *end;
    unsigned long n = strtoul(b, &end, 10);
    if (n >= 1 && n <= max && !*end)
      *value = n;
    else
      nui_status(&a->ui, "Invalid number");
  }
}
static void render(App *a) {
  NativeUi *u = &a->ui;
  nui_clear(u);
  snprintf(u->badge, sizeof u->badge, "%s",
           a->job ? (atomic_load(&a->job->cancel) ? "STOPPING" : "RUNNING")
                  : "IDLE");
  snprintf(u->foot, sizeof u->foot, "TURN SELECT   OK OPEN   BACK RETURN");
  if (a->screen == Home) {
    unsigned top = a->selection < 11 ? 0 : a->selection - 10;
    for (unsigned row = 0; row < 11 && row + top < 15; row++) {
      unsigned i = row + top;
      const char *name = i < 11    ? net_job_name(i)
                         : i == 11 ? "Configuration"
                         : i == 12 ? "Last terminal log"
                         : i == 13 ? "Save log to SD"
                                   : "About / limits";
      nui_line(u, row, "%c %s", i == a->selection ? '>' : ' ', name);
    }
    u->highlight = a->selection - top;
    nui_line(u, 12, "Target %s / port %u", a->config.target, a->config.port);
  } else if (a->screen == Config) {
    nui_line(u, 0, "%c Target IP: %s", a->selection == 0 ? '>' : ' ',
             a->config.target);
    nui_line(u, 1, "%c Name: %.40s", a->selection == 1 ? '>' : ' ',
             a->config.name);
    nui_line(u, 2, "%c Reply IP: %s", a->selection == 2 ? '>' : ' ',
             a->config.reply);
    nui_line(u, 3, "%c DNS resolver: %s", a->selection == 3 ? '>' : ' ',
             a->config.dns);
    nui_line(u, 4, "%c Scan first port: %u", a->selection == 4 ? '>' : ' ',
             a->config.first_port);
    nui_line(u, 5, "%c Scan last port: %u", a->selection == 5 ? '>' : ' ',
             a->config.last_port);
    nui_line(u, 6, "%c HTTP / terminal / indicator port: %u",
             a->selection == 6 ? '>' : ' ', a->config.port);
    nui_line(u, 7, "%c Scan timeout: %u ms", a->selection == 7 ? '>' : ' ',
             a->config.timeout_ms);
    nui_line(u, 8, "%c Terminal text: %.32s", a->selection == 8 ? '>' : ' ',
             a->config.payload);
    nui_line(u, 9, "%c DNS redirect within ARP relay: %s",
             a->selection == 9 ? '>' : ' ', a->config.redirect ? "ON" : "OFF");
    nui_line(u, 12, "Settings last for this app session.");
    u->highlight = a->selection;
  } else if (a->screen == Warning) {
    nui_line(u, 0, "! CONTROLLED NETWORK TEST");
    nui_line(u, 2, "%s", net_job_name(a->next));
    nui_line(u, 4, "Use only devices/networks you may test.");
    nui_line(u, 5, "ARP/DNS changes can interrupt connections.");
    nui_line(u, 7, "Target: %s", a->config.target);
    nui_line(u, 8, "Name: %.44s", a->config.name);
    nui_line(u, 9, "Reply IP: %s", a->config.reply);
    nui_line(u, 11, "Exposure mode records indicators, not secrets.");
    nui_line(u, 12, "No hardware success guarantee; check counters.");
    snprintf(u->foot, sizeof u->foot, "HOLD OK START    BACK CANCEL");
  } else if (a->screen == Logs) {
    unsigned end = a->count > a->scroll ? a->count - a->scroll : 0,
             start = end > 13 ? end - 13 : 0;
    for (unsigned i = start; i < end; i++)
      nui_line(u, i - start, "%s",
               a->lines[(a->head + LOG_LINES - a->count + i) % LOG_LINES]);
    snprintf(u->foot, sizeof u->foot,
             "TURN SCROLL  OK LIVE  HOLD OK SAVE  BACK STOP");
  } else if (a->screen == About) {
    nui_line(u, 0, "cSploit Mini 1.0 - native C for T-Embed Native Tools");
    nui_line(u, 2, "Independent reimplementation of selected tools.");
    nui_line(u, 3, "Not the Android cSploit/dSploit application.");
    nui_line(u, 5, "LAN discovery, TCP scan, banners, DNS lookup,");
    nui_line(u, 6, "HTTP HEAD, text TCP/UDP, IPv4 ARP relay,");
    nui_line(u, 7, "exact-name DNS/LLMNR/NBT-NS response tests.");
    nui_line(u, 8, "Exposure indicator discards header values.");
    nui_line(u, 10, "No Metasploit, exploits, password/hash capture,");
    nui_line(u, 11, "HTTPS interception, IPv6 relay or Linux tools.");
    nui_line(u, 12, "Packet backend: %s",
             cs_backend() ? "available" : "needs Native Tools FW");
  }
}
static void action(App *a, InputEvent *e) {
  int d = nui_delta(e);
  bool ok = e->key == InputKeyOk && e->type == InputTypeShort,
       held = e->key == InputKeyOk && e->type == InputTypeLong;
  if (e->key == InputKeyBack) {
    if (a->job) {
      net_cancel(a->job);
      nui_status(&a->ui, "Stopping; waiting for cleanup...");
      return;
    }
    if (a->screen == Home)
      a->exit = true;
    else {
      a->screen = Home;
      a->selection = 0;
    }
    return;
  }
  if (a->screen == Home) {
    if (d)
      a->selection = (a->selection + 15 + d) % 15;
    if (ok) {
      if (a->selection < 11) {
        a->next = a->selection;
        if (a->next >= JobArp)
          a->screen = Warning;
        else
          start(a, a->next);
      } else if (a->selection == 11) {
        a->screen = Config;
        a->selection = 0;
      } else if (a->selection == 12)
        a->screen = Logs;
      else if (a->selection == 13)
        save_log(a);
      else
        a->screen = About;
    }
  } else if (a->screen == Warning) {
    if (held)
      start(a, a->next);
  } else if (a->screen == Config) {
    if (d)
      a->selection = (a->selection + 10 + d) % 10;
    if (ok) {
      switch (a->selection) {
      case 0:
        edit_ip(a, "Target IPv4", a->config.target);
        break;
      case 1:
        nui_edit(&a->ui, "Exact hostname (NBNS max 15)", a->config.name,
                 sizeof a->config.name,
                 " abcdefghijklmnopqrstuvwxyz0123456789.-_");
        break;
      case 2:
        edit_ip(a, "DNS response IPv4", a->config.reply);
        break;
      case 3:
        edit_ip(a, "Resolver IPv4 for DNS lookup", a->config.dns);
        break;
      case 4:
        edit_number(a, "First TCP port", &a->config.first_port, 65535);
        if (a->config.last_port < a->config.first_port)
          a->config.last_port = a->config.first_port;
        break;
      case 5:
        edit_number(a, "Last TCP port", &a->config.last_port, 65535);
        if (a->config.first_port > a->config.last_port)
          a->config.first_port = a->config.last_port;
        break;
      case 6:
        edit_number(a, "Service port", &a->config.port, 65535);
        break;
      case 7:
        edit_number(a, "Probe timeout (1..5000 ms)", &a->config.timeout_ms,
                    5000);
        break;
      case 8:
        nui_edit(&a->ui, "Text to send (LF appended)", a->config.payload,
                 sizeof a->config.payload,
                 " abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ01234567"
                 "89.,:/?=&_-+");
        break;
      case 9:
        a->config.redirect = !a->config.redirect;
        break;
      }
    }
  } else if (a->screen == Logs) {
    if (d < 0 && a->scroll + 13 < a->count) {
      a->scroll++;
      a->follow = false;
    }
    if (d > 0 && a->scroll) {
      a->scroll--;
      if (!a->scroll)
        a->follow = true;
    }
    if (ok) {
      a->scroll = 0;
      a->follow = true;
    }
    if (held)
      save_log(a);
  }
}
int32_t csploit_mini_app(void *ctx) {
  UNUSED(ctx);
  App *a = calloc(1, sizeof(*a));
  if (!a)
    return -1;
  if (!nui_open(&a->ui, "cSploit Mini 1.0")) {
    free(a);
    return -1;
  }
  defaults(a);
  nui_status(&a->ui, "Connect through Wi-Fi app before starting");
  while (!a->exit) {
    InputEvent e;
    if (nui_poll(&a->ui, &e, 25))
      action(a, &e);
    if (a->job) {
      NetEvent event;
      while (net_poll(a->job, &event))
        log_event(a, event.text);
      if (atomic_load(&a->job->done)) {
        while (net_poll(a->job, &event))
          log_event(a, event.text);
        net_free(a->job);
        a->job = NULL;
        nui_status(&a->ui, "Complete. HOLD OK saves the terminal log.");
      }
    }
    if (furi_get_tick() - a->refresh >= furi_ms_to_ticks(100)) {
      render(a);
      nui_draw(&a->ui);
      a->refresh = furi_get_tick();
    }
  }
  net_free(a->job);
  nui_close(&a->ui);
  memset(a, 0, sizeof(*a));
  free(a);
  return 0;
}
