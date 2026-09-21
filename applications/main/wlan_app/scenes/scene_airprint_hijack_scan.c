/* Printer-Hijack Schritt 1: mDNS-Scan nach _ipp._tcp-Druckern. Zeigt die
 * Treffer (Name + IP) in der geteilten LAN-Listenansicht; OK übernimmt das Ziel
 * und wechselt zur Hijack-Scene. Der Scan läuft in einem pthread (wlan_printhijack_scan
 * blockiert für die Dauer des Timeouts), damit die GUI nicht einfriert. */

#include "../wlan_app.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>

#define PH_SCAN_TIMEOUT_MS 2500

static WlanPrinterEntry s_printers[WLAN_PRINTHIJACK_MAX];
static volatile int s_count;
static volatile bool s_scanning;
static volatile bool s_scan_complete;
static pthread_t s_thread;
static bool s_thread_running;
static int s_rendered;

static void* ph_scan_thread(void* arg) {
    (void)arg;
    int n = wlan_printhijack_scan(s_printers, WLAN_PRINTHIJACK_MAX, PH_SCAN_TIMEOUT_MS);
    s_count = n;
    s_scan_complete = true;
    s_scanning = false;
    return NULL;
}

static void ph_fmt_ip(uint32_t ip, char* out, size_t sz) {
    snprintf(out, sz, "%u.%u.%u.%u", (unsigned)(ip & 0xFF), (unsigned)((ip >> 8) & 0xFF),
             (unsigned)((ip >> 16) & 0xFF), (unsigned)((ip >> 24) & 0xFF));
}

static void ph_scan_render(WlanApp* app, bool keep_selection) {
    View* v = app->view_lan;
    uint8_t sel = keep_selection ? wlan_lan_view_get_selected(v) : 0;

    wlan_lan_view_clear(v);
    wlan_lan_view_set_empty_text(v, s_scan_complete ? "No printers" : "Scanning...");

    int n = s_count;
    for(int i = 0; i < n; ++i) {
        char ip_buf[20];
        ph_fmt_ip(s_printers[i].ip, ip_buf, sizeof(ip_buf));
        const char* disp = s_printers[i].name[0] ? s_printers[i].name : ip_buf;
        wlan_lan_view_add_device(v, disp, ip_buf, NULL, NULL, true, (uint8_t)i);
    }
    if(sel < n) wlan_lan_view_set_selected(v, sel);
    s_rendered = n;
}

static void ph_scan_stop_thread(void) {
    if(s_thread_running) {
        pthread_join(s_thread, NULL);
        s_thread_running = false;
        s_thread = 0;
    }
}

void wlan_app_scene_airprint_hijack_scan_on_enter(void* context) {
    WlanApp* app = context;

    s_count = 0;
    s_scan_complete = false;
    s_rendered = 0;

    View* v = app->view_lan;
    wlan_lan_view_clear_menu(v);
    wlan_lan_view_close_menu(v);
    wlan_lan_view_set_header_title(v, "Printer Scan");
    wlan_lan_view_set_force_selection_counter(v, false);
    ph_scan_render(app, false);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewLan);

    /* FuriThread-TLS-Reset vor pthread_create (siehe androidtv_scan/port_scanner). */
    void* saved_tls = pvTaskGetThreadLocalStoragePointer(NULL, 0);
    vTaskSetThreadLocalStoragePointer(NULL, 0, NULL);

    s_scanning = true;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 6144);
    int rc = pthread_create(&s_thread, &attr, ph_scan_thread, app);
    pthread_attr_destroy(&attr);
    vTaskSetThreadLocalStoragePointer(NULL, 0, saved_tls);
    s_thread_running = (rc == 0);
    if(rc != 0) {
        s_scanning = false;
        s_scan_complete = true;
    }
}

bool wlan_app_scene_airprint_hijack_scan_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeTick) {
        char title[24];
        if(s_scan_complete) {
            snprintf(title, sizeof(title), "Printers (%u)", (unsigned)s_count);
        } else {
            snprintf(title, sizeof(title), "Scanning...");
        }
        wlan_lan_view_set_header_title(app->view_lan, title);
        if(s_count != s_rendered || s_scan_complete) {
            ph_scan_render(app, true);
        }
        consumed = true;
    } else if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == WlanAppCustomEventLanItemOk) {
            uint8_t sel = wlan_lan_view_get_selected(app->view_lan);
            WlanLanItem it = wlan_lan_view_get_item(app->view_lan, sel);
            if(it.kind == WlanLanItemKindDevice && it.user_id < s_count) {
                WlanPrinterEntry* p = &s_printers[it.user_id];
                app->airprint_hijack_ip = p->ip;
                strncpy(app->airprint_hijack_name, p->name, sizeof(app->airprint_hijack_name) - 1);
                app->airprint_hijack_name[sizeof(app->airprint_hijack_name) - 1] = '\0';
                memcpy(app->airprint_hijack_mac, p->mac, 6);
                app->airprint_hijack_has_mac = p->has_mac;
                ph_scan_stop_thread();
                scene_manager_next_scene(app->scene_manager, WlanAppSceneAirPrintHijack);
            }
            consumed = true;
        }
    }
    return consumed;
}

void wlan_app_scene_airprint_hijack_scan_on_exit(void* context) {
    WlanApp* app = context;
    ph_scan_stop_thread();
    wlan_lan_view_clear(app->view_lan);
    wlan_lan_view_set_empty_text(app->view_lan, NULL);
}
