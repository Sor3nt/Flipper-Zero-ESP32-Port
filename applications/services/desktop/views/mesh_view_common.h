/**
 * Gemeinsame Zeichen-Helfer für die per-Client Mesh-Views (Menü/Device/Wifi/
 * Handshake). Vereinheitlicht den Header (Buddy-Name zentriert + Ch: rechts +
 * Trennlinie) und das zentrale Result-Overlay ("Handshake received").
 */
#pragma once

#include <gui/canvas.h>

/** Header zeichnen: Name zentriert oben, "Ch:<channel>" rechtsbündig (nur wenn
 *  channel 1..13), darunter eine Trennlinie. */
void mesh_view_draw_header(Canvas* canvas, const char* name, uint8_t channel);

/** Zentrales Overlay (gerahmte Box mit Text) mittig auf dem Screen — no-op wenn
 *  text NULL/leer. Am Ende des draw_callback aufrufen, damit es über allem liegt. */
void mesh_view_draw_overlay(Canvas* canvas, const char* text);
