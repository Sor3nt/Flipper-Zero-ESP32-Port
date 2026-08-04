/** Compatibility shim for upstream Flipper external apps.
 *
 * Some upstream apps include FuriString via the short <core/string.h> path
 * (others use <furi/core/string.h> — see ../furi/core/string.h). This ESP32
 * port renamed the header to furi_string.h, so forward to it here.
 *
 * This lives under components/compat/ (not components/furi/core/) on purpose:
 * components/furi/core/ is a direct -I include root, so a string.h placed
 * there would shadow libc's <string.h>. The compat root has no top-level
 * string.h, so only the qualified <core/string.h> resolves here.
 *
 * @file string.h
 */

#pragma once

#include "furi_string.h"
