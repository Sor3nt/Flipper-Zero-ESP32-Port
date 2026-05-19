#pragma once
#include <stdbool.h>
typedef struct WlanApp WlanApp;

void wlan_evil_portal_bridge_start(WlanApp* app, const char* real_ssid, const char* password);
void wlan_evil_portal_bridge_stop(void);
bool wlan_evil_portal_bridge_is_active(void);
// Set the fallback creds used when an authentication attempt fails. Empty
// strings disable fallback. Should be called before bridge_start.
void wlan_evil_portal_bridge_set_fallback(const char* ssid, const char* password);
// Pre-warm: bring STA up and acquire upstream IP NOW (when portal starts),
// leaving NAPT and DNS-forward disabled until bridge_start is called on
// cred capture. Eliminates the 5-10s post-cred bridge bringup latency.
void wlan_evil_portal_bridge_prewarm(WlanApp* app, const char* real_ssid, const char* password);
