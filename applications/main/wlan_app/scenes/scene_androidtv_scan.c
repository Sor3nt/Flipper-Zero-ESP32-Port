/* Android TV step 1: discover Android TV / Google TV boxes on the LAN.
 *
 * Reuses the already-scanned device list (app->devices from the LAN/attack
 * scan) and TCP-probes port 6466 (the Android TV Remote v2 control port) on
 * each host, keeping the ones that answer. The result list shows the device
 * name (IP fallback) in the shared LAN list view; OK continues to the remote
 * scene, which connects (or routes to pairing if not paired yet). */

#include "../wlan_app.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <esp_log.h>

#define ATV_TAG "WlanAtvScan"
#define ATV_SCAN_PORT 6466
// Generous timeout: an Android TV in WiFi power-save (and/or a cold ARP entry)
// often doesn't answer the first SYN within a few hundred ms — that's why a
// short single-shot probe "misses" the TV on the first scan and finds it on
// the second. Probing a batch concurrently lets every host get the full window
// without making the scan slow (whole batch ~= one timeout, not N x timeout).
#define ATV_CONNECT_TIMEOUT_MS 2000
#define ATV_SCAN_BATCH 10 // concurrent sockets (LWIP_MAX_SOCKETS=16, leave headroom)
#define ATV_SCAN_MAX 32

typedef struct {
    uint32_t ip; // network byte order
    char name[WLAN_APP_HOSTNAME_MAX];
} AtvServer;

static AtvServer s_servers[ATV_SCAN_MAX];
static volatile uint8_t s_count;
static volatile bool s_scanning;
static volatile bool s_scan_complete;
static volatile uint8_t s_progress;
static volatile bool s_cancel;
static pthread_t s_thread;
static bool s_thread_running;
static uint8_t s_rendered;

// Probe port 6466 on a batch of hosts concurrently. Fills open[] (true = the
// host accepted the TCP connection within the timeout). Every host in the
// batch gets the full ATV_CONNECT_TIMEOUT_MS window in parallel.
static void atv_probe_batch(const uint32_t* ips, uint8_t n, bool* open) {
    int fds[ATV_SCAN_BATCH];
    bool pending[ATV_SCAN_BATCH];

    for(uint8_t i = 0; i < n; ++i) {
        open[i] = false;
        pending[i] = false;
        fds[i] = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if(fds[i] < 0) continue;
        int flags = fcntl(fds[i], F_GETFL, 0);
        fcntl(fds[i], F_SETFL, flags | O_NONBLOCK);
        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_port = htons(ATV_SCAN_PORT),
            .sin_addr.s_addr = ips[i],
        };
        int ret = connect(fds[i], (struct sockaddr*)&addr, sizeof(addr));
        if(ret == 0) {
            open[i] = true; // connected immediately
        } else if(errno == EINPROGRESS) {
            pending[i] = true;
        } else {
            close(fds[i]);
            fds[i] = -1;
        }
    }

    // Give the whole batch the full window: repeatedly select over the still-
    // pending sockets until they all resolve or the deadline passes.
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(ATV_CONNECT_TIMEOUT_MS);
    for(;;) {
        fd_set wset;
        FD_ZERO(&wset);
        int maxfd = -1;
        bool any = false;
        for(uint8_t i = 0; i < n; ++i) {
            if(pending[i] && fds[i] >= 0) {
                FD_SET(fds[i], &wset);
                if(fds[i] > maxfd) maxfd = fds[i];
                any = true;
            }
        }
        if(!any || s_cancel) break;
        TickType_t now = xTaskGetTickCount();
        if(now >= deadline) break;
        uint32_t remain_ms = (deadline - now) * portTICK_PERIOD_MS;
        struct timeval tv = {
            .tv_sec = remain_ms / 1000,
            .tv_usec = (remain_ms % 1000) * 1000,
        };
        int r = select(maxfd + 1, NULL, &wset, NULL, &tv);
        if(r <= 0) break; // timeout or error -> remaining stay closed
        for(uint8_t i = 0; i < n; ++i) {
            if(pending[i] && fds[i] >= 0 && FD_ISSET(fds[i], &wset)) {
                int err = 0;
                socklen_t len = sizeof(err);
                getsockopt(fds[i], SOL_SOCKET, SO_ERROR, &err, &len);
                open[i] = (err == 0);
                pending[i] = false;
            }
        }
    }

    for(uint8_t i = 0; i < n; ++i) {
        if(fds[i] >= 0) close(fds[i]);
    }
}

static void* atv_scan_thread(void* arg) {
    WlanApp* app = arg;
    uint16_t total = app->device_count;
    ESP_LOGI(ATV_TAG, "probing 6466 on %u known host(s)", (unsigned)total);
    for(uint16_t base = 0; base < total && !s_cancel; base += ATV_SCAN_BATCH) {
        uint8_t n = (uint8_t)((total - base) < ATV_SCAN_BATCH ? (total - base) : ATV_SCAN_BATCH);
        uint32_t ips[ATV_SCAN_BATCH];
        bool open[ATV_SCAN_BATCH];
        for(uint8_t i = 0; i < n; ++i) ips[i] = app->devices[base + i].ip;

        atv_probe_batch(ips, n, open);

        for(uint8_t i = 0; i < n; ++i) {
            if(open[i] && s_count < ATV_SCAN_MAX) {
                AtvServer* srv = &s_servers[s_count];
                srv->ip = app->devices[base + i].ip;
                strncpy(srv->name, app->devices[base + i].hostname, sizeof(srv->name) - 1);
                srv->name[sizeof(srv->name) - 1] = '\0';
                s_count++;
            }
        }
        uint16_t done = base + n;
        if(total) s_progress = (uint8_t)((done * 100) / total);
    }
    ESP_LOGI(ATV_TAG, "scan done: %u Android TV(s)", (unsigned)s_count);
    s_progress = 100;
    s_scan_complete = true;
    s_scanning = false;
    return NULL;
}

static void atv_scan_format_ip(uint32_t ip, char* out, size_t sz) {
    snprintf(
        out, sz, "%u.%u.%u.%u", (unsigned)(ip & 0xFF), (unsigned)((ip >> 8) & 0xFF),
        (unsigned)((ip >> 16) & 0xFF), (unsigned)((ip >> 24) & 0xFF));
}

static void atv_scan_render(WlanApp* app, bool keep_selection) {
    View* v = app->view_lan;
    uint8_t sel = keep_selection ? wlan_lan_view_get_selected(v) : 0;

    wlan_lan_view_clear(v);
    wlan_lan_view_set_empty_text(v, s_scan_complete ? "No Android TVs" : "Scanning...");

    uint8_t n = s_count;
    for(uint8_t i = 0; i < n; ++i) {
        char ip_buf[20];
        atv_scan_format_ip(s_servers[i].ip, ip_buf, sizeof(ip_buf));
        const char* disp = s_servers[i].name[0] ? s_servers[i].name : ip_buf;
        wlan_lan_view_add_device(v, disp, NULL, NULL, NULL, true, i);
    }
    if(sel < n) wlan_lan_view_set_selected(v, sel);
    s_rendered = n;
}

static void atv_scan_stop_thread(void) {
    s_cancel = true;
    s_scanning = false;
    if(s_thread_running) {
        pthread_join(s_thread, NULL);
        s_thread_running = false;
        s_thread = 0;
    }
}

void wlan_app_scene_androidtv_scan_on_enter(void* context) {
    WlanApp* app = context;

    s_count = 0;
    s_progress = 0;
    s_scan_complete = false;
    s_cancel = false;
    s_rendered = 0;

    View* v = app->view_lan;
    wlan_lan_view_clear_menu(v);
    wlan_lan_view_close_menu(v);
    wlan_lan_view_set_header_title(v, "Android TV");
    wlan_lan_view_set_force_selection_counter(v, false);
    atv_scan_render(app, false);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewLan);

    /* Furi and pthreads use separate TLS slots; keep pthread TLS intact. */
    s_scanning = true;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 6144);
    int rc = pthread_create(&s_thread, &attr, atv_scan_thread, app);
    pthread_attr_destroy(&attr);
    s_thread_running = (rc == 0);
    if(rc != 0) {
        s_scanning = false;
        s_scan_complete = true;
    }
}

bool wlan_app_scene_androidtv_scan_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeTick) {
        char title[24];
        if(s_scan_complete) {
            snprintf(title, sizeof(title), "TVs (%u)", (unsigned)s_count);
        } else {
            snprintf(title, sizeof(title), "TV Scan %u%%", (unsigned)s_progress);
        }
        wlan_lan_view_set_header_title(app->view_lan, title);
        if(s_count != s_rendered || s_scan_complete) {
            atv_scan_render(app, true);
        }
        consumed = true;
    } else if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == WlanAppCustomEventLanItemOk) {
            uint8_t sel = wlan_lan_view_get_selected(app->view_lan);
            WlanLanItem it = wlan_lan_view_get_item(app->view_lan, sel);
            if(it.kind == WlanLanItemKindDevice && it.user_id < s_count) {
                AtvServer* srv = &s_servers[it.user_id];
                atv_scan_format_ip(srv->ip, app->androidtv_ip, sizeof(app->androidtv_ip));
                strncpy(app->androidtv_name, srv->name, sizeof(app->androidtv_name) - 1);
                app->androidtv_name[sizeof(app->androidtv_name) - 1] = '\0';
                atv_scan_stop_thread();
                scene_manager_next_scene(app->scene_manager, WlanAppSceneAndroidTvRemote);
            }
            consumed = true;
        }
    }
    return consumed;
}

void wlan_app_scene_androidtv_scan_on_exit(void* context) {
    WlanApp* app = context;
    atv_scan_stop_thread();
    wlan_lan_view_clear(app->view_lan);
    wlan_lan_view_set_empty_text(app->view_lan, NULL);
}
