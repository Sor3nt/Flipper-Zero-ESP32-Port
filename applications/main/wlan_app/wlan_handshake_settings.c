#include "wlan_handshake_settings.h"
#include <toolbox/saved_struct.h>
#include <string.h>

static void apply_defaults(WlanHandshakeSettings* s) {
    s->channel = 1;
    s->hopping = false;
    memset(s->save_path, 0, sizeof(s->save_path));
    strncpy(s->save_path, WLAN_HS_DEFAULT_SAVE_PATH, sizeof(s->save_path) - 1);
}

void wlan_handshake_settings_load(WlanHandshakeSettings* out) {
    if(!out) return;
    memset(out, 0, sizeof(*out));
    bool ok = saved_struct_load(
        WLAN_HS_SETTINGS_PATH, out, sizeof(*out),
        WLAN_HS_SETTINGS_MAGIC, WLAN_HS_SETTINGS_VERSION);
    if(!ok) apply_defaults(out);

    if(out->channel < 1 || out->channel > 13) out->channel = 1;
    if(out->save_path[0] == '\0') {
        strncpy(out->save_path, WLAN_HS_DEFAULT_SAVE_PATH, sizeof(out->save_path) - 1);
    }
}

void wlan_handshake_settings_save(const WlanHandshakeSettings* in) {
    if(!in) return;
    saved_struct_save(
        WLAN_HS_SETTINGS_PATH, in, sizeof(*in),
        WLAN_HS_SETTINGS_MAGIC, WLAN_HS_SETTINGS_VERSION);
}
