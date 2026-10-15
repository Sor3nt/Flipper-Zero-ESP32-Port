#include "radio_suite.h"

#include <stdio.h>
#include <string.h>

/* ========================================================================== *
 *  Constants
 * ========================================================================== */

#define RADIO_CHANNELS       126 /* nRF24 channels 0..125 */
#define RADIO_TOP_N          6 /* rows shown by the channel scanner */
#define RADIO_WIFI_CHANNELS  14
#define RADIO_BLE_CHANNELS   3
#define RADIO_DIAG_REGS      14
#define RADIO_DIAG_VISIBLE   6
#define RADIO_SETTLE_US      400
#define RADIO_PROMISC_WINDOW 40 /* ms per BLE channel */

/* extra pentest tools (MJ Scan / Replay / Fuzzer / RSSI) */
#define RADIO_MJ_CHANNELS   81 /* MouseJack / 2.4G-HID band: nRF ch 0..80 */
#define RADIO_MJ_ENTRIES    8 /* unique payload prefixes kept in the table */
#define RADIO_MJ_KEYLEN     5 /* payload prefix bytes used as the "address" */
#define RADIO_MJ_VISIBLE    5 /* list rows drawn on screen */
#define RADIO_MJ_DWELL_MS   12 /* listen window per channel */
#define RADIO_FUZZ_CHANNELS 81 /* fuzzed channel range 0..80 */
#define RADIO_FUZZ_PATTERNS 6 /* number of payload patterns */
#define RADIO_RSSI_SAMPLES  64 /* REG_RPD samples per update */

/* REG_RPD exists in nrf24.h; OBSERVE_TX is only needed for the diag screen. */
#define RADIO_REG_OBSERVE_TX 0x08

/* nRF24 channel numbers of the BLE advertising channels (2402/2426/2480 MHz) */
static const uint8_t radio_ble_channels[RADIO_BLE_CHANNELS] = {2, 26, 80};

/* -------------------------------------------------------------------------- *
 *  Jammer model
 * -------------------------------------------------------------------------- */

typedef enum {
    JamModeCw = 0,
    JamModeFlood,
    JamModeTurbo,
} JamMode;

#define JAM_PRESET_COUNT 8
#define JAM_PARAM_COUNT  5 /* mode, power, preset, custom start, custom stop */

static const char* jam_mode_names[] = {"CW", "FLOOD", "TURBO"};
static const char* jam_preset_names[JAM_PRESET_COUNT] = {
    "Bluetooth",
    "BLE Adv",
    "BLE Data",
    "WiFi All",
    "WiFi Sel",
    "Zigbee",
    "Drone",
    "Custom",
};

static const char* fuzz_pattern_names[RADIO_FUZZ_PATTERNS] = {
    "0x00",
    "0xFF",
    "0xAA",
    "0x55",
    "Counter",
    "Random",
};

/* -------------------------------------------------------------------------- *
 *  Per-screen models (kept inside the Views, updated under view model lock)
 * -------------------------------------------------------------------------- */

typedef struct {
    uint8_t level[RADIO_CHANNELS];
    uint8_t peak[RADIO_CHANNELS];
    uint8_t cur;
    uint8_t samples;
    bool running;
} SpectrumModel;

typedef struct {
    uint16_t hits[RADIO_CHANNELS];
    uint8_t cur;
    uint8_t dwell_ms;
    uint16_t rounds;
    bool running;
} ChanScanModel;

typedef struct {
    uint8_t level[RADIO_WIFI_CHANNELS]; /* 0..255 congestion */
    uint8_t cur_wifi;
    uint8_t dwell_ms;
    uint16_t sweeps;
    bool running;
} WifiModel;

typedef struct {
    uint8_t activity[RADIO_BLE_CHANNELS]; /* 0/1 RPD seen this window */
    uint16_t pkts[RADIO_BLE_CHANNELS];
    uint8_t active;
    uint8_t dwell_ms;
    bool running;
} BleModel;

typedef struct {
    uint8_t mode; /* JamMode */
    uint8_t power; /* 0..3 */
    uint8_t preset; /* 0..JAM_PRESET_COUNT-1 */
    uint8_t sel; /* selected parameter */
    uint8_t custom_start;
    uint8_t custom_stop;
    uint8_t cur_channel;
    uint32_t sent;
    bool running;
} JammerModel;

typedef struct {
    bool connected;
    uint8_t vals[RADIO_DIAG_REGS];
    uint8_t sel;
    uint32_t refresh;
} DiagModel;

/* --- MJ Scan: wireless-input (MouseJack) reconnaissance --------------- */

typedef struct {
    uint8_t key[RADIO_MJ_KEYLEN]; /* first 4..5 payload bytes (zero padded) */
    uint8_t klen; /* valid bytes in key (1..RADIO_MJ_KEYLEN) */
    uint16_t count; /* packets seen with this prefix */
} MjScanEntry;

typedef struct {
    MjScanEntry entries[RADIO_MJ_ENTRIES];
    uint8_t entry_count;
    uint8_t scroll; /* first visible list row */
    uint8_t cur_chan;
    uint32_t total; /* every packet seen */
    bool running;
} MjScanModel;

/* --- Packet Replay ------------------------------------------------------ */

typedef struct {
    uint8_t payload[MAX_PAYLOAD_SIZE];
    uint8_t chan; /* channel the packet was captured on */
    uint16_t delay_ms; /* replay pacing */
    bool captured;
    bool replaying; /* worker is actively retransmitting */
    uint32_t count;
    bool running;
} ReplayModel;

/* --- Packet Fuzzer ------------------------------------------------------ */

typedef struct {
    uint8_t pattern; /* index into fuzz_pattern_names[] */
    uint8_t delay_ms;
    uint8_t cur_channel;
    uint32_t sent;
    uint32_t rate; /* packets per second (500 ms window) */
    bool running;
} FuzzerModel;

/* --- RSSI meter --------------------------------------------------------- */

typedef struct {
    uint8_t channel;
    uint8_t level; /* 0..255 scaled RPD hit ratio */
    uint8_t peak; /* peak hold */
    uint8_t raw; /* raw RPD hits in the last window */
    bool running;
} RssiModel;

typedef struct RadioSuite {
    Nrf24Tool* app;
    RadioTool current;

    View* spectrum_view;
    View* chanscan_view;
    View* wifi_view;
    View* ble_view;
    View* jammer_view;
    View* diag_view;
    View* mjscan_view;
    View* replay_view;
    View* fuzzer_view;
    View* rssi_view;

    FuriThread* thread;
    _Atomic bool running;
} RadioSuite;

/* -------------------------------------------------------------------------- *
 *  Small helpers
 * -------------------------------------------------------------------------- */

static uint32_t radio_view_id(RadioTool tool) {
    switch(tool) {
    case RadioToolSpectrum:
        return VIEW_RADIO_SPECTRUM;
    case RadioToolChannelScan:
        return VIEW_RADIO_CHANSCAN;
    case RadioToolWifi:
        return VIEW_RADIO_WIFI;
    case RadioToolBle:
        return VIEW_RADIO_BLE;
    case RadioToolJammer:
        return VIEW_RADIO_JAMMER;
    case RadioToolMjScan:
        return VIEW_RADIO_MJSCAN;
    case RadioToolReplay:
        return VIEW_RADIO_REPLAY;
    case RadioToolFuzzer:
        return VIEW_RADIO_FUZZER;
    case RadioToolRssi:
        return VIEW_RADIO_RSSI;
    case RadioToolDiag:
    default:
        return VIEW_RADIO_DIAG;
    }
}

/* ---- RPD listen primitives (implemented locally: the ported libnrf24 has no
 *      nrf24_init_promisc_mode / nrf24_sniff_address, and libnrf24 must not be
 *      edited) ------------------------------------------------------------- */

static void radio_listen_setup(void) {
    nrf24_write_reg(REG_CONFIG, 0x00); /* power down while configuring */
    nrf24_write_reg(REG_STATUS, 0x70); /* clear IRQ flags */
    nrf24_write_reg(REG_EN_AA, 0x00);
    nrf24_write_reg(REG_DYNPD, 0x00);
    nrf24_write_reg(REG_FEATURE, 0x00);
    nrf24_write_reg(REG_SETUP_RETR, 0x00);
    nrf24_set_maclen(ADDR_WIDTH_2_BYTES);
    nrf24_set_payload_size(MAX_PAYLOAD_SIZE, 0);
    nrf24_set_rate(DATA_RATE_2MBPS);
    nrf24_set_txpower(TX_POWER_0DBM);
    nrf24_flush_rx();
    nrf24_flush_tx();
}

/* One latched RPD sample: CE low (clears RPD) -> CE high in RX -> read RPD. */
static uint8_t radio_rpd_sample(uint8_t chan, uint32_t settle_us) {
    nrf24_set_chan(chan);
    nrf24_set_mode(NRF24_MODE_STANDBY);
    nrf24_set_mode(NRF24_MODE_RX);
    furi_delay_us(settle_us < 100 ? 100 : settle_us);
    return (nrf24_read_reg(REG_RPD) & 0x01u) ? 1u : 0u;
}

static uint8_t radio_rpd_count(uint8_t chan, uint8_t samples, uint32_t settle_us) {
    uint8_t hits = 0;
    for(uint8_t i = 0; i < samples; i++) {
        hits += radio_rpd_sample(chan, settle_us);
    }
    return hits;
}

/* Promiscuous receiver for the BLE channel activity/packet counter. */
static void radio_promisc_setup(uint8_t chan) {
    uint8_t preamble[2] = {0xAA, 0x00};

    nrf24_write_reg(REG_CONFIG, 0x00);
    nrf24_write_reg(REG_STATUS, 0x70);
    nrf24_write_reg(REG_DYNPD, 0x00);
    nrf24_write_reg(REG_EN_AA, 0x00);
    nrf24_write_reg(REG_FEATURE, 0x00);
    nrf24_set_maclen(ADDR_WIDTH_2_BYTES);
    nrf24_set_rx_mac(preamble, ADDR_WIDTH_2_BYTES, 0);
    nrf24_set_rx_mac(preamble, ADDR_WIDTH_2_BYTES, 1);
    nrf24_set_payload_size(MAX_PAYLOAD_SIZE, 0);
    nrf24_set_payload_size(MAX_PAYLOAD_SIZE, 1);
    nrf24_write_reg(REG_EN_RXADDR, 0x03);
    nrf24_set_rate(DATA_RATE_2MBPS);
    nrf24_set_chan(chan);
    nrf24_flush_rx();
    nrf24_flush_tx();
    nrf24_write_reg(REG_CONFIG, PWR_UP | PRIM_RX); /* CRC off, no AA */
    nrf24_set_mode(NRF24_MODE_RX);
}

/* Poll RX_DR / RPD for `window_ms`; returns 1 if any activity was seen. */
static uint8_t radio_promisc_poll(RadioSuite* r, uint32_t window_ms, uint16_t* pkts) {
    uint32_t start = furi_get_tick();
    uint8_t activity = 0;

    while(r->running && (furi_get_tick() - start) < window_ms) {
        uint8_t st = nrf24_status();
        if(st & RX_DR) {
            nrf24_flush_rx();
            nrf24_write_reg(REG_STATUS, RX_DR);
            (*pkts)++;
            activity = 1;
        }
        if(nrf24_read_reg(REG_RPD) & 0x01u) activity = 1;
        furi_delay_ms(1);
    }
    return activity;
}

/* ---- CW carrier (CONT_WAVE + PLL_LOCK), implemented locally -------------- */

static void radio_cw_start(uint8_t chan, uint8_t power) {
    nrf24_set_mode(NRF24_MODE_STANDBY);
    nrf24_set_chan(chan);

    uint8_t rf = nrf24_read_reg(REG_RF_SETUP);
    rf = (uint8_t)((rf & ~0x06u) | ((power & 0x03u) << 1)); /* RF_PWR bits 2:1 */
    rf |= 0x80u | 0x10u; /* CONT_WAVE | PLL_LOCK */
    nrf24_write_reg(REG_RF_SETUP, rf);
    nrf24_write_reg(REG_EN_AA, 0x00);

    uint8_t cfg = (uint8_t)(nrf24_read_reg(REG_CONFIG) & ~0x08u); /* CRC off */
    nrf24_write_reg(REG_CONFIG, cfg);

    nrf24_set_mode(NRF24_MODE_TX); /* CE high -> continuous carrier */
}

static void radio_cw_stop(void) {
    uint8_t rf = (uint8_t)(nrf24_read_reg(REG_RF_SETUP) & ~(0x80u | 0x10u));
    nrf24_write_reg(REG_RF_SETUP, rf);

    uint8_t cfg = (uint8_t)(nrf24_read_reg(REG_CONFIG) | 0x08u);
    nrf24_write_reg(REG_CONFIG, cfg);

    nrf24_set_mode(NRF24_MODE_STANDBY);
}

static void radio_emit_setup(void) {
    nrf24_write_reg(REG_CONFIG, 0x00);
    nrf24_write_reg(REG_STATUS, 0x70);
    nrf24_write_reg(REG_EN_AA, 0x00);
    nrf24_write_reg(REG_EN_RXADDR, 0x00);
    nrf24_write_reg(REG_DYNPD, 0x00);
    nrf24_write_reg(REG_FEATURE, 0x01); /* EN_DYN_ACK -> W_TX_PAYLOAD_NOACK */
    nrf24_write_reg(REG_SETUP_RETR, 0x00);
    nrf24_set_maclen(ADDR_WIDTH_2_BYTES);
    nrf24_set_payload_size(MAX_PAYLOAD_SIZE, 0);
    nrf24_set_rate(DATA_RATE_2MBPS);
    nrf24_set_txpower(TX_POWER_0DBM);
    nrf24_flush_tx();
    nrf24_flush_rx();
}

static void radio_emit_stop(void) {
    uint8_t rf = (uint8_t)(nrf24_read_reg(REG_RF_SETUP) & ~(0x80u | 0x10u));
    nrf24_write_reg(REG_RF_SETUP, rf);
    nrf24_flush_tx();
    nrf24_set_mode(NRF24_MODE_POWER_DOWN);
}

/* Jammer preset -> nRF channel for a given step. */
static uint8_t jam_channel_for(const JammerModel* m, uint8_t step) {
    switch(m->preset) {
    case 0: /* Bluetooth BR/EDR: 0..78 */
        return (uint8_t)(step % 79);
    case 1: /* BLE advertising: 2402 / 2426 / 2480 */
        return radio_ble_channels[step % RADIO_BLE_CHANNELS];
    case 2: /* BLE data: 0..36 */
        return (uint8_t)(step % 37);
    case 3: /* WiFi all */
        return (uint8_t)(step % RADIO_CHANNELS);
    case 4: /* WiFi channel centres (nRF 7,12,...,67) */
        return (uint8_t)(7 + 5 * (step % 13));
    case 5: /* Zigbee 2405..2480 -> nRF 5..80 */
        return (uint8_t)(5 + (step % 76));
    case 6: /* Drone / RC range */
        return (uint8_t)(25 + (step % 101));
    case 7: /* Custom */
    default: {
        uint8_t span = (uint8_t)((m->custom_stop >= m->custom_start) ?
                                     (m->custom_stop - m->custom_start + 1) :
                                     1);
        return (uint8_t)(m->custom_start + (step % span));
    }
    }
}

static uint8_t jam_preset_first(const JammerModel* m) {
    switch(m->preset) {
    case 1:
        return radio_ble_channels[0];
    case 4:
        return 7;
    case 5:
        return 5;
    case 6:
        return 25;
    case 7:
        return m->custom_start;
    default:
        return 0;
    }
}

/* ========================================================================== *
 *  Worker threads (one shared thread slot)
 * ========================================================================== */

static int32_t radio_spectrum_worker(void* ctx) {
    RadioSuite* r = ctx;
    radio_listen_setup();

    while(r->running) {
        furi_delay_ms(1);
        SpectrumModel* m0 = view_get_model(r->spectrum_view);
        uint8_t samples = m0->samples ? m0->samples : 2;
        view_commit_model(r->spectrum_view, false);

        for(uint8_t ch = 0; ch < RADIO_CHANNELS && r->running; ch++) {
            uint8_t hits = radio_rpd_count(ch, samples, RADIO_SETTLE_US);
            uint16_t v = (uint16_t)(((uint16_t)hits * 255u) / samples);
            if(v > 255u) v = 255u;

            SpectrumModel* m = view_get_model(r->spectrum_view);
            m->level[ch] = (uint8_t)v;
            if(v > m->peak[ch]) m->peak[ch] = (uint8_t)v;
            m->cur = ch;
            view_commit_model(r->spectrum_view, (ch % 16) == 15);
            furi_delay_ms(1);
        }

        SpectrumModel* md = view_get_model(r->spectrum_view);
        for(uint8_t i = 0; i < RADIO_CHANNELS; i++) {
            if(md->peak[i] > md->level[i]) {
                uint8_t d = (uint8_t)(md->peak[i] - md->level[i]);
                md->peak[i] = (uint8_t)(d > 8 ? md->peak[i] - 8 : md->level[i]);
            }
        }
        view_commit_model(r->spectrum_view, true);
    }
    return 0;
}

static int32_t radio_chanscan_worker(void* ctx) {
    RadioSuite* r = ctx;
    radio_listen_setup();

    while(r->running) {
        furi_delay_ms(1);
        ChanScanModel* m0 = view_get_model(r->chanscan_view);
        uint8_t dwell_ms = m0->dwell_ms ? m0->dwell_ms : 3;
        view_commit_model(r->chanscan_view, false);

        for(uint8_t ch = 0; ch < RADIO_CHANNELS && r->running; ch++) {
            uint8_t hits = radio_rpd_count(ch, 3, (uint32_t)dwell_ms * 200u);
            ChanScanModel* m = view_get_model(r->chanscan_view);
            m->hits[ch] = (uint16_t)(m->hits[ch] + hits);
            m->cur = ch;
            view_commit_model(r->chanscan_view, (ch % 16) == 15);
            furi_delay_ms(1);
        }

        ChanScanModel* mr = view_get_model(r->chanscan_view);
        mr->rounds++;
        view_commit_model(r->chanscan_view, true);
    }
    return 0;
}

static int32_t radio_wifi_worker(void* ctx) {
    RadioSuite* r = ctx;
    radio_listen_setup();

    while(r->running) {
        furi_delay_ms(1);
        WifiModel* m0 = view_get_model(r->wifi_view);
        uint8_t dwell_ms = m0->dwell_ms ? m0->dwell_ms : 2;
        view_commit_model(r->wifi_view, false);

        for(uint8_t n = 1; n <= RADIO_WIFI_CHANNELS && r->running; n++) {
            uint8_t first = (uint8_t)(5u * (n - 1u) + 1u);
            uint8_t last = (uint8_t)(5u * (n - 1u) + 23u);
            if(last > (MAX_CHANNEL)) last = MAX_CHANNEL;

            uint8_t busy = 0;
            uint8_t span = 0;
            for(uint8_t ch = first; ch <= last; ch++) {
                busy += radio_rpd_sample(ch, (uint32_t)dwell_ms * 200u);
                span++;
            }
            uint8_t lvl = span ? (uint8_t)(((uint16_t)busy * 255u) / span) : 0;

            WifiModel* m = view_get_model(r->wifi_view);
            m->level[n - 1] = lvl;
            m->cur_wifi = n;
            view_commit_model(r->wifi_view, true);
        }

        WifiModel* ms = view_get_model(r->wifi_view);
        ms->sweeps++;
        view_commit_model(r->wifi_view, true);
    }
    return 0;
}

static int32_t radio_ble_worker(void* ctx) {
    RadioSuite* r = ctx;

    while(r->running) {
        furi_delay_ms(1);
        for(uint8_t i = 0; i < RADIO_BLE_CHANNELS && r->running; i++) {
            BleModel* m0 = view_get_model(r->ble_view);
            uint8_t dwell_ms = m0->dwell_ms ? m0->dwell_ms : RADIO_PROMISC_WINDOW;
            view_commit_model(r->ble_view, false);

            radio_promisc_setup(radio_ble_channels[i]);
            uint16_t pkts = 0;
            uint8_t act = radio_promisc_poll(r, dwell_ms, &pkts);

            BleModel* m = view_get_model(r->ble_view);
            m->activity[i] = act;
            m->pkts[i] = (uint16_t)(m->pkts[i] + pkts);
            m->active = i;
            view_commit_model(r->ble_view, true);
        }
    }
    return 0;
}

static int32_t radio_jammer_worker(void* ctx) {
    RadioSuite* r = ctx;
    radio_emit_setup();

    bool cw_on = false;
    uint8_t cw_chan = 0;
    uint8_t cw_power = 0;
    uint8_t step = 0;
    uint8_t payload[MAX_PAYLOAD_SIZE];
    memset(payload, 0xFF, sizeof(payload));

    while(r->running) {
        furi_delay_ms(1);
        JammerModel* m0 = view_get_model(r->jammer_view);
        uint8_t mode = m0->mode;
        uint8_t power = m0->power;
        uint8_t preset0 = m0->preset;
        uint8_t cstart = m0->custom_start;
        uint8_t cstop = m0->custom_stop;
        view_commit_model(r->jammer_view, false);

        /* snapshot for channel computation */
        JammerModel snap;
        memset(&snap, 0, sizeof(snap));
        snap.preset = preset0;
        snap.custom_start = cstart;
        snap.custom_stop = cstop;

        if(mode == JamModeCw) {
            uint8_t chan = jam_channel_for(&snap, step);
            if(!cw_on || chan != cw_chan || power != cw_power) {
                if(cw_on) radio_cw_stop();
                radio_cw_start(chan, power);
                cw_on = true;
                cw_chan = chan;
                cw_power = power;
            }

            JammerModel* m = view_get_model(r->jammer_view);
            m->cur_channel = chan;
            m->sent++;
            view_commit_model(r->jammer_view, true);
            furi_delay_ms(120);
        } else {
            if(cw_on) {
                radio_cw_stop();
                cw_on = false;
            }

            uint8_t chan = jam_channel_for(&snap, step);
            nrf24_set_chan(chan);
            nrf24_set_mode(NRF24_MODE_TX);
            nrf24_txpacket(payload, sizeof(payload), true, NULL, NULL);

            JammerModel* m = view_get_model(r->jammer_view);
            m->cur_channel = chan;
            m->sent++;
            view_commit_model(r->jammer_view, true);

            step++;
            if(mode == JamModeFlood) furi_delay_ms(2);
        }
    }

    if(cw_on) radio_cw_stop();
    radio_emit_stop();
    return 0;
}

/* ---- MJ Scan worker: promiscuous 0..80 sweep, tally payload prefixes ---- */

static void radio_mjscan_add(RadioSuite* r, const uint8_t* data, uint8_t len, uint8_t chan) {
    uint8_t klen = (len > RADIO_MJ_KEYLEN) ? RADIO_MJ_KEYLEN : len;
    if(klen == 0) klen = 1;

    MjScanModel* m = view_get_model(r->mjscan_view);
    m->total++;
    m->cur_chan = chan;

    int found = -1;
    for(uint8_t i = 0; i < m->entry_count; i++) {
        if(m->entries[i].klen == klen && memcmp(m->entries[i].key, data, klen) == 0) {
            found = (int)i;
            break;
        }
    }

    if(found >= 0) {
        m->entries[found].count++;
    } else if(m->entry_count < RADIO_MJ_ENTRIES) {
        MjScanEntry* e = &m->entries[m->entry_count];
        memset(e, 0, sizeof(MjScanEntry));
        memcpy(e->key, data, klen);
        e->klen = klen;
        e->count = 1;
        m->entry_count++;
    } else {
        /* table full: evict the least-seen prefix so active devices survive */
        uint8_t lo = 0;
        for(uint8_t i = 1; i < RADIO_MJ_ENTRIES; i++) {
            if(m->entries[i].count < m->entries[lo].count) lo = i;
        }
        MjScanEntry* e = &m->entries[lo];
        memset(e, 0, sizeof(MjScanEntry));
        memcpy(e->key, data, klen);
        e->klen = klen;
        e->count = 1;
    }

    view_commit_model(r->mjscan_view, true);
}

static int32_t radio_mjscan_worker(void* ctx) {
    RadioSuite* r = ctx;
    uint8_t chan = 0;
    uint8_t buf[MAX_PAYLOAD_SIZE];

    while(r->running) {
        furi_delay_ms(1);
        radio_promisc_setup(chan);

        MjScanModel* mc = view_get_model(r->mjscan_view);
        mc->cur_chan = chan;
        view_commit_model(r->mjscan_view, true);

        uint32_t start = furi_get_tick();
        while(r->running && (furi_get_tick() - start) < RADIO_MJ_DWELL_MS) {
            uint8_t size = 0;
            uint8_t pipe = 0;
            if(nrf24_rxpacket(buf, &size, &pipe, false)) {
                radio_mjscan_add(r, buf, size, chan);
            }
            furi_delay_ms(1);
        }

        chan = (uint8_t)((chan + 1) % RADIO_MJ_CHANNELS);
    }
    return 0;
}

/* ---- Packet Replay worker: capture one packet, then loop-retransmit it --- */

static int32_t radio_replay_worker(void* ctx) {
    RadioSuite* r = ctx;
    uint8_t sweep = 0;
    uint8_t buf[MAX_PAYLOAD_SIZE];

    radio_emit_setup();
    nrf24_set_mode(NRF24_MODE_STANDBY);

    while(r->running) {
        furi_delay_ms(1);
        ReplayModel* m0 = view_get_model(r->replay_view);
        bool captured = m0->captured;
        uint8_t chan = m0->chan;
        uint16_t delay_ms = m0->delay_ms ? m0->delay_ms : 100;
        uint8_t payload[MAX_PAYLOAD_SIZE];
        memcpy(payload, m0->payload, sizeof(payload));
        view_commit_model(r->replay_view, false);

        if(!captured) {
            /* capture phase: promiscuously hunt for the first packet */
            uint8_t scan_chan = (uint8_t)(sweep % RADIO_MJ_CHANNELS);
            radio_promisc_setup(scan_chan);

            uint32_t start = furi_get_tick();
            bool got = false;
            while(r->running && (furi_get_tick() - start) < RADIO_MJ_DWELL_MS) {
                uint8_t size = 0;
                uint8_t pipe = 0;
                if(nrf24_rxpacket(buf, &size, &pipe, false)) {
                    ReplayModel* m = view_get_model(r->replay_view);
                    memset(m->payload, 0, sizeof(m->payload));
                    if(size) memcpy(m->payload, buf, size);
                    m->chan = scan_chan;
                    m->captured = true;
                    m->count = 0;
                    m->replaying = false;
                    view_commit_model(r->replay_view, true);
                    got = true;
                    break;
                }
                furi_delay_ms(1);
            }

            sweep++;
            if(got) {
                /* promiscuous setup left the radio in RX: restore the TX path */
                radio_emit_setup();
                nrf24_set_mode(NRF24_MODE_STANDBY);
            }
            continue;
        }

        /* replay phase: retransmit the captured payload on its own channel */
        nrf24_set_chan(chan);
        nrf24_txpacket(payload, MAX_PAYLOAD_SIZE, true, NULL, NULL);

        ReplayModel* m = view_get_model(r->replay_view);
        m->replaying = true;
        m->count++;
        view_commit_model(r->replay_view, true);

        furi_delay_ms(delay_ms);
    }

    radio_emit_stop();
    return 0;
}

/* ---- Packet Fuzzer worker: cycle payload patterns across a channel range - */

static void radio_fuzz_payload(uint8_t pattern, uint32_t* seed, uint32_t counter, uint8_t* out) {
    switch(pattern % RADIO_FUZZ_PATTERNS) {
    case 0:
        memset(out, 0x00, MAX_PAYLOAD_SIZE);
        break;
    case 1:
        memset(out, 0xFF, MAX_PAYLOAD_SIZE);
        break;
    case 2:
        memset(out, 0xAA, MAX_PAYLOAD_SIZE);
        break;
    case 3:
        memset(out, 0x55, MAX_PAYLOAD_SIZE);
        break;
    case 4:
        for(uint8_t i = 0; i < MAX_PAYLOAD_SIZE; i++) {
            out[i] = (uint8_t)(counter + i);
        }
        break;
    default:
        for(uint8_t i = 0; i < MAX_PAYLOAD_SIZE; i++) {
            *seed ^= *seed << 13;
            *seed ^= *seed >> 17;
            *seed ^= *seed << 5;
            out[i] = (uint8_t)(*seed & 0xFFu);
        }
        break;
    }
}

static int32_t radio_fuzzer_worker(void* ctx) {
    RadioSuite* r = ctx;
    uint8_t payload[MAX_PAYLOAD_SIZE];
    uint8_t chan = 0;
    uint32_t seed = 0x13579BDFu;
    uint32_t counter = 0;
    uint32_t win_start = furi_get_tick();
    uint32_t win_count = 0;

    radio_emit_setup();
    nrf24_set_mode(NRF24_MODE_STANDBY);

    while(r->running) {
        furi_delay_ms(1);
        FuzzerModel* m0 = view_get_model(r->fuzzer_view);
        uint8_t pattern = (uint8_t)(m0->pattern % RADIO_FUZZ_PATTERNS);
        uint8_t delay_ms = m0->delay_ms ? m0->delay_ms : 2;
        view_commit_model(r->fuzzer_view, false);

        radio_fuzz_payload(pattern, &seed, counter, payload);
        counter++;

        nrf24_set_chan(chan);
        nrf24_txpacket(payload, MAX_PAYLOAD_SIZE, true, NULL, NULL);

        uint32_t now = furi_get_tick();
        win_count++;

        FuzzerModel* m = view_get_model(r->fuzzer_view);
        m->cur_channel = chan;
        m->sent++;
        m->pattern = (uint8_t)((pattern + 1) % RADIO_FUZZ_PATTERNS);
        if((now - win_start) >= 500u) {
            m->rate = (win_count * 1000u) / (now - win_start);
            win_start = now;
            win_count = 0;
        }
        view_commit_model(r->fuzzer_view, true);

        chan = (uint8_t)(((chan + 1) >= RADIO_FUZZ_CHANNELS) ? 0 : (chan + 1));
        furi_delay_ms(delay_ms);
    }

    radio_emit_stop();
    return 0;
}

/* ---- RSSI meter worker: single-channel RPD hit ratio + peak hold -------- */

static int32_t radio_rssi_worker(void* ctx) {
    RadioSuite* r = ctx;
    radio_listen_setup();

    while(r->running) {
        furi_delay_ms(1);
        RssiModel* m0 = view_get_model(r->rssi_view);
        uint8_t chan = (uint8_t)((m0->channel <= MAX_CHANNEL) ? m0->channel : 0);
        view_commit_model(r->rssi_view, false);

        uint8_t hits = 0;
        for(uint8_t i = 0; i < RADIO_RSSI_SAMPLES && r->running; i++) {
            hits += radio_rpd_sample(chan, RADIO_SETTLE_US);
        }

        uint8_t lvl = (uint8_t)(((uint16_t)hits * 255u) / RADIO_RSSI_SAMPLES);

        RssiModel* m = view_get_model(r->rssi_view);
        m->level = lvl;
        if(lvl > m->peak) m->peak = lvl;
        m->raw = hits;
        view_commit_model(r->rssi_view, true);
    }
    return 0;
}

static FuriThreadCallback radio_worker_fn(RadioTool tool) {
    switch(tool) {
    case RadioToolSpectrum:
        return radio_spectrum_worker;
    case RadioToolChannelScan:
        return radio_chanscan_worker;
    case RadioToolWifi:
        return radio_wifi_worker;
    case RadioToolBle:
        return radio_ble_worker;
    case RadioToolJammer:
        return radio_jammer_worker;
    case RadioToolMjScan:
        return radio_mjscan_worker;
    case RadioToolReplay:
        return radio_replay_worker;
    case RadioToolFuzzer:
        return radio_fuzzer_worker;
    case RadioToolRssi:
        return radio_rssi_worker;
    default:
        return NULL;
    }
}

static View* radio_view_for(RadioSuite* r, RadioTool tool) {
    switch(tool) {
    case RadioToolSpectrum:
        return r->spectrum_view;
    case RadioToolChannelScan:
        return r->chanscan_view;
    case RadioToolWifi:
        return r->wifi_view;
    case RadioToolBle:
        return r->ble_view;
    case RadioToolJammer:
        return r->jammer_view;
    case RadioToolMjScan:
        return r->mjscan_view;
    case RadioToolReplay:
        return r->replay_view;
    case RadioToolFuzzer:
        return r->fuzzer_view;
    case RadioToolRssi:
        return r->rssi_view;
    default:
        return r->diag_view;
    }
}

static void radio_set_running(RadioSuite* r, bool running) {
    View* v = radio_view_for(r, r->current);
    switch(r->current) {
    case RadioToolSpectrum: {
        SpectrumModel* m = view_get_model(v);
        m->running = running;
        view_commit_model(v, true);
    } break;
    case RadioToolChannelScan: {
        ChanScanModel* m = view_get_model(v);
        m->running = running;
        view_commit_model(v, true);
    } break;
    case RadioToolWifi: {
        WifiModel* m = view_get_model(v);
        m->running = running;
        view_commit_model(v, true);
    } break;
    case RadioToolBle: {
        BleModel* m = view_get_model(v);
        m->running = running;
        view_commit_model(v, true);
    } break;
    case RadioToolJammer: {
        JammerModel* m = view_get_model(v);
        m->running = running;
        view_commit_model(v, true);
    } break;
    case RadioToolMjScan: {
        MjScanModel* m = view_get_model(v);
        m->running = running;
        view_commit_model(v, true);
    } break;
    case RadioToolReplay: {
        ReplayModel* m = view_get_model(v);
        m->running = running;
        if(!running) m->replaying = false;
        view_commit_model(v, true);
    } break;
    case RadioToolFuzzer: {
        FuzzerModel* m = view_get_model(v);
        m->running = running;
        view_commit_model(v, true);
    } break;
    case RadioToolRssi: {
        RssiModel* m = view_get_model(v);
        m->running = running;
        view_commit_model(v, true);
    } break;
    default:
        break;
    }
}

static void radio_stop(RadioSuite* r) {
    r->running = false;
    if(r->thread) {
        furi_thread_join(r->thread);
        furi_thread_free(r->thread);
        r->thread = NULL;
    }
    nrf24_set_mode(NRF24_MODE_POWER_DOWN);
    nrf24_flush_rx();
    nrf24_flush_tx();
    nrf24_write_reg(REG_STATUS, 0x70); /* clear latched IRQs before next tool */
}

static void radio_start(RadioSuite* r, FuriThreadCallback fn) {
    radio_stop(r);
    if(!fn) return;
    r->running = true;
    r->thread = furi_thread_alloc_ex("nRF24 Radio", 8192, fn, r);
    if(!r->thread) {
        r->running = false;
        radio_set_running(r, false);
        return;
    }
    furi_thread_set_priority(r->thread, FuriThreadPriorityNormal);
    furi_thread_start(r->thread);
}

/* ========================================================================== *
 *  Draw callbacks
 * ========================================================================== */

static void spectrum_draw(Canvas* canvas, void* model) {
    SpectrumModel* m = model;
    char line[40];

    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontSecondary);

    uint8_t best_ch = 0;
    for(uint8_t i = 1; i < RADIO_CHANNELS; i++) {
        if(m->level[i] > m->level[best_ch]) best_ch = i;
    }
    snprintf(line, sizeof(line), "Spectrum %s ch%u %uMHz", m->running ? "RUN" : "IDLE", best_ch, 2400 + best_ch);
    canvas_draw_str(canvas, 1, 8, line);
    canvas_draw_line(canvas, 0, 10, 127, 10);

    const uint8_t base_y = 63;
    const uint8_t top_y = 12;
    const uint8_t h = base_y - top_y;
    for(uint8_t ch = 0; ch < RADIO_CHANNELS; ch++) {
        uint8_t x = (uint8_t)(ch + 1);
        uint8_t bh = (uint8_t)(((uint16_t)m->level[ch] * h) / 255u);
        if(bh) canvas_draw_line(canvas, x, base_y, x, (uint8_t)(base_y - bh));
        uint8_t ph = (uint8_t)(((uint16_t)m->peak[ch] * h) / 255u);
        if(ph) canvas_draw_dot(canvas, x, (uint8_t)(base_y - ph));
    }
}

static void chanscan_draw(Canvas* canvas, void* model) {
    ChanScanModel* m = model;
    char line[40];

    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontSecondary);
    snprintf(
        line, sizeof(line), "Ch scan %s rounds %u", m->running ? "RUN" : "IDLE", m->rounds);
    canvas_draw_str(canvas, 1, 8, line);
    canvas_draw_line(canvas, 0, 10, 127, 10);

    /* partial selection of the top-N busiest channels */
    uint8_t chosen[RADIO_TOP_N];
    memset(chosen, 0xFF, sizeof(chosen));
    for(uint8_t ch = 0; ch < RADIO_CHANNELS; ch++) {
        for(uint8_t k = 0; k < RADIO_TOP_N; k++) {
            if(chosen[k] == 0xFF || m->hits[ch] > m->hits[chosen[k]]) {
                for(uint8_t j = RADIO_TOP_N - 1; j > k; j--) chosen[j] = chosen[j - 1];
                chosen[k] = ch;
                break;
            }
        }
    }

    uint8_t y = 12;
    for(uint8_t k = 0; k < RADIO_TOP_N; k++) {
        if(chosen[k] == 0xFF) break;
        uint8_t ch = chosen[k];
        snprintf(line, sizeof(line), "%u: ch%3u %4uMHz  %u", k + 1, ch, 2400 + ch, m->hits[ch]);
        canvas_draw_str(canvas, 2, y, line);
        y += 8;
    }
    /* current dwell + scanning channel */
    snprintf(line, sizeof(line), "now ch%u  dwell %ums", m->cur, m->dwell_ms);
    canvas_draw_str(canvas, 2, 62, line);
}

static void wifi_draw(Canvas* canvas, void* model) {
    WifiModel* m = model;
    char line[40];

    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontSecondary);
    snprintf(line, sizeof(line), "WiFi Mon %s sw%u", m->running ? "RUN" : "IDLE", m->sweeps);
    canvas_draw_str(canvas, 1, 8, line);
    canvas_draw_line(canvas, 0, 10, 127, 10);

    const uint8_t base_y = 52;
    const uint8_t top_y = 13;
    const uint8_t h = base_y - top_y;
    const uint8_t bw = 8;
    const uint8_t x0 = 4;
    for(uint8_t i = 0; i < RADIO_WIFI_CHANNELS; i++) {
        uint8_t x = (uint8_t)(x0 + i * bw);
        uint8_t bh = (uint8_t)(((uint16_t)m->level[i] * h) / 255u);
        canvas_draw_frame(canvas, x, top_y, bw - 2, h + 1);
        if(bh) canvas_draw_box(canvas, x, (uint8_t)(base_y - bh), bw - 2, bh);
        if((i + 1) == m->cur_wifi) canvas_draw_str(canvas, x, base_y + 8, "*");
    }
    snprintf(line, sizeof(line), "1..14  max %u  dwell %ums", 255, m->dwell_ms);
    canvas_draw_str(canvas, 2, 62, line);
}

static void ble_draw(Canvas* canvas, void* model) {
    BleModel* m = model;
    char line[40];

    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontSecondary);
    snprintf(line, sizeof(line), "BLE Adv %s", m->running ? "RUN" : "IDLE");
    canvas_draw_str(canvas, 1, 8, line);
    canvas_draw_line(canvas, 0, 10, 127, 10);

    uint8_t y = 22;
    for(uint8_t i = 0; i < RADIO_BLE_CHANNELS; i++) {
        uint8_t ch = radio_ble_channels[i];
        snprintf(
            line,
            sizeof(line),
            "ch%3u %4uMHz %s %u",
            ch,
            2400 + ch,
            m->activity[i] ? "ACT" : "---",
            m->pkts[i]);
        if(m->active == i && m->running) {
            canvas_draw_str(canvas, 2, (uint8_t)(y - 8), ">");
        }
        canvas_draw_str(canvas, 8, y, line);
        y += 12;
    }
    snprintf(line, sizeof(line), "dwell %ums  total %u", m->dwell_ms,
             (uint16_t)(m->pkts[0] + m->pkts[1] + m->pkts[2]));
    canvas_draw_str(canvas, 2, 62, line);
}

static void jammer_draw(Canvas* canvas, void* model) {
    JammerModel* m = model;
    char line[44];

    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontSecondary);
    snprintf(line, sizeof(line), "Jammer %s", m->running ? "RUN" : "STOP");
    canvas_draw_str(canvas, 1, 8, line);
    canvas_draw_line(canvas, 0, 10, 127, 10);

    const char* rows[JAM_PARAM_COUNT];
    char v0[20], v1[20], v2[20], v3[20], v4[20];
    snprintf(v0, sizeof(v0), "Mode: %s", jam_mode_names[m->mode < 3 ? m->mode : 0]);
    snprintf(v1, sizeof(v1), "Power: %u", m->power);
    snprintf(v2, sizeof(v2), "Preset: %s", jam_preset_names[m->preset < JAM_PRESET_COUNT ? m->preset : 0]);
    snprintf(v3, sizeof(v3), "Start: ch%u", m->custom_start);
    snprintf(v4, sizeof(v4), "Stop:  ch%u", m->custom_stop);
    rows[0] = v0;
    rows[1] = v1;
    rows[2] = v2;
    rows[3] = v3;
    rows[4] = v4;

    uint8_t y = 21;
    for(uint8_t i = 0; i < JAM_PARAM_COUNT; i++) {
        if(i == m->sel) canvas_draw_str(canvas, 0, y, ">");
        canvas_draw_str(canvas, 8, y, rows[i]);
        y += 9;
    }
    snprintf(
        line,
        sizeof(line),
        "@ch%u %uMHz  sent %lu",
        m->cur_channel,
        2400 + m->cur_channel,
        (unsigned long)m->sent);
    canvas_draw_str(canvas, 1, 63, line);
}

static const struct {
    const char* name;
    uint8_t reg;
} diag_regs[RADIO_DIAG_REGS] = {
    {"CONFIG", REG_CONFIG},
    {"EN_AA", REG_EN_AA},
    {"EN_RXADDR", REG_EN_RXADDR},
    {"SETUP_AW", REG_SETUP_AW},
    {"SETUP_RETR", REG_SETUP_RETR},
    {"RF_CH", REG_RF_CH},
    {"RF_SETUP", REG_RF_SETUP},
    {"STATUS", REG_STATUS},
    {"OBSERVE_TX", RADIO_REG_OBSERVE_TX},
    {"RPD", REG_RPD},
    {"RX_PW_P0", RX_PW_P0},
    {"FIFO_STATUS", REG_FIFO_STATUS},
    {"DYNPD", REG_DYNPD},
    {"FEATURE", REG_FEATURE},
};

static void diag_draw(Canvas* canvas, void* model) {
    DiagModel* m = model;
    char line[40];

    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontSecondary);
    snprintf(line, sizeof(line), "Diag %s  #%lu", m->connected ? "CONNECTED" : "NO LINK",
             (unsigned long)m->refresh);
    canvas_draw_str(canvas, 1, 8, line);
    canvas_draw_line(canvas, 0, 10, 127, 10);

    uint8_t first = 0;
    if(m->sel >= RADIO_DIAG_VISIBLE) first = (uint8_t)(m->sel - RADIO_DIAG_VISIBLE + 1);

    uint8_t y = 20;
    for(uint8_t k = 0; k < RADIO_DIAG_VISIBLE; k++) {
        uint8_t idx = (uint8_t)(first + k);
        if(idx >= RADIO_DIAG_REGS) break;
        if(idx == m->sel) canvas_draw_str(canvas, 0, y, ">");
        snprintf(line, sizeof(line), "%-10s 0x%02X", diag_regs[idx].name, m->vals[idx]);
        canvas_draw_str(canvas, 8, y, line);
        y += 9;
    }
}

/* ---- MJ Scan draw ------------------------------------------------------ */

static void mjscan_draw(Canvas* canvas, void* model) {
    MjScanModel* m = model;
    char line[44];

    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontSecondary);
    snprintf(
        line,
        sizeof(line),
        "MJ Scan %s ch%u tot %lu",
        m->running ? "RUN" : "IDLE",
        m->cur_chan,
        (unsigned long)m->total);
    canvas_draw_str(canvas, 1, 8, line);
    canvas_draw_line(canvas, 0, 10, 127, 10);

    if(m->entry_count == 0) {
        canvas_draw_str(canvas, 3, 30, "no packets yet...");
        canvas_draw_str(canvas, 3, 62, "ch0..80 2Mbps CRC off/AA off");
        return;
    }

    uint8_t first = m->scroll;
    uint8_t max_first = (m->entry_count > RADIO_MJ_VISIBLE) ?
                            (uint8_t)(m->entry_count - RADIO_MJ_VISIBLE) :
                            0;
    if(first > max_first) first = max_first;

    uint8_t y = 22;
    for(uint8_t k = 0; k < RADIO_MJ_VISIBLE; k++) {
        uint8_t idx = (uint8_t)(first + k);
        if(idx >= m->entry_count) break;
        MjScanEntry* e = &m->entries[idx];

        char hex[RADIO_MJ_KEYLEN * 2 + 1];
        uint8_t p = 0;
        for(uint8_t b = 0; b < e->klen && b < RADIO_MJ_KEYLEN; b++) {
            snprintf(&hex[p], 3, "%02X", e->key[b]);
            p += 2;
        }
        hex[p] = '\0';

        if(idx == first && m->entry_count > RADIO_MJ_VISIBLE) canvas_draw_str(canvas, 0, y, ">");
        snprintf(line, sizeof(line), "%u %-10s %lu", idx + 1, hex, (unsigned long)e->count);
        canvas_draw_str(canvas, 6, y, line);
        y += 9;
    }
    snprintf(line, sizeof(line), "uniq %u/8  scroll %u", m->entry_count, first);
    canvas_draw_str(canvas, 2, 62, line);
}

/* ---- Packet Replay draw ------------------------------------------------ */

static void replay_draw(Canvas* canvas, void* model) {
    ReplayModel* m = model;
    char line[44];

    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontSecondary);

    const char* state;
    if(!m->captured)
        state = "WAITING";
    else if(m->running && m->replaying)
        state = "REPLAYING";
    else
        state = "CAPTURED";

    snprintf(line, sizeof(line), "Replay %s", state);
    canvas_draw_str(canvas, 1, 8, line);
    canvas_draw_line(canvas, 0, 10, 127, 10);

    if(!m->captured) {
        canvas_draw_str(canvas, 3, 30, "listening for packet...");
        canvas_draw_str(canvas, 3, 62, "Left/Right: clear capture");
        return;
    }

    snprintf(line, sizeof(line), "ch%u %uMHz  delay %ums", m->chan, 2400 + m->chan, m->delay_ms);
    canvas_draw_str(canvas, 2, 22, line);

    char h1[25];
    char h2[25];
    for(uint8_t b = 0; b < 12; b++) {
        snprintf(&h1[b * 2], 3, "%02X", m->payload[b]);
        snprintf(&h2[b * 2], 3, "%02X", m->payload[12 + b]);
    }
    h1[24] = '\0';
    h2[24] = '\0';
    canvas_draw_str(canvas, 2, 34, h1);
    canvas_draw_str(canvas, 2, 44, h2);

    snprintf(line, sizeof(line), "replays %lu", (unsigned long)m->count);
    canvas_draw_str(canvas, 2, 62, line);
}

/* ---- Packet Fuzzer draw ------------------------------------------------ */

static void fuzzer_draw(Canvas* canvas, void* model) {
    FuzzerModel* m = model;
    char line[44];

    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontSecondary);
    snprintf(line, sizeof(line), "Fuzzer %s", m->running ? "RUN" : "IDLE");
    canvas_draw_str(canvas, 1, 8, line);
    canvas_draw_line(canvas, 0, 10, 127, 10);

    snprintf(
        line, sizeof(line), "Pattern: %s", fuzz_pattern_names[m->pattern % RADIO_FUZZ_PATTERNS]);
    canvas_draw_str(canvas, 2, 24, line);

    snprintf(line, sizeof(line), "ch%u %uMHz  delay %ums", m->cur_channel, 2400 + m->cur_channel, m->delay_ms);
    canvas_draw_str(canvas, 2, 36, line);

    snprintf(line, sizeof(line), "sent %lu", (unsigned long)m->sent);
    canvas_draw_str(canvas, 2, 48, line);

    snprintf(line, sizeof(line), "rate %lu pkt/s  0..80", (unsigned long)m->rate);
    canvas_draw_str(canvas, 2, 60, line);
}

/* ---- RSSI meter draw --------------------------------------------------- */

static void rssi_draw(Canvas* canvas, void* model) {
    RssiModel* m = model;
    char line[44];

    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontSecondary);
    snprintf(
        line,
        sizeof(line),
        "Activity ch%u %uMHz %s",
        m->channel,
        2400 + m->channel,
        m->running ? "RUN" : "IDLE");
    canvas_draw_str(canvas, 1, 8, line);
    canvas_draw_line(canvas, 0, 10, 127, 10);

    /* horizontal level bar */
    canvas_draw_frame(canvas, 2, 18, 124, 14);
    uint8_t w = (uint8_t)(((uint16_t)m->level * 120u) / 255u);
    if(w) canvas_draw_box(canvas, 4, 20, w, 10);

    snprintf(line, sizeof(line), "Level %3u  Peak %3u", m->level, m->peak);
    canvas_draw_str(canvas, 2, 44, line);

    snprintf(line, sizeof(line), "RPD %u/%u  pk-hold %u", m->raw, RADIO_RSSI_SAMPLES, m->peak);
    canvas_draw_str(canvas, 2, 54, line);

    canvas_draw_str(canvas, 2, 62, "UD=ch  LR=reset peak");
}

/* ========================================================================== *
 *  Input handling
 * ========================================================================== */

static void radio_diag_refresh(RadioSuite* r) {
    DiagModel* m = view_get_model(r->diag_view);
    m->connected = nrf24_check_connected();
    for(uint8_t i = 0; i < RADIO_DIAG_REGS; i++) {
        m->vals[i] = nrf24_read_reg(diag_regs[i].reg);
    }
    m->refresh++;
    view_commit_model(r->diag_view, true);
}

static void radio_leave(RadioSuite* r) {
    radio_stop(r);
    nrf24_set_mode(NRF24_MODE_POWER_DOWN);
    view_dispatcher_switch_to_view(r->app->view_dispatcher, VIEW_MENU);
}

static void radio_toggle(RadioSuite* r) {
    if(r->thread) {
        radio_stop(r);
        radio_set_running(r, false);
    } else {
        radio_set_running(r, true);
        radio_start(r, radio_worker_fn(r->current));
    }
}

/* Up/Down: change the tool's tunable value. */
static void radio_change_value(RadioSuite* r, int8_t dir) {
    switch(r->current) {
    case RadioToolSpectrum: {
        SpectrumModel* m = view_get_model(r->spectrum_view);
        int v = (int)m->samples + dir;
        if(v < 1) v = 1;
        if(v > 10) v = 10;
        m->samples = (uint8_t)v;
        view_commit_model(r->spectrum_view, true);
    } break;
    case RadioToolChannelScan: {
        ChanScanModel* m = view_get_model(r->chanscan_view);
        int v = (int)m->dwell_ms + dir;
        if(v < 1) v = 1;
        if(v > 20) v = 20;
        m->dwell_ms = (uint8_t)v;
        view_commit_model(r->chanscan_view, true);
    } break;
    case RadioToolWifi: {
        WifiModel* m = view_get_model(r->wifi_view);
        int v = (int)m->dwell_ms + dir;
        if(v < 1) v = 1;
        if(v > 20) v = 20;
        m->dwell_ms = (uint8_t)v;
        view_commit_model(r->wifi_view, true);
    } break;
    case RadioToolBle: {
        BleModel* m = view_get_model(r->ble_view);
        int v = (int)m->dwell_ms + dir;
        if(v < 5) v = 5;
        if(v > 200) v = 200;
        m->dwell_ms = (uint8_t)v;
        view_commit_model(r->ble_view, true);
    } break;
    case RadioToolJammer: {
        JammerModel* m = view_get_model(r->jammer_view);
        switch(m->sel) {
        case 0:
            m->mode = (uint8_t)((m->mode + dir + 3) % 3);
            break;
        case 1: {
            int v = (int)m->power + dir;
            if(v < 0) v = 0;
            if(v > 3) v = 3;
            m->power = (uint8_t)v;
        } break;
        case 2:
            m->preset = (uint8_t)((m->preset + dir + JAM_PRESET_COUNT) % JAM_PRESET_COUNT);
            break;
        case 3: {
            int v = (int)m->custom_start + dir;
            if(v < 0) v = 0;
            if(v > (int)MAX_CHANNEL) v = MAX_CHANNEL;
            m->custom_start = (uint8_t)v;
            if(m->custom_start > m->custom_stop) m->custom_stop = m->custom_start;
        } break;
        default: {
            int v = (int)m->custom_stop + dir;
            if(v < 0) v = 0;
            if(v > (int)MAX_CHANNEL) v = MAX_CHANNEL;
            m->custom_stop = (uint8_t)v;
            if(m->custom_stop < m->custom_start) m->custom_start = m->custom_stop;
        } break;
        }
        view_commit_model(r->jammer_view, true);
    } break;
    case RadioToolMjScan: {
        MjScanModel* m = view_get_model(r->mjscan_view);
        int max_first = (m->entry_count > RADIO_MJ_VISIBLE) ?
                            (int)(m->entry_count - RADIO_MJ_VISIBLE) :
                            0;
        int v = (int)m->scroll + dir;
        if(v < 0) v = 0;
        if(v > max_first) v = max_first;
        m->scroll = (uint8_t)v;
        view_commit_model(r->mjscan_view, true);
    } break;
    case RadioToolReplay: {
        ReplayModel* m = view_get_model(r->replay_view);
        int v = (int)m->delay_ms + (int)dir * 10;
        if(v < 10) v = 10;
        if(v > 1000) v = 1000;
        m->delay_ms = (uint16_t)v;
        view_commit_model(r->replay_view, true);
    } break;
    case RadioToolFuzzer: {
        FuzzerModel* m = view_get_model(r->fuzzer_view);
        int v = (int)m->delay_ms + dir;
        if(v < 1) v = 1;
        if(v > 100) v = 100;
        m->delay_ms = (uint8_t)v;
        view_commit_model(r->fuzzer_view, true);
    } break;
    case RadioToolRssi: {
        RssiModel* m = view_get_model(r->rssi_view);
        int v = (int)m->channel + dir;
        if(v < 0) v = 0;
        if(v > (int)MAX_CHANNEL) v = MAX_CHANNEL;
        m->channel = (uint8_t)v;
        view_commit_model(r->rssi_view, true);
    } break;
    default:
        break;
    }
}

/* Left/Right: act (select parameter for the jammer, reset peaks / rescan). */
static void radio_act(RadioSuite* r, int8_t dir) {
    switch(r->current) {
    case RadioToolSpectrum: {
        SpectrumModel* m = view_get_model(r->spectrum_view);
        memset(m->peak, 0, sizeof(m->peak));
        view_commit_model(r->spectrum_view, true);
    } break;
    case RadioToolChannelScan: {
        ChanScanModel* m = view_get_model(r->chanscan_view);
        if(dir < 0) {
            memset(m->hits, 0, sizeof(m->hits));
            m->rounds = 0;
        }
        view_commit_model(r->chanscan_view, true);
    } break;
    case RadioToolWifi: {
        WifiModel* m = view_get_model(r->wifi_view);
        memset(m->level, 0, sizeof(m->level));
        m->sweeps = 0;
        view_commit_model(r->wifi_view, true);
    } break;
    case RadioToolBle: {
        BleModel* m = view_get_model(r->ble_view);
        if(dir < 0) {
            memset(m->pkts, 0, sizeof(m->pkts));
            memset(m->activity, 0, sizeof(m->activity));
        }
        view_commit_model(r->ble_view, true);
    } break;
    case RadioToolJammer: {
        JammerModel* m = view_get_model(r->jammer_view);
        m->sel = (uint8_t)((m->sel + dir + JAM_PARAM_COUNT) % JAM_PARAM_COUNT);
        view_commit_model(r->jammer_view, true);
    } break;
    case RadioToolMjScan: {
        MjScanModel* m = view_get_model(r->mjscan_view);
        if(dir < 0) {
            memset(m->entries, 0, sizeof(m->entries));
            m->entry_count = 0;
            m->total = 0;
        }
        m->scroll = 0;
        view_commit_model(r->mjscan_view, true);
    } break;
    case RadioToolReplay: {
        ReplayModel* m = view_get_model(r->replay_view);
        memset(m->payload, 0, sizeof(m->payload));
        m->chan = 0;
        m->captured = false;
        m->replaying = false;
        m->count = 0;
        view_commit_model(r->replay_view, true);
    } break;
    case RadioToolFuzzer: {
        FuzzerModel* m = view_get_model(r->fuzzer_view);
        m->pattern = (uint8_t)((m->pattern + dir + RADIO_FUZZ_PATTERNS) % RADIO_FUZZ_PATTERNS);
        view_commit_model(r->fuzzer_view, true);
    } break;
    case RadioToolRssi: {
        RssiModel* m = view_get_model(r->rssi_view);
        m->peak = 0;
        view_commit_model(r->rssi_view, true);
    } break;
    default:
        break;
    }
}

static bool radio_input(InputEvent* event, void* context) {
    RadioSuite* r = context;
    if(event->type == InputTypeLong && event->key == InputKeyOk) {
        radio_act(r, -1); /* Hold encoder: clear peaks / secondary action. */
        return true;
    }
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return false;
    if(event->type == InputTypeRepeat &&
       (event->key == InputKeyOk || event->key == InputKeyBack)) return true;

    switch(event->key) {
    case InputKeyBack:
        radio_leave(r);
        return true;
    case InputKeyOk:
        if(r->current == RadioToolDiag) {
            radio_diag_refresh(r);
        } else {
            radio_toggle(r);
        }
        return true;
    case InputKeyUp:
        if(r->current != RadioToolDiag) radio_change_value(r, -1);
        return true;
    case InputKeyDown:
        if(r->current == RadioToolDiag) {
            DiagModel* m = view_get_model(r->diag_view);
            m->sel = (uint8_t)((m->sel + 1) % RADIO_DIAG_REGS);
            view_commit_model(r->diag_view, true);
        } else {
            radio_change_value(r, 1);
        }
        return true;
    case InputKeyLeft:
        if(r->current == RadioToolDiag) {
            DiagModel* m = view_get_model(r->diag_view);
            m->sel = (uint8_t)((m->sel + RADIO_DIAG_REGS - 1) % RADIO_DIAG_REGS);
            view_commit_model(r->diag_view, true);
        } else {
            radio_act(r, -1);
        }
        return true;
    case InputKeyRight:
        radio_act(r, 1);
        return true;
    default:
        return false;
    }
}

/* ========================================================================== *
 *  View allocation / lifetime
 * ========================================================================== */

static View* radio_make_view(
    RadioSuite* r,
    uint32_t view_id,
    ViewDrawCallback draw,
    size_t model_size) {
    View* view = view_alloc();
    view_set_context(view, r);
    view_set_draw_callback(view, draw);
    view_set_input_callback(view, radio_input);
    view_allocate_model(view, ViewModelTypeLocking, model_size);
    view_dispatcher_add_view(r->app->view_dispatcher, view_id, view);
    return view;
}

void radio_suite_alloc(Nrf24Tool* app) {
    RadioSuite* r = malloc(sizeof(RadioSuite));
    memset(r, 0, sizeof(RadioSuite));
    r->app = app;
    r->current = RadioToolSpectrum;
    app->radio = r;

    r->spectrum_view = radio_make_view(r, VIEW_RADIO_SPECTRUM, spectrum_draw, sizeof(SpectrumModel));
    r->chanscan_view =
        radio_make_view(r, VIEW_RADIO_CHANSCAN, chanscan_draw, sizeof(ChanScanModel));
    r->wifi_view = radio_make_view(r, VIEW_RADIO_WIFI, wifi_draw, sizeof(WifiModel));
    r->ble_view = radio_make_view(r, VIEW_RADIO_BLE, ble_draw, sizeof(BleModel));
    r->jammer_view = radio_make_view(r, VIEW_RADIO_JAMMER, jammer_draw, sizeof(JammerModel));
    r->diag_view = radio_make_view(r, VIEW_RADIO_DIAG, diag_draw, sizeof(DiagModel));
    r->mjscan_view =
        radio_make_view(r, VIEW_RADIO_MJSCAN, mjscan_draw, sizeof(MjScanModel));
    r->replay_view =
        radio_make_view(r, VIEW_RADIO_REPLAY, replay_draw, sizeof(ReplayModel));
    r->fuzzer_view =
        radio_make_view(r, VIEW_RADIO_FUZZER, fuzzer_draw, sizeof(FuzzerModel));
    r->rssi_view = radio_make_view(r, VIEW_RADIO_RSSI, rssi_draw, sizeof(RssiModel));

    /* sensible per-screen defaults */
    SpectrumModel* sp = view_get_model(r->spectrum_view);
    sp->samples = 2;
    view_commit_model(r->spectrum_view, true);

    ChanScanModel* cs = view_get_model(r->chanscan_view);
    cs->dwell_ms = 3;
    view_commit_model(r->chanscan_view, true);

    WifiModel* wf = view_get_model(r->wifi_view);
    wf->dwell_ms = 2;
    view_commit_model(r->wifi_view, true);

    BleModel* bl = view_get_model(r->ble_view);
    bl->dwell_ms = RADIO_PROMISC_WINDOW;
    view_commit_model(r->ble_view, true);

    JammerModel* jm = view_get_model(r->jammer_view);
    jm->mode = JamModeCw;
    jm->power = 3;
    jm->preset = 0;
    jm->sel = 0;
    jm->custom_start = 0;
    jm->custom_stop = MAX_CHANNEL;
    view_commit_model(r->jammer_view, true);

    MjScanModel* mj = view_get_model(r->mjscan_view);
    mj->entry_count = 0;
    mj->scroll = 0;
    mj->total = 0;
    mj->cur_chan = 0;
    view_commit_model(r->mjscan_view, true);

    ReplayModel* rp = view_get_model(r->replay_view);
    rp->delay_ms = 100;
    rp->captured = false;
    rp->replaying = false;
    rp->count = 0;
    view_commit_model(r->replay_view, true);

    FuzzerModel* fz = view_get_model(r->fuzzer_view);
    fz->pattern = 0;
    fz->delay_ms = 2;
    fz->cur_channel = 0;
    fz->sent = 0;
    fz->rate = 0;
    view_commit_model(r->fuzzer_view, true);

    RssiModel* rs = view_get_model(r->rssi_view);
    rs->channel = 0;
    rs->level = 0;
    rs->peak = 0;
    rs->raw = 0;
    view_commit_model(r->rssi_view, true);

    radio_diag_refresh(r);
}

void radio_suite_free(Nrf24Tool* app) {
    RadioSuite* r = app->radio;
    if(!r) return;

    radio_stop(r);

    view_dispatcher_remove_view(app->view_dispatcher, VIEW_RADIO_SPECTRUM);
    view_dispatcher_remove_view(app->view_dispatcher, VIEW_RADIO_CHANSCAN);
    view_dispatcher_remove_view(app->view_dispatcher, VIEW_RADIO_WIFI);
    view_dispatcher_remove_view(app->view_dispatcher, VIEW_RADIO_BLE);
    view_dispatcher_remove_view(app->view_dispatcher, VIEW_RADIO_JAMMER);
    view_dispatcher_remove_view(app->view_dispatcher, VIEW_RADIO_DIAG);
    view_dispatcher_remove_view(app->view_dispatcher, VIEW_RADIO_MJSCAN);
    view_dispatcher_remove_view(app->view_dispatcher, VIEW_RADIO_REPLAY);
    view_dispatcher_remove_view(app->view_dispatcher, VIEW_RADIO_FUZZER);
    view_dispatcher_remove_view(app->view_dispatcher, VIEW_RADIO_RSSI);

    view_free(r->spectrum_view);
    view_free(r->chanscan_view);
    view_free(r->wifi_view);
    view_free(r->ble_view);
    view_free(r->jammer_view);
    view_free(r->diag_view);
    view_free(r->mjscan_view);
    view_free(r->replay_view);
    view_free(r->fuzzer_view);
    view_free(r->rssi_view);

    free(r);
    app->radio = NULL;
}

static void radio_reset_models(RadioSuite* r, RadioTool tool) {
    switch(tool) {
    case RadioToolSpectrum: {
        SpectrumModel* m = view_get_model(r->spectrum_view);
        memset(m->level, 0, sizeof(m->level));
        memset(m->peak, 0, sizeof(m->peak));
        m->cur = 0;
        m->running = false;
        view_commit_model(r->spectrum_view, true);
    } break;
    case RadioToolChannelScan: {
        ChanScanModel* m = view_get_model(r->chanscan_view);
        memset(m->hits, 0, sizeof(m->hits));
        m->cur = 0;
        m->rounds = 0;
        m->running = false;
        view_commit_model(r->chanscan_view, true);
    } break;
    case RadioToolWifi: {
        WifiModel* m = view_get_model(r->wifi_view);
        memset(m->level, 0, sizeof(m->level));
        m->cur_wifi = 1;
        m->sweeps = 0;
        m->running = false;
        view_commit_model(r->wifi_view, true);
    } break;
    case RadioToolBle: {
        BleModel* m = view_get_model(r->ble_view);
        memset(m->activity, 0, sizeof(m->activity));
        memset(m->pkts, 0, sizeof(m->pkts));
        m->active = 0;
        m->running = false;
        view_commit_model(r->ble_view, true);
    } break;
    case RadioToolJammer: {
        JammerModel* m = view_get_model(r->jammer_view);
        m->cur_channel = jam_preset_first(m);
        m->sent = 0;
        m->running = false;
        view_commit_model(r->jammer_view, true);
    } break;
    case RadioToolMjScan: {
        MjScanModel* m = view_get_model(r->mjscan_view);
        memset(m->entries, 0, sizeof(m->entries));
        m->entry_count = 0;
        m->scroll = 0;
        m->cur_chan = 0;
        m->total = 0;
        m->running = false;
        view_commit_model(r->mjscan_view, true);
    } break;
    case RadioToolReplay: {
        ReplayModel* m = view_get_model(r->replay_view);
        memset(m->payload, 0, sizeof(m->payload));
        m->chan = 0;
        m->captured = false;
        m->replaying = false;
        m->count = 0;
        m->running = false;
        view_commit_model(r->replay_view, true);
    } break;
    case RadioToolFuzzer: {
        FuzzerModel* m = view_get_model(r->fuzzer_view);
        m->cur_channel = 0;
        m->sent = 0;
        m->rate = 0;
        m->running = false;
        view_commit_model(r->fuzzer_view, true);
    } break;
    case RadioToolRssi: {
        RssiModel* m = view_get_model(r->rssi_view);
        m->level = 0;
        m->peak = 0;
        m->raw = 0;
        m->running = false;
        view_commit_model(r->rssi_view, true);
    } break;
    case RadioToolDiag:
    default:
        break;
    }
}

void radio_suite_enter(Nrf24Tool* app, RadioTool tool) {
    RadioSuite* r = app->radio;
    if(!r) return;

    radio_stop(r);

    if(!nrf24_test_connection(app, VIEW_MENU, radio_view_id(tool))) {
        /* disconnected: nrf24_test_connection already switched to the error dialog */
        return;
    }

    r->current = tool;
    radio_reset_models(r, tool);

    if(tool == RadioToolDiag) {
        radio_diag_refresh(r);
    } else {
        /* Open paused: inspect options before starting the radio. */
        radio_set_running(r, false);
    }
}
