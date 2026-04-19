#include "../wifi_app.h"
#include "../wifi_hal.h"

#include <esp_log.h>
#include <lwip/sockets.h>
#include <lwip/ip4_addr.h>
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
static FuriThread* s_scan_thread = NULL;
static char s_result_buf[512];
static size_t s_result_len = 0;
static volatile bool s_scan_complete = false;
static uint16_t s_current_port = 0;

static bool port_is_open(uint32_t ip, uint16_t port) {
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(sock < 0) return false;

    /* Non-blocking connect + select for timeout */
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

static int32_t portscan_thread(void* arg) {
    WifiApp* app = arg;
    uint32_t ip = app->portscan_target_ip;
    uint8_t* ipb = (uint8_t*)&ip;

    s_result_len = snprintf(s_result_buf, sizeof(s_result_buf),
                            "Target: %d.%d.%d.%d\nScanning %d ports...\n",
                            ipb[0], ipb[1], ipb[2], ipb[3], (int)SCAN_PORT_COUNT);

    int open_count = 0;
    for(size_t i = 0; i < SCAN_PORT_COUNT && s_scanning; i++) {
        s_current_port = SCAN_PORTS[i];

        if(port_is_open(ip, SCAN_PORTS[i])) {
            const char* svc = port_service_name(SCAN_PORTS[i]);
            int n;
            if(svc[0]) {
                n = snprintf(s_result_buf + s_result_len,
                             sizeof(s_result_buf) - s_result_len,
                             "  %d  %s  OPEN\n", SCAN_PORTS[i], svc);
            } else {
                n = snprintf(s_result_buf + s_result_len,
                             sizeof(s_result_buf) - s_result_len,
                             "  %d  OPEN\n", SCAN_PORTS[i]);
            }
            if(n > 0) s_result_len += n;
            open_count++;
            ESP_LOGI(TAG, "Port %d OPEN (%s)", SCAN_PORTS[i], svc);
        }
    }

    if(s_scanning) {
        int n = snprintf(s_result_buf + s_result_len,
                         sizeof(s_result_buf) - s_result_len,
                         "\nDone. %d open port%s.", open_count, open_count == 1 ? "" : "s");
        if(n > 0) s_result_len += n;
    }

    s_scan_complete = true;
    return 0;
}

void wifi_app_scene_portscan_on_enter(void* context) {
    WifiApp* app = context;

    s_scanning = true;
    s_scan_complete = false;
    s_result_len = 0;
    s_current_port = 0;
    memset(s_result_buf, 0, sizeof(s_result_buf));

    uint8_t* ipb = (uint8_t*)&app->portscan_target_ip;
    snprintf(s_result_buf, sizeof(s_result_buf),
             "Target: %d.%d.%d.%d\nStarting scan...\n", ipb[0], ipb[1], ipb[2], ipb[3]);
    s_result_len = strlen(s_result_buf);

    widget_reset(app->widget);
    widget_add_string_multiline_element(
        app->widget, 0, 2, AlignLeft, AlignTop, FontSecondary, s_result_buf);
    view_dispatcher_switch_to_view(app->view_dispatcher, WifiAppViewWidget);

    s_scan_thread = furi_thread_alloc();
    furi_thread_set_name(s_scan_thread, "PortScan");
    furi_thread_set_stack_size(s_scan_thread, 4096);
    furi_thread_set_context(s_scan_thread, app);
    furi_thread_set_callback(s_scan_thread, portscan_thread);
    furi_thread_start(s_scan_thread);
}

bool wifi_app_scene_portscan_on_event(void* context, SceneManagerEvent event) {
    WifiApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeTick) {
        /* Refresh widget with latest scan results */
        widget_reset(app->widget);
        widget_add_string_multiline_element(
            app->widget, 0, 2, AlignLeft, AlignTop, FontSecondary, s_result_buf);
        consumed = true;
    }

    return consumed;
}

void wifi_app_scene_portscan_on_exit(void* context) {
    WifiApp* app = context;

    s_scanning = false;
    if(s_scan_thread) {
        furi_thread_join(s_scan_thread);
        furi_thread_free(s_scan_thread);
        s_scan_thread = NULL;
    }

    widget_reset(app->widget);
}
