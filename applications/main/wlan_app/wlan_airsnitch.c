#include "wlan_airsnitch.h"
#include <wlan_hal.h>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <furi.h>

#define TAG "WlanAirSnitch"

// Interner Ergebnis-Puffer der Engine. Muss <= WLAN_AIRSNITCH_MAX_PEERS (wlan_app.h)
// sein; die Scene mergt/kappt ohnehin auf ihre eigene Grenze.
#define AS_MAX_RESULTS 48
// Parallel offene Sockets pro Batch. LwIP-Limit ist CONFIG_LWIP_MAX_SOCKETS=16;
// wir lassen Headroom für andere Subsysteme.
#define AS_CONCURRENCY 10
#define AS_CONNECT_TIMEOUT_MS 300

// Gängige Home-Router-/24-Netze (Host-Order-Basis, low byte = 0).
static const uint32_t AS_CANDIDATE_NETS[] = {
    0xC0A80000u, // 192.168.0.0
    0xC0A80100u, // 192.168.1.0
    0xC0A80200u, // 192.168.2.0
    0xC0A8B200u, // 192.168.178.0 (FritzBox Privat)
    0xC0A8B300u, // 192.168.179.0 (FritzBox Gast)
    0x0A000000u, // 10.0.0.0
    0x0A000100u, // 10.0.1.0
    0xAC100000u, // 172.16.0.0
};
#define AS_CANDIDATE_COUNT (sizeof(AS_CANDIDATE_NETS) / sizeof(AS_CANDIDATE_NETS[0]))

// Liveness-Ports. Ein SYN-ACK (offen) ODER RST (geschlossen, aber Host lebt &
// wurde geroutet) zählt als "erreichbar". Deckt Web/IoT/Router (80/443),
// Windows/NAS (445) und Linux/SSH (22) ab.
static const uint16_t AS_PORTS[] = {80, 443, 445, 22};
#define AS_PORT_COUNT (sizeof(AS_PORTS) / sizeof(AS_PORTS[0]))

// Gate-Hosts: entscheidet, ob ein Subnetz überhaupt routbar ist, bevor wir es
// voll durchscannen (hält den "sauber isoliert"-Fall schnell).
static const uint8_t AS_GATE_HOSTS[] = {1, 100, 254};
#define AS_GATE_COUNT (sizeof(AS_GATE_HOSTS) / sizeof(AS_GATE_HOSTS[0]))
static const uint16_t AS_GATE_PORTS[] = {80, 443, 445};
#define AS_GATE_PORT_COUNT (sizeof(AS_GATE_PORTS) / sizeof(AS_GATE_PORTS[0]))

typedef struct {
    uint32_t ip_be;
    uint16_t port;
} AsJob;

static volatile bool s_running = false;
static volatile bool s_stop = false;
static volatile bool s_done = false;
static volatile uint8_t s_progress = 0;
static uint32_t s_results[AS_MAX_RESULTS]; // Network-Byte-Order
static volatile uint8_t s_count = 0;
static char s_status[24];
static TaskHandle_t s_task = NULL;

// Große Arbeitspuffer statisch (nur ein Worker gleichzeitig) → kleiner Stack.
static bool s_found[256];        // index = host-lowbyte 1..254
static AsJob s_jobs[254];
static int s_hostmap[254];
static bool s_res[254];

static void as_add_result(uint32_t ip_be) {
    if(s_count >= AS_MAX_RESULTS) return;
    for(uint8_t i = 0; i < s_count; i++) {
        if(s_results[i] == ip_be) return;
    }
    s_results[s_count] = ip_be;
    s_count = s_count + 1;
}

// Führt bis zu AS_CONCURRENCY parallele non-blocking Connects aus. result[i] wird
// true, wenn Job i's Ziel geantwortet hat (offen ODER RST = erreichbar). Läuft
// batchweise über njobs.
static void as_run_jobs(const AsJob* jobs, int njobs, bool* result) {
    for(int start = 0; start < njobs && !s_stop; start += AS_CONCURRENCY) {
        int socks[AS_CONCURRENCY];
        int jidx[AS_CONCURRENCY];
        int nfd = 0;
        int maxfd = -1;
        fd_set wset;
        FD_ZERO(&wset);

        int end = start + AS_CONCURRENCY;
        if(end > njobs) end = njobs;
        for(int j = start; j < end; j++) {
            int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if(s < 0) continue;
            int fl = fcntl(s, F_GETFL, 0);
            fcntl(s, F_SETFL, fl | O_NONBLOCK);
            struct sockaddr_in addr = {
                .sin_family = AF_INET,
                .sin_port = htons(jobs[j].port),
                .sin_addr.s_addr = jobs[j].ip_be,
            };
            int ret = connect(s, (struct sockaddr*)&addr, sizeof(addr));
            if(ret == 0) {
                result[j] = true; // sofort verbunden
                close(s);
                continue;
            }
            if(errno != EINPROGRESS) {
                close(s);
                continue;
            }
            socks[nfd] = s;
            jidx[nfd] = j;
            FD_SET(s, &wset);
            if(s > maxfd) maxfd = s;
            nfd++;
        }

        if(nfd > 0) {
            struct timeval tv = {
                .tv_sec = AS_CONNECT_TIMEOUT_MS / 1000,
                .tv_usec = (AS_CONNECT_TIMEOUT_MS % 1000) * 1000,
            };
            select(maxfd + 1, NULL, &wset, NULL, &tv);
            for(int k = 0; k < nfd; k++) {
                if(FD_ISSET(socks[k], &wset)) {
                    int err = 0;
                    socklen_t len = sizeof(err);
                    getsockopt(socks[k], SOL_SOCKET, SO_ERROR, &err, &len);
                    // err==0 → offen; ECONNREFUSED/ECONNRESET → RST → Host lebt &
                    // wurde geroutet. Andere Fehler (EHOSTUNREACH, ETIMEDOUT) →
                    // nicht erreichbar.
                    if(err == 0 || err == ECONNREFUSED || err == ECONNRESET) {
                        result[jidx[k]] = true;
                    }
                }
                close(socks[k]);
            }
        }
    }
}

// Prüft per kleinem Gate, ob ein Subnetz überhaupt von uns aus routbar ist.
static bool as_subnet_routable(uint32_t net) {
    AsJob jobs[AS_GATE_COUNT * AS_GATE_PORT_COUNT];
    bool res[AS_GATE_COUNT * AS_GATE_PORT_COUNT];
    int n = 0;
    for(int gi = 0; gi < (int)AS_GATE_COUNT; gi++) {
        for(int pi = 0; pi < (int)AS_GATE_PORT_COUNT; pi++) {
            jobs[n].ip_be = htonl(net | (uint32_t)AS_GATE_HOSTS[gi]);
            jobs[n].port = AS_GATE_PORTS[pi];
            res[n] = false;
            n++;
        }
    }
    as_run_jobs(jobs, n, res);
    for(int i = 0; i < n; i++) {
        if(res[i]) return true;
    }
    return false;
}

// Voll-Sweep eines routbaren /24: pro Port eine Runde über alle noch nicht
// gefundenen Hosts; gefundene werden in nachfolgenden Port-Runden übersprungen.
static void as_sweep_subnet(uint32_t net, int subnet_idx, int subnet_total) {
    memset(s_found, 0, sizeof(s_found));

    for(int pi = 0; pi < (int)AS_PORT_COUNT && !s_stop; pi++) {
        int nj = 0;
        for(int h = 1; h <= 254; h++) {
            if(s_found[h]) continue;
            s_jobs[nj].ip_be = htonl(net | (uint32_t)h);
            s_jobs[nj].port = AS_PORTS[pi];
            s_hostmap[nj] = h;
            s_res[nj] = false;
            nj++;
        }

        // In Chunks laufen, damit der Progress mitwandert.
        for(int off = 0; off < nj && !s_stop; off += AS_CONCURRENCY) {
            int cnt = nj - off;
            if(cnt > AS_CONCURRENCY) cnt = AS_CONCURRENCY;
            as_run_jobs(&s_jobs[off], cnt, &s_res[off]);

            // Progress: Anteil über Subnetze + Ports + Hosts.
            int host_done = off + cnt;
            int within = (pi * 254 + host_done);
            int total = AS_PORT_COUNT * 254;
            int subnet_frac = (within * 100) / (total > 0 ? total : 1);
            int base = (subnet_idx * 100) / (subnet_total > 0 ? subnet_total : 1);
            int span = 100 / (subnet_total > 0 ? subnet_total : 1);
            int p = base + (subnet_frac * span) / 100;
            if(p > 99) p = 99;
            s_progress = (uint8_t)p;
        }

        for(int j = 0; j < nj; j++) {
            if(s_res[j]) {
                int h = s_hostmap[j];
                if(!s_found[h]) {
                    s_found[h] = true;
                    as_add_result(htonl(net | (uint32_t)h));
                }
            }
        }
    }
}

static void as_worker(void* arg) {
    UNUSED(arg);
    s_running = true;

    uint32_t own_ip = wlan_hal_get_own_ip();
    uint32_t own_net24 = ntohl(own_ip) & 0xFFFFFF00u; // Host-Order /24-Basis

    for(int ci = 0; ci < (int)AS_CANDIDATE_COUNT && !s_stop; ci++) {
        uint32_t net = AS_CANDIDATE_NETS[ci];
        s_progress = (uint8_t)((ci * 100) / AS_CANDIDATE_COUNT);

        // Eigenes /24 überspringen — das ist der L2-Fall (ARP), nicht L3.
        if(net == own_net24) continue;

        snprintf(
            s_status, sizeof(s_status), "%u.%u.%u.x",
            (unsigned)((net >> 24) & 0xFF), (unsigned)((net >> 16) & 0xFF),
            (unsigned)((net >> 8) & 0xFF));

        if(!as_subnet_routable(net)) continue;
        as_sweep_subnet(net, ci, AS_CANDIDATE_COUNT);
    }

    s_progress = 100;
    s_status[0] = '\0';
    s_done = true;
    s_running = false;
    s_task = NULL;
    vTaskDelete(NULL);
}

void wlan_airsnitch_reset(void) {
    s_stop = false;
    s_done = false;
    s_running = false;
    s_progress = 0;
    s_count = 0;
    s_status[0] = '\0';
    memset(s_results, 0, sizeof(s_results));
}

void wlan_airsnitch_start(void) {
    if(s_task) return;
    s_stop = false;
    s_done = false;
    s_progress = 0;
    s_count = 0;
    s_status[0] = '\0';
    xTaskCreate(as_worker, "AirSnitch", 6144, NULL, 4, &s_task);
}

bool wlan_airsnitch_is_done(void) {
    return s_done;
}

void wlan_airsnitch_stop(void) {
    if(!s_task) return;
    s_stop = true;
    while(s_task) {
        furi_delay_ms(20);
    }
}

uint8_t wlan_airsnitch_get_progress(void) {
    return s_progress;
}

uint8_t wlan_airsnitch_get_count(void) {
    return s_count;
}

bool wlan_airsnitch_get(uint8_t idx, uint32_t* ip_be_out) {
    if(idx >= s_count || !ip_be_out) return false;
    *ip_be_out = s_results[idx];
    return true;
}

void wlan_airsnitch_get_status(char* out, size_t cap) {
    if(!out || cap == 0) return;
    strncpy(out, s_status, cap - 1);
    out[cap - 1] = '\0';
}
