#pragma once

#include "mouse_click_recorder.h"

/** Start the selected transport. false = could not start, see app->hid_error. */
bool hid_init(App *app);
void hid_deinit(App *app);
/** Last call before the app exits: retries a USB release that failed (the FAP is unloaded
 *  afterwards, so what is still owned then could never be given back). */
void hid_deinit_final(App *app);
bool hid_ensure_init(App *app);
void hid_mouse_move(App *app, int8_t dx, int8_t dy);
void hid_mouse_press(App *app);
void hid_mouse_release(App *app);
void hid_mouse_click(App *app);
bool hid_is_connected(App *app);
void hid_mouse_press_btn(App *app, uint8_t btn);
void hid_mouse_release_btn(App *app, uint8_t btn);
void hid_mouse_release_all(App *app);
