/* Compact RGB565 UI for T-Embed Native Tools / T-Embed. MIT; font attribution in
 * LICENSE.font. */
#pragma once
#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>
#define NUI_ROWS 13
#define NUI_COLS 52
typedef struct {
  Gui *gui;
  ViewPort *view;
  FuriPubSub *pubsub;
  FuriPubSubSubscription *sub;
  FuriMessageQueue *input;
  uint16_t *stripe;
  char title[40], badge[16], body[NUI_ROWS][NUI_COLS + 1], status[NUI_COLS + 1],
      foot[NUI_COLS + 1];
  int highlight;
  uint32_t drawn;
  bool dirty;
} NativeUi;
bool nui_open(NativeUi *u, const char *title);
void nui_close(NativeUi *u);
void nui_draw(NativeUi *u);
bool nui_poll(NativeUi *u, InputEvent *e, uint32_t timeout);
int nui_delta(const InputEvent *e);
void nui_clear(NativeUi *u);
void nui_line(NativeUi *u, unsigned row, const char *fmt, ...);
void nui_status(NativeUi *u, const char *fmt, ...);
void nui_page(NativeUi *u, const char *text, unsigned offset);
bool nui_edit(NativeUi *u, const char *label, char *text, size_t capacity,
              const char *alphabet);
void nui_suspend(NativeUi *u);
void nui_resume(NativeUi *u);
