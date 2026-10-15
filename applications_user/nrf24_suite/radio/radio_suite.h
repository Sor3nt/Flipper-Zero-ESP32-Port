#pragma once

#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <input/input.h>

#include "../nrf24tool.h"
#include "../libnrf24/nrf24.h"

/* ------------------------------------------------------------------------- *
 * nRF24 Suite - extra pentest tools (self-contained module)
 *
 *   Spectrum Analyzer / Channel Scanner / WiFi Monitor / BLE Adv Scanner /
 *   Jammer (CW / Flood / Turbo + presets) / Register + Diag
 *
 * Design rules honoured:
 *   - never edits libnrf24/ (missing helpers are implemented locally here)
 *   - never holds the shared SPI bus (the driver acquire/releases per trx)
 *   - radio work runs on a single worker FuriThread so the UI stays live
 *   - the existing RX / TX / Sniffer / BadMouse code is untouched
 * ------------------------------------------------------------------------- */

typedef enum {
    RadioToolSpectrum = 0,
    RadioToolChannelScan,
    RadioToolWifi,
    RadioToolBle,
    RadioToolJammer,
    RadioToolDiag,
    RadioToolMjScan,
    RadioToolReplay,
    RadioToolFuzzer,
    RadioToolRssi,
    RadioToolCount
} RadioTool;

/* Register all Views with the app dispatcher (call once, after badmouse_alloc). */
void radio_suite_alloc(Nrf24Tool* app);

/* Remove/free all Views and stop the worker (call before view_dispatcher_free). */
void radio_suite_free(Nrf24Tool* app);

/* Open a tool screen from app_menu_enter_callback(): checks the radio link,
 * switches to the tool's View and (re)starts its worker thread. */
void radio_suite_enter(Nrf24Tool* app, RadioTool tool);
