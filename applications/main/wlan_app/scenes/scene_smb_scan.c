/* SMB Browser step 1: discover SMB servers on the LAN.
 *
 * Reuses the already-scanned device list (app->devices from the LAN/attack
 * scan) and TCP-probes port 445 on each host, keeping the ones that answer.
 * The result list shows the PC/NetBIOS name (IP fallback) in the shared LAN
 * list view; OK on a server continues to the login scene. */

#include "../wlan_app.h"
#include <wlan_hal.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <esp_log.h>

#define SMB_TAG "WlanSmbScan"
#define SMB_SCAN_PORT 445
#define SMB_CONNECT_TIMEOUT_MS 500
#define SMB_SCAN_MAX_SERVERS 32

typedef struct {
    uint32_t ip; // network byte order
    char name[WLAN_APP_HOSTNAME_MAX];
} SmbServer;

static SmbServer s_servers[SMB_SCAN_MAX_SERVERS];
static volatile uint8_t s_count;
static volatile bool s_scanning;
static volatile bool s_scan_complete;
static volatile uint8_t s_progress;
static volatile bool s_cancel;
static pthread_t s_thread;
static bool s_thread_running;
static uint8_t s_rendered;

static bool smb_probe_445(uint32_t ip_n) {
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(sock < 0) return false;

    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(SMB_SCAN_PORT),
        .sin_addr.s_addr = ip_n,
    };

    bool open = false;
    int ret = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
    if(ret == 0) {
        open = true;
    } else if(errno == EINPROGRESS) {
        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(sock, &wset);
        struct timeval tv = {.tv_sec = 0, .tv_usec = SMB_CONNECT_TIMEOUT_MS * 1000};
        if(select(sock + 1, NULL, &wset, NULL, &tv) > 0) {
            int err = 0;
            socklen_t len = sizeof(err);
            getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len);
            open = (err == 0);
        }
    }
    close(sock);
    return open;
}

static void* smb_scan_thread(void* arg) {
    WlanApp* app = arg;

    // Reuse the already-known device list (from the LAN/attack scan) and just
    // probe port 445 on each host. The hostname (NetBIOS name) is taken from
    // the device record; IP is the fallback for display.
    uint16_t total = app->device_count;
    ESP_LOGI(SMB_TAG, "probing 445 on %u known host(s)", (unsigned)total);
    for(uint16_t i = 0; i < total && !s_cancel; ++i) {
        WlanDeviceRecord* d = &app->devices[i];
        bool open = smb_probe_445(d->ip);
        if(open && s_count < SMB_SCAN_MAX_SERVERS) {
            SmbServer* srv = &s_servers[s_count];
            srv->ip = d->ip;
            strncpy(srv->name, d->hostname, sizeof(srv->name) - 1);
            srv->name[sizeof(srv->name) - 1] = '\0';
            s_count++;
        }
        if(total) s_progress = (uint8_t)(((i + 1) * 100) / total);
    }

    ESP_LOGI(SMB_TAG, "scan done: %u SMB server(s)", (unsigned)s_count);
    s_progress = 100;
    s_scan_complete = true;
    s_scanning = false;
    return NULL;
}

static void smb_scan_format_ip(uint32_t ip, char* out, size_t sz) {
    snprintf(
        out, sz, "%u.%u.%u.%u", (unsigned)(ip & 0xFF), (unsigned)((ip >> 8) & 0xFF),
        (unsigned)((ip >> 16) & 0xFF), (unsigned)((ip >> 24) & 0xFF));
}

static void smb_scan_render(WlanApp* app, bool keep_selection) {
    View* v = app->view_lan;
    uint8_t sel = keep_selection ? wlan_lan_view_get_selected(v) : 0;

    wlan_lan_view_clear(v);
    wlan_lan_view_set_empty_text(v, s_scan_complete ? "No SMB servers" : "Scanning...");

    uint8_t n = s_count;
    for(uint8_t i = 0; i < n; ++i) {
        char ip_buf[20];
        smb_scan_format_ip(s_servers[i].ip, ip_buf, sizeof(ip_buf));
        // Prefer the PC/NetBIOS name; fall back to the IP.
        const char* disp = s_servers[i].name[0] ? s_servers[i].name : ip_buf;
        wlan_lan_view_add_device(v, disp, NULL, NULL, NULL, true, i);
    }
    if(sel < n) wlan_lan_view_set_selected(v, sel);
    s_rendered = n;
}

static void smb_scan_stop_thread(void) {
    s_cancel = true;
    s_scanning = false;
    if(s_thread_running) {
        pthread_join(s_thread, NULL);
        s_thread_running = false;
        s_thread = 0;
    }
}

void wlan_app_scene_smb_scan_on_enter(void* context) {
    WlanApp* app = context;

    s_count = 0;
    s_progress = 0;
    s_scan_complete = false;
    s_cancel = false;
    s_rendered = 0;

    View* v = app->view_lan;
    wlan_lan_view_clear_menu(v);
    wlan_lan_view_close_menu(v);
    wlan_lan_view_set_header_title(v, "SMB Scan");
    wlan_lan_view_set_force_selection_counter(v, false);
    smb_scan_render(app, false);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewLan);

    // FuriThread-TLS reset before spawning the pthread (see port_scanner).
    // Save + restore: leaving the GUI thread's TLS[0] NULL crashes
    // furi_event_loop_run's cleanup on app exit (furi_thread_get_current()
    // returns NULL -> furi_check in furi_thread_set_signal_callback).
    void* saved_tls = pvTaskGetThreadLocalStoragePointer(NULL, 0);
    vTaskSetThreadLocalStoragePointer(NULL, 0, NULL);

    s_scanning = true;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 6144);
    int rc = pthread_create(&s_thread, &attr, smb_scan_thread, app);
    pthread_attr_destroy(&attr);
    vTaskSetThreadLocalStoragePointer(NULL, 0, saved_tls);
    s_thread_running = (rc == 0);
    if(rc != 0) {
        s_scanning = false;
        s_scan_complete = true;
    }
}

bool wlan_app_scene_smb_scan_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeTick) {
        char title[24];
        if(s_scan_complete) {
            snprintf(title, sizeof(title), "SMB (%u)", (unsigned)s_count);
        } else {
            snprintf(title, sizeof(title), "SMB Scan %u%%", (unsigned)s_progress);
        }
        wlan_lan_view_set_header_title(app->view_lan, title);
        if(s_count != s_rendered || s_scan_complete) {
            smb_scan_render(app, true);
        }
        consumed = true;
    } else if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == WlanAppCustomEventLanItemOk) {
            uint8_t sel = wlan_lan_view_get_selected(app->view_lan);
            WlanLanItem it = wlan_lan_view_get_item(app->view_lan, sel);
            if(it.kind == WlanLanItemKindDevice && it.user_id < s_count) {
                SmbServer* srv = &s_servers[it.user_id];
                smb_scan_format_ip(srv->ip, app->smb_server_ip, sizeof(app->smb_server_ip));
                strncpy(app->smb_server_name, srv->name, sizeof(app->smb_server_name) - 1);
                app->smb_server_name[sizeof(app->smb_server_name) - 1] = '\0';
                // Fresh credentials for a freshly-picked server (empty = guest).
                app->smb_user[0] = '\0';
                app->smb_pass[0] = '\0';
                smb_scan_stop_thread();
                scene_manager_set_scene_state(app->scene_manager, WlanAppSceneSmbLogin, 0);
                scene_manager_next_scene(app->scene_manager, WlanAppSceneSmbLogin);
            }
            consumed = true;
        }
    }
    return consumed;
}

void wlan_app_scene_smb_scan_on_exit(void* context) {
    WlanApp* app = context;
    smb_scan_stop_thread();
    wlan_lan_view_clear(app->view_lan);
    wlan_lan_view_set_empty_text(app->view_lan, NULL);
}
