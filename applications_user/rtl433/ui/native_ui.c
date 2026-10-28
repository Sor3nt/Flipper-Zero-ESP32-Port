#include "native_ui.h"
#include "../rtl_text.h"
#include <esp_heap_caps.h>
#include <furi_hal_display.h>
#include <furi_hal_power.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void blank(Canvas *c, void *ctx) {
  UNUSED(ctx);
  canvas_clear(c);
}
static void input(const void *v, void *ctx) {
  NativeUi *u = ctx;
  const InputEvent *e = v;
  if (e->type == InputTypeShort || e->type == InputTypeLong ||
      e->type == InputTypeRepeat)
    furi_message_queue_put(u->input, e, 0);
}
bool nui_open(NativeUi *u, const char *title) {
  memset(u, 0, sizeof(*u));
  u->highlight = -1;
  u->stripe =
      heap_caps_malloc(320 * 8 * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  if (!u->stripe)
    return false;
  u->input = furi_message_queue_alloc(24, sizeof(InputEvent));
  if (!u->input) {
    heap_caps_free(u->stripe);
    return false;
  }
  u->gui = furi_record_open(RECORD_GUI);
  u->pubsub = furi_record_open(RECORD_INPUT_EVENTS);
  u->view = view_port_alloc();
  view_port_draw_callback_set(u->view, blank, u);
  gui_add_view_port(u->gui, u->view, GuiLayerFullscreen);
  gui_direct_draw_acquire(u->gui);
  u->sub = furi_pubsub_subscribe(u->pubsub, input, u);
  snprintf(u->title, sizeof u->title, "%s", title);
  snprintf(u->foot, sizeof u->foot, "TURN SELECT   OK OPEN   BACK RETURN");
  furi_hal_power_insomnia_enter();
  u->dirty = true;
  return true;
}
void nui_suspend(NativeUi *u) {
  furi_pubsub_unsubscribe(u->pubsub, u->sub);
  u->sub = NULL;
  gui_direct_draw_release(u->gui);
}
void nui_resume(NativeUi *u) {
  gui_direct_draw_acquire(u->gui);
  InputEvent e;
  while (nui_poll(u, &e, 0)) {
  }
  u->sub = furi_pubsub_subscribe(u->pubsub, input, u);
  u->dirty = true;
  u->painted_valid = false; /* dialogs may have overwritten the framebuffer */
}
void nui_close(NativeUi *u) {
  nui_suspend(u);
  gui_remove_view_port(u->gui, u->view);
  view_port_free(u->view);
  furi_record_close(RECORD_GUI);
  furi_record_close(RECORD_INPUT_EVENTS);
  furi_message_queue_free(u->input);
  heap_caps_free(u->stripe);
  furi_hal_power_insomnia_exit();
}
void nui_clear(NativeUi *u) {
  memset(u->body, 0, sizeof u->body);
  memset(&u->live, 0, sizeof u->live);
  u->highlight = -1;
  u->dirty = true;
}
void nui_line(NativeUi *u, unsigned row, const char *fmt, ...) {
  if (row >= NUI_ROWS)
    return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(u->body[row], sizeof u->body[row], fmt, ap);
  va_end(ap);
  u->dirty = true;
}
void nui_status(NativeUi *u, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(u->status, sizeof u->status, fmt, ap);
  va_end(ap);
  u->status_at=furi_get_tick();
  u->dirty = true;
}
bool nui_poll(NativeUi *u, InputEvent *e, uint32_t timeout) {
  return furi_message_queue_get(u->input, e, timeout) == FuriStatusOk;
}
int nui_delta(const InputEvent *e) {
  if (e->type == InputTypeLong)
    return 0;
  return e->key == InputKeyUp || e->key == InputKeyLeft      ? -1
         : e->key == InputKeyDown || e->key == InputKeyRight ? 1
                                                             : 0;
}
void nui_page(NativeUi *u, const char *text, unsigned offset) {
  nui_clear(u);
  rtl_text_page(text, NUI_COLS, NUI_ROWS, offset, &u->body[0][0], NUI_COLS + 1);
}
void nui_draw(NativeUi *u) {
  uint32_t now = furi_get_tick();
  if (!u->dirty || now - u->drawn < furi_ms_to_ticks(30))
    return;
  RtlScreen next = {.highlight = u->highlight};
  next.live = u->live;
  memcpy(next.title, u->title, sizeof next.title);
  memcpy(next.badge, u->badge, sizeof next.badge);
  memcpy(next.body, u->body, sizeof next.body);
  memcpy(next.status, u->status, sizeof next.status);
  memcpy(next.foot, u->foot, sizeof next.foot);
  uint32_t changed = rtl_screen_changes(&u->painted, &next, u->painted_valid);
  for (unsigned sy = 0; sy < 170; sy += 8) {
    if (!(changed & (1u << (sy / 8)))) continue;
    rtl_screen_stripe(&next, u->stripe, sy);
    unsigned h = 170 - sy < 8 ? 170 - sy : 8;
    if (!furi_hal_display_draw_rgb565(0, sy, 320, h, u->stripe)) {
      u->painted_valid = false;
      return; /* keep dirty and retry after a failed SPI flush */
    }
  }
  u->painted = next;
  u->painted_valid = true;
  u->drawn = now;
  u->dirty = false;
}
bool nui_edit(NativeUi *u, const char *label, char *value, size_t cap,
              const char *alphabet) {
  if (cap < 2 || cap > 256)
    return false;
  char work[256];
  snprintf(work, cap, "%s", value);
  size_t cursor = 0, alen = strlen(alphabet);
  if (!alen)
    return false;
  for (;;) {
    nui_clear(u);
    nui_line(u, 0, "%s", label);
    size_t view = cursor < 48 ? 0 : cursor - 47;
    nui_line(u, 2, "%.50s", work + view);
    nui_line(u, 3, "%*s^", (int)(cursor - view), "");
    nui_line(u, 4, "Character %u / %u", (unsigned)cursor + 1,
             (unsigned)cap - 1);
    nui_line(u, 6, "TURN change character; OK next character");
    nui_line(u, 8, "HOLD OK save; BACK cancel");
    nui_line(u, 10, "Space at the end removes trailing characters");
    nui_draw(u);
    InputEvent e;
    if (!nui_poll(u, &e, 40))
      continue;
    if (e.key == InputKeyBack)
      return false;
    if (e.key == InputKeyOk && e.type == InputTypeLong) {
      size_t len = strlen(work);
      while (len && work[len - 1] == ' ')
        work[--len] = 0;
      snprintf(value, cap, "%s", work);
      return true;
    }
    if (e.key == InputKeyOk && e.type == InputTypeShort) {
      cursor = (cursor + 1) % (cap - 1);
      while (strlen(work) < cursor)
        strcat(work, " ");
    }
    int d = nui_delta(&e);
    if (d) {
      while (strlen(work) <= cursor)
        strcat(work, " ");
      char *p = strchr(alphabet, work[cursor]);
      size_t at = p ? (size_t)(p - alphabet) : 0;
      work[cursor] = alphabet[(at + alen + d) % alen];
    }
  }
}
