#pragma once

/* QFDIAG — compile-time switch for the qFlipper USB/RPC transfer diagnostics.
 *
 * Enable with CONFIG_FURI_QFDIAG (menuconfig: "qFlipper / RPC diagnostics",
 * or CONFIG_FURI_QFDIAG=y in sdkconfig.defaults.<board>). Default OFF.
 *
 * Diagnostic code is written as `if(QFDIAG_ENABLED) { ... }` rather than
 * #ifdef so it keeps compiling (and cannot rot) in normal builds; with the
 * switch off the compiler drops it entirely. See Kconfig help for what it
 * records and where the log ends up (/ext/qfdiag.log). */

#include <sdkconfig.h>

#ifdef CONFIG_FURI_QFDIAG
#define QFDIAG_ENABLED 1
#else
#define QFDIAG_ENABLED 0
#endif
