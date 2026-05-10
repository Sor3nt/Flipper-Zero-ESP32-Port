#include "../wlan_app.h"
#include "../wlan_hal.h"
#include <input/input.h>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>

#define PS_TAG "WlanPortscan"

static const uint16_t SCAN_PORTS[] = {
    21, 22, 23, 25, 53, 80, 110, 135, 139, 143,
    443, 445, 993, 995, 3306, 3389, 5900, 8080, 8443,
};
#define SCAN_PORT_COUNT (sizeof(SCAN_PORTS) / sizeof(SCAN_PORTS[0]))
#define CONNECT_TIMEOUT_MS 500
#define BANNER_RECV_TIMEOUT_MS 600

typedef enum {
    PortScanPhasePicking = 0,
    PortScanPhaseScanning = 1,
} PortScanPhase;

static PortScanPhase s_phase;
static uint32_t s_target_ip;       // Network-Byte-Order
static bool s_came_from_picker;
static bool s_fingerprint_active;

// Worker-State (Thread schreibt, Tick liest in View-Model).
static pthread_t s_scan_thread = 0;
static bool s_thread_running = false;
static volatile bool s_scanning = false;
static volatile bool s_scan_complete = false;
static volatile uint8_t s_progress = 0;
static WlanPortscanEntry s_open_ports[WLAN_PORTSCAN_MAX_OPEN];
static volatile uint8_t s_open_count = 0;

static const char* port_service_name(uint16_t port) {
    switch(port) {
    case 21: return "FTP";
    case 22: return "SSH";
    case 23: return "Telnet";
    case 25: return "SMTP";
    case 53: return "DNS";
    case 80: return "HTTP";
    case 110: return "POP3";
    case 135: return "RPC";
    case 139: return "NetBIOS";
    case 143: return "IMAP";
    case 443: return "HTTPS";
    case 445: return "SMB";
    case 993: return "IMAPS";
    case 995: return "POP3S";
    case 3306: return "MySQL";
    case 3389: return "RDP";
    case 5900: return "VNC";
    case 8080: return "HTTP-Alt";
    case 8443: return "HTTPS-Alt";
    default: return "";
    }
}

// Strippt Trailing-CR/LF und truncated Banner auf out-Buffer.
static void banner_clean(const char* in, int in_len, char* out, size_t out_sz) {
    if(out_sz == 0) return;
    if(in_len < 0) in_len = 0;
    if((size_t)in_len > out_sz - 1) in_len = (int)(out_sz - 1);
    int n = 0;
    for(int i = 0; i < in_len; i++) {
        char c = in[i];
        if(c == '\r' || c == '\n') break;
        if(c < 0x20 || c > 0x7E) c = '.'; // Non-Printable ersetzen
        out[n++] = c;
    }
    out[n] = '\0';
}

// Liest bis zu max_len Bytes vom Socket mit Timeout. Returns Anzahl Bytes
// (>=0) oder -1 bei Fehler.
static int recv_with_timeout(int sock, char* buf, int max_len, int timeout_ms) {
    fd_set rset;
    FD_ZERO(&rset);
    FD_SET(sock, &rset);
    struct timeval tv = {.tv_sec = timeout_ms / 1000,
                         .tv_usec = (timeout_ms % 1000) * 1000};
    int ret = select(sock + 1, &rset, NULL, NULL, &tv);
    if(ret <= 0) return 0; // Timeout / Error
    int n = recv(sock, buf, max_len, 0);
    return n < 0 ? 0 : n;
}

// Versucht ein Banner zu greifen. Erst passive (manche Services schicken
// sofort nach Connect), dann aktive HTTP-HEAD-Probe wenn Port HTTP-typisch.
static void grab_banner(int sock, uint16_t port, char* out, size_t out_sz) {
    if(out_sz == 0) return;
    out[0] = '\0';

    char buf[128];
    int n = recv_with_timeout(sock, buf, sizeof(buf) - 1, BANNER_RECV_TIMEOUT_MS);
    if(n > 0) {
        banner_clean(buf, n, out, out_sz);
        if(out[0]) return;
    }

    // Aktive HTTP-Probe für HTTP-typische Ports. HTTPS lassen wir leer
    // (TLS-Handshake bräuchte Stack-Support).
    if(port == 80 || port == 8080 || port == 8000 || port == 8888) {
        const char* req = "HEAD / HTTP/1.0\r\nHost: scan\r\n\r\n";
        send(sock, req, strlen(req), 0);
        n = recv_with_timeout(sock, buf, sizeof(buf) - 1, BANNER_RECV_TIMEOUT_MS);
        if(n > 0) {
            buf[n] = '\0';
            // "Server:"-Header bevorzugen, sonst Status-Line.
            char* srv = strstr(buf, "Server:");
            if(!srv) srv = strstr(buf, "server:");
            if(srv) {
                srv += 7;
                while(*srv == ' ') srv++;
                int line_len = 0;
                while(srv[line_len] && srv[line_len] != '\r' && srv[line_len] != '\n') {
                    line_len++;
                }
                banner_clean(srv, line_len, out, out_sz);
            } else {
                banner_clean(buf, n, out, out_sz);
            }
        }
    }
}

// Versucht Connect mit Timeout. Wenn open → Banner-Grab. Returns true wenn
// Port offen ist; Banner liegt in out_banner (möglicherweise leer).
static bool probe_port(uint32_t ip_n, uint16_t port, char* out_banner, size_t banner_sz) {
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(sock < 0) return false;

    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = ip_n,
    };

    int ret = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
    bool open = false;
    if(ret == 0) {
        open = true;
    } else if(errno == EINPROGRESS) {
        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(sock, &wset);
        struct timeval tv = {.tv_sec = 0, .tv_usec = CONNECT_TIMEOUT_MS * 1000};
        if(select(sock + 1, NULL, &wset, NULL, &tv) > 0) {
            int err = 0;
            socklen_t len = sizeof(err);
            getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len);
            open = (err == 0);
        }
    }

    if(open) {
        // Blocking-Mode für Banner-Grab.
        fcntl(sock, F_SETFL, flags & ~O_NONBLOCK);
        grab_banner(sock, port, out_banner, banner_sz);
    }
    close(sock);
    return open;
}

static void* portscan_thread(void* arg) {
    (void)arg;
    uint32_t ip = s_target_ip;

    for(size_t i = 0; i < SCAN_PORT_COUNT && s_scanning; i++) {
        s_progress = (uint8_t)((i * 100) / SCAN_PORT_COUNT);

        char banner[64];
        if(probe_port(ip, SCAN_PORTS[i], banner, sizeof(banner))) {
            uint8_t idx = s_open_count;
            if(idx < WLAN_PORTSCAN_MAX_OPEN) {
                WlanPortscanEntry* e = &s_open_ports[idx];
                e->port = SCAN_PORTS[i];
                strncpy(e->service, port_service_name(SCAN_PORTS[i]),
                    sizeof(e->service) - 1);
                e->service[sizeof(e->service) - 1] = '\0';
                strncpy(e->banner, banner, sizeof(e->banner) - 1);
                e->banner[sizeof(e->banner) - 1] = '\0';
                s_open_count = idx + 1;
            }
            ESP_LOGI(PS_TAG, "Port %u OPEN (%s) banner='%s'",
                (unsigned)SCAN_PORTS[i], port_service_name(SCAN_PORTS[i]), banner);
        }
    }

    s_progress = 100;
    s_scan_complete = true;
    s_scanning = false;
    return NULL;
}

static void portscan_show_fingerprint(WlanApp* app, uint8_t idx) {
    WlanPortscanViewModel* model = view_get_model(app->view_portscan);
    WlanPortscanEntry e;
    bool valid = idx < model->count;
    if(valid) e = model->ports[idx];
    view_commit_model(app->view_portscan, false);
    if(!valid) return;

    char header[24];
    snprintf(header, sizeof(header), "Port %u %s",
        (unsigned)e.port, e.service[0] ? e.service : "");

    popup_reset(app->popup);
    popup_set_header(app->popup, header, 64, 8, AlignCenter, AlignTop);
    popup_set_text(app->popup,
        e.banner[0] ? e.banner : "(no banner)",
        64, 36, AlignCenter, AlignCenter);
    popup_set_context(app->popup, app);
    popup_set_callback(app->popup, NULL);
    s_fingerprint_active = true;
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewPopup);
}

static void portscan_close_fingerprint(WlanApp* app) {
    popup_reset(app->popup);
    s_fingerprint_active = false;
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewPortscan);
}

static void portscan_format_ip_short(uint32_t ip, char* out, size_t out_sz) {
    snprintf(out, out_sz, "%u.%u",
        (unsigned)((ip >> 16) & 0xFF), (unsigned)((ip >> 24) & 0xFF));
}

static void portscan_show_picker(WlanApp* app) {
    View* v = app->view_lan;
    wlan_lan_view_clear_menu(v);
    wlan_lan_view_close_menu(v);
    wlan_lan_view_clear(v);
    wlan_lan_view_set_header_title(v, "Pick Target");

    for(uint16_t i = 0; i < app->device_count; ++i) {
        if(!app->devices[i].active) continue;
        char ip_buf[WLAN_LAN_VIEW_LABEL_MAX];
        portscan_format_ip_short(app->devices[i].ip, ip_buf, sizeof(ip_buf));
        const char* host = app->devices[i].hostname[0] ? app->devices[i].hostname : "?";
        wlan_lan_view_add_device(v, ip_buf, host, NULL, NULL, true, i);
    }

    wlan_lan_view_set_selected(v, 0);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewLan);
}

static void portscan_worker_stop(void) {
    s_scanning = false;
    if(s_thread_running) {
        pthread_join(s_scan_thread, NULL);
        s_thread_running = false;
        s_scan_thread = 0;
    }
}

static void portscan_start_scan(WlanApp* app, uint32_t ip) {
    portscan_worker_stop();

    s_phase = PortScanPhaseScanning;
    s_target_ip = ip;
    s_progress = 0;
    s_open_count = 0;
    s_scan_complete = false;
    memset(s_open_ports, 0, sizeof(s_open_ports));

    WlanPortscanViewModel* model = view_get_model(app->view_portscan);
    memset(model, 0, sizeof(*model));
    snprintf(model->target_ip, sizeof(model->target_ip), "%u.%u.%u.%u",
        (unsigned)(ip & 0xFF), (unsigned)((ip >> 8) & 0xFF),
        (unsigned)((ip >> 16) & 0xFF), (unsigned)((ip >> 24) & 0xFF));
    model->scanning = true;
    snprintf(model->status, sizeof(model->status), "Scanning ...");
    view_commit_model(app->view_portscan, true);

    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewPortscan);

    // FuriThread-TLS-Reset (analog wifi_app: pthread_getspecific würde
    // sonst auf garbage-Pointer dereferencen).
    vTaskSetThreadLocalStoragePointer(NULL, 0, NULL);

    s_scanning = true;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 4096);
    int rc = pthread_create(&s_scan_thread, &attr, portscan_thread, app);
    pthread_attr_destroy(&attr);
    if(rc == 0) {
        s_thread_running = true;
    } else {
        ESP_LOGE(PS_TAG, "pthread_create failed: %d", rc);
        s_scan_thread = 0;
        s_thread_running = false;
        s_scanning = false;
        s_scan_complete = true;
    }
}

void wlan_app_scene_port_scanner_on_enter(void* context) {
    WlanApp* app = context;

    uint16_t target_count = 0;
    uint16_t single_idx = 0;
    for(uint16_t i = 0; i < app->device_count; ++i) {
        if(app->devices[i].active) {
            single_idx = i;
            target_count++;
        }
    }

    if(target_count == 1) {
        s_came_from_picker = false;
        portscan_start_scan(app, app->devices[single_idx].ip);
    } else if(target_count == 0) {
        // Fallback: Gateway-IP wenn verbunden, sonst 192.168.1.1.
        s_came_from_picker = false;
        uint32_t gw = wlan_hal_get_gw_ip();
        portscan_start_scan(app, gw ? gw : 0x0101A8C0u);
    } else {
        s_came_from_picker = true;
        s_phase = PortScanPhasePicking;
        portscan_show_picker(app);
    }
}

bool wlan_app_scene_port_scanner_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(s_phase == PortScanPhasePicking) {
        if(event.type == SceneManagerEventTypeCustom &&
           event.event == WlanAppCustomEventLanItemOk) {
            uint8_t sel = wlan_lan_view_get_selected(app->view_lan);
            WlanLanItem it = wlan_lan_view_get_item(app->view_lan, sel);
            if(it.kind == WlanLanItemKindDevice && it.user_id < app->device_count) {
                portscan_start_scan(app, app->devices[it.user_id].ip);
            }
            consumed = true;
        }
        return consumed;
    }

    if(event.type == SceneManagerEventTypeBack) {
        if(s_fingerprint_active) {
            portscan_close_fingerprint(app);
            return true;
        }
        if(s_came_from_picker) {
            portscan_worker_stop();
            s_phase = PortScanPhasePicking;
            portscan_show_picker(app);
            return true;
        }
    }

    if(event.type == SceneManagerEventTypeTick) {
        WlanPortscanViewModel* model = view_get_model(app->view_portscan);

        // Worker-State in View-Model spiegeln.
        model->scanning = !s_scan_complete;
        model->progress = s_progress;
        model->count = s_open_count;
        for(uint8_t i = 0; i < s_open_count && i < WLAN_PORTSCAN_MAX_OPEN; i++) {
            model->ports[i] = s_open_ports[i];
        }
        if(s_scan_complete) {
            snprintf(model->status, sizeof(model->status),
                "Done. %u open.", (unsigned)model->count);
        } else {
            snprintf(model->status, sizeof(model->status),
                "Scanning %u%%...", (unsigned)s_progress);
        }
        view_commit_model(app->view_portscan, true);
    } else if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == WlanAppCustomEventPortscanOk) {
            uint8_t sel = wlan_portscan_view_get_selected(app->view_portscan);
            portscan_show_fingerprint(app, sel);
            consumed = true;
        } else {
            WlanPortscanViewModel* model = view_get_model(app->view_portscan);
            if(event.event == InputKeyUp) {
                if(model->selected > 0) {
                    model->selected--;
                    if(model->selected < model->window_offset) {
                        model->window_offset = model->selected;
                    }
                }
                view_commit_model(app->view_portscan, true);
                consumed = true;
            } else if(event.event == InputKeyDown) {
                if(model->count > 0 && model->selected + 1 < model->count) {
                    model->selected++;
                    if(model->selected >= model->window_offset + WLAN_PORTSCAN_ITEMS_ON_SCREEN) {
                        model->window_offset =
                            model->selected - WLAN_PORTSCAN_ITEMS_ON_SCREEN + 1;
                    }
                }
                view_commit_model(app->view_portscan, true);
                consumed = true;
            } else {
                view_commit_model(app->view_portscan, false);
            }
        }
    }

    return consumed;
}

void wlan_app_scene_port_scanner_on_exit(void* context) {
    WlanApp* app = context;
    portscan_worker_stop();
    wlan_lan_view_set_header_title(app->view_lan, "LAN");
    if(s_fingerprint_active) {
        popup_reset(app->popup);
        s_fingerprint_active = false;
    }
    s_phase = PortScanPhaseScanning;
}
