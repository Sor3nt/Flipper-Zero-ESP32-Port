#include "../wifi_app.h"
#include "../wifi_hal.h"
#include "../views/portscan_view.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <input/input.h>
#include <lwip/sockets.h>
#include <lwip/ip4_addr.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>

#define TAG WIFI_APP_LOG_TAG

static const uint16_t SCAN_PORTS[] = {
    21, 22, 23, 25, 53, 80, 110, 135, 139, 143,
    443, 445, 993, 995, 3306, 3389, 5900, 8080, 8443
};
#define SCAN_PORT_COUNT (sizeof(SCAN_PORTS) / sizeof(SCAN_PORTS[0]))
#define CONNECT_TIMEOUT_MS 500

static volatile bool s_scanning = false;
static volatile bool s_scan_complete = false;
static pthread_t s_scan_thread = 0;
static bool s_thread_running = false;
static uint8_t s_progress = 0;

static PortscanEntry s_open_ports[PORTSCAN_MAX_OPEN];
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

static bool port_is_open(uint32_t ip, uint16_t port) {
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(sock < 0) return false;

    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = ip,
    };

    int ret = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
    if(ret == 0) {
        close(sock);
        return true;
    }

    if(errno != EINPROGRESS) {
        close(sock);
        return false;
    }

    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(sock, &wset);
    struct timeval tv = {
        .tv_sec = 0,
        .tv_usec = CONNECT_TIMEOUT_MS * 1000,
    };

    ret = select(sock + 1, NULL, &wset, NULL, &tv);
    if(ret > 0) {
        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len);
        close(sock);
        return err == 0;
    }

    close(sock);
    return false;
}

static void* portscan_thread(void* arg) {
    WifiApp* app = arg;
    uint32_t ip = app->portscan_target_ip;

    for(size_t i = 0; i < SCAN_PORT_COUNT && s_scanning; i++) {
        s_progress = (uint8_t)((i * 100) / SCAN_PORT_COUNT);

        if(port_is_open(ip, SCAN_PORTS[i])) {
            if(s_open_count < PORTSCAN_MAX_OPEN) {
                PortscanEntry* e = &s_open_ports[s_open_count];
                e->port = SCAN_PORTS[i];
                strncpy(e->service, port_service_name(SCAN_PORTS[i]), sizeof(e->service) - 1);
                e->service[sizeof(e->service) - 1] = '\0';
                s_open_count++;
            }
            ESP_LOGI(TAG, "Port %d OPEN (%s)", SCAN_PORTS[i],
                     port_service_name(SCAN_PORTS[i]));
        }
    }

    s_progress = 100;
    s_scan_complete = true;
    return NULL;
}

void wifi_app_scene_portscan_on_enter(void* context) {
    WifiApp* app = context;
    uint8_t* ipb = (uint8_t*)&app->portscan_target_ip;
    ESP_LOGI(TAG, "portscan_on_enter: target=%d.%d.%d.%d",
             ipb[0], ipb[1], ipb[2], ipb[3]);

    s_scanning = true;
    s_scan_complete = false;
    s_progress = 0;
    s_open_count = 0;
    memset(s_open_ports, 0, sizeof(s_open_ports));

    PortscanViewModel* model = view_get_model(app->view_portscan);
    memset(model, 0, sizeof(PortscanViewModel));
    snprintf(model->target_ip, sizeof(model->target_ip), "%d.%d.%d.%d",
             ipb[0], ipb[1], ipb[2], ipb[3]);
    model->scanning = true;
    snprintf(model->status, sizeof(model->status), "Scanning...");
    view_commit_model(app->view_portscan, true);

    view_dispatcher_switch_to_view(app->view_dispatcher, WifiAppViewPortscan);

    /* FuriThread tasks have garbage in PTHREAD TLS slot — must zero before
     * pthread_create or pthread_getspecific dereferences invalid memory. */
    vTaskSetThreadLocalStoragePointer(NULL, 0, NULL);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 4096);
    int rc = pthread_create(&s_scan_thread, &attr, portscan_thread, app);
    if(rc == 0) {
        s_thread_running = true;
    } else {
        ESP_LOGE(TAG, "pthread_create failed: %d", rc);
        s_scan_thread = 0;
        s_thread_running = false;
        s_scan_complete = true;
    }
    pthread_attr_destroy(&attr);
}

bool wifi_app_scene_portscan_on_event(void* context, SceneManagerEvent event) {
    WifiApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        PortscanViewModel* model = view_get_model(app->view_portscan);
        if(event.event == InputKeyUp) {
            if(model->selected > 0) {
                model->selected--;
                if(model->selected < model->window_offset)
                    model->window_offset = model->selected;
            }
            view_commit_model(app->view_portscan, true);
            consumed = true;
        } else if(event.event == InputKeyDown) {
            if(model->count > 0 && model->selected < model->count - 1) {
                model->selected++;
                if(model->selected >= model->window_offset + PORTSCAN_ITEMS_ON_SCREEN)
                    model->window_offset = model->selected - PORTSCAN_ITEMS_ON_SCREEN + 1;
            }
            view_commit_model(app->view_portscan, true);
            consumed = true;
        } else {
            view_commit_model(app->view_portscan, false);
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        PortscanViewModel* model = view_get_model(app->view_portscan);
        model->scanning = !s_scan_complete;
        model->progress = s_progress;
        model->count = s_open_count;
        for(int i = 0; i < s_open_count && i < PORTSCAN_MAX_OPEN; i++) {
            model->ports[i] = s_open_ports[i];
        }
        if(s_scan_complete) {
            snprintf(model->status, sizeof(model->status),
                     "Done. %d open.", s_open_count);
        } else {
            snprintf(model->status, sizeof(model->status),
                     "Scanning %d%%...", s_progress);
        }
        view_commit_model(app->view_portscan, true);
    }

    return consumed;
}

void wifi_app_scene_portscan_on_exit(void* context) {
    UNUSED(context);

    s_scanning = false;
    if(s_thread_running) {
        pthread_join(s_scan_thread, NULL);
        s_thread_running = false;
        s_scan_thread = 0;
    }
}
