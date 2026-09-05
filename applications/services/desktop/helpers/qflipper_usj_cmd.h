#pragma once

#include <stdbool.h>

/** "qflipper"-Kommando auf der USB-Serial-JTAG-Konsole.
 *
 *  Solange das USB-Composite nicht installiert ist, meldet sich der T-Embed am
 *  Host als USB-Serial-JTAG (303a:1001). qT-Embed schreibt dort die Zeile
 *  "qflipper\n"; der Desktop pollt den RX-FIFO alle 100 ms (Timer) und startet
 *  bei Treffer die qFlipper-Bridge — derselbe Pfad wie "Enable qFlipper" im
 *  Lock-Menue. Damit muss der Nutzer nichts mehr am Geraet drücken.
 *  Boot-Default bleibt USB-Serial-JTAG; nur ein Host mit qT-Embed schaltet um. */

#define QFLIPPER_USJ_COMMAND "qflipper"

/** Liest anliegende Bytes und parst zeilenweise. true genau dann, wenn eine
 *  Zeile "qflipper" vollstaendig empfangen wurde (dann wird ein kurzes Ack auf
 *  den USJ-TX geschrieben). Aus jedem Task-Kontext aufrufbar (nur Register). */
bool qflipper_usj_cmd_poll(void);
