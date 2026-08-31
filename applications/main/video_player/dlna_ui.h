#ifndef DLNA_UI_H
#define DLNA_UI_H

/* Self-contained DLNA setup UI for the video player.
 *
 * Same design as the MP3 player's airplay_ui: it runs on the player's plain
 * ViewPort (no ViewDispatcher). The player delegates render/input to it while
 * dlna_ui_is_active() is true. It owns its whole sub-flow:
 *
 *   WiFi scan  ->  password keyboard  ->  connecting  ->  renderer scan  ->  connected
 *
 * Unlike AirPlay there is no local-output alternative (a TV is the only sink),
 * so there is no output-select screen: entering the flow goes straight to the
 * WiFi scan (or, if already connected, to the renderer scan). Choosing a device
 * just records it as the target — the actual streaming is started by the player
 * when the user plays a file. The player reads the result via
 * dlna_ui_is_connected() / dlna_ui_target().
 */

#include <stdbool.h>
#include <stdint.h>
#include <gui/gui.h>
#include <input/input.h>

#include "dlna_ssdp.h"

typedef struct DlnaUi DlnaUi;

DlnaUi* dlna_ui_alloc(void);
void dlna_ui_free(DlnaUi* ui);

/* Open the setup flow: WiFi scan if not connected yet, otherwise the renderer
 * scan. */
void dlna_ui_start_connect(DlnaUi* ui);

/* Drop the chosen target (keeps the WiFi connection up for a fast reconnect). */
void dlna_ui_disconnect_target(DlnaUi* ui);

/* True once a renderer has been chosen. */
bool dlna_ui_is_connected(DlnaUi* ui);

/* True while any DLNA setup screen is on top and should own render/input. */
bool dlna_ui_is_active(DlnaUi* ui);

void dlna_ui_render(Canvas* canvas, DlnaUi* ui);

/* Feed an input event. Returns true if the setup UI stays active, false when
 * the user has left the flow (control returns to the player). */
bool dlna_ui_input(DlnaUi* ui, const InputEvent* event);

/* Periodic poll (drives async WiFi connect + kicks off scans). */
void dlna_ui_tick(DlnaUi* ui);

/* Own STA IP (network byte order), 0 if not connected. */
uint32_t dlna_ui_own_ip(DlnaUi* ui);

/* Chosen renderer, NULL if none. */
const DlnaDevice* dlna_ui_target(DlnaUi* ui);

#endif /* DLNA_UI_H */
