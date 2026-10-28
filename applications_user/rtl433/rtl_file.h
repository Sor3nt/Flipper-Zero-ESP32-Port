#pragma once
#include "rtl_capture.h"
/* Transparent text format, microseconds, bounded strict parser. */
char *rtl_pulse_text(const RtlFrame *f);
bool rtl_pulse_parse(const char *text, RtlFrame *f);
