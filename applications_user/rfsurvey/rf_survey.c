#include <furi.h>
#include <stdlib.h> // strtoul for the bookmark file
#include <stdarg.h>
#include <math.h> // sqrtf for the cadence jitter (CV)
#include <lib/subghz/receiver.h>
#include <lib/subghz/environment.h>
#include <lib/subghz/subghz_protocol_registry.h>
#include "rf_logic.h" // pure, host-testable: geometry + the parked burst machine

// The lab build adds a transmitter; the public build must never contain one. The switch is the
// PRESENCE of rf_lab.h, not a runtime flag, so the public binary has the TX code compiled out --
// syncing the two repos is therefore a plain file copy, with no patch to reapply on either side.
#if defined(__has_include)
#if __has_include("rf_lab.h")
#include "rf_lab.h"
#define RFS_LAB 1
#endif
#endif
#include <furi_hal_subghz.h>
#include <furi_hal_region.h>
#include <furi_hal_power.h> // external module 5V (OTG) power
#include <lib/subghz/devices/devices.h> // external radio (cc1101_ext) via the device abstraction
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/view.h>
#include <gui/elements.h>
#include <gui/modules/variable_item_list.h>
#include <storage/storage.h>
#include <furi_hal_rtc.h>
#include <notification/notification_messages.h>
#include <flipper_format/flipper_format.h>
#include <furi_hal_board_info.h> // PORT: board gate + input layout (dpad vs encoder)
#include <dialogs/dialogs.h> // PORT: "board not supported" dialog

// CC1101 preset register tables exported to apps. These set the RX filter bandwidth; the
// survey preset is user-selectable in config (same four names the stock Spectrum Analyzer /
// SubGHz use). AM650 (widest OOK) is the default -- best broadband energy pickup at wide steps.
extern const uint8_t subghz_device_cc1101_preset_ook_650khz_async_regs[];
extern const uint8_t subghz_device_cc1101_preset_ook_270khz_async_regs[];
extern const uint8_t subghz_device_cc1101_preset_2fsk_dev2_38khz_async_regs[];
extern const uint8_t subghz_device_cc1101_preset_2fsk_dev47_6khz_async_regs[];

// ---- PORT: encoder input support ------------------------------------------------------------
// The spectrum / range / capture screens use a custom ViewPort, so the firmware's automatic
// encoder handling (which covers submenu / variable_item_list only) does not apply. On encoder
// boards (T-Embed, ThorChimera) the firmware delivers a wheel turn as Up/Down, an OK-held+turn as
// Left/Right, and a spurious OK Long at the end of every hold-and-turn gesture. s_is_encoder is
// resolved once in rf_survey_app(); on D-pad / button boards every helper below is a no-op, so the
// original D-pad behaviour is byte-for-byte unchanged.
static bool s_is_encoder = false;
static uint32_t s_enc_mod_tick = 0; // furi tick of the last OK-held+turn (native Left/Right)
static bool s_enc_mod_seen = false;

// Track the OK-held+turn modifier and drop the spurious OK Long that ends it (it would otherwise
// toggle recording / confirm an edit). Returns false if the event must be ignored. No-op on D-pad.
static bool enc_prefilter(InputEvent* e) {
    if(!s_is_encoder) return true;
    if(e->key == InputKeyLeft || e->key == InputKeyRight) {
        s_enc_mod_seen = true;
        s_enc_mod_tick = furi_get_tick();
    } else if(
        e->key == InputKeyOk && e->type == InputTypeLong && s_enc_mod_seen &&
        (furi_get_tick() - s_enc_mod_tick) < furi_ms_to_ticks(600)) {
        return false;
    }
    return true;
}

typedef struct {
    const char* name;
    const uint8_t* regs;
    const char* sub; // stock Preset name written into a .sub so SubGHz can replay it
} SurveyPreset;

// available in both OFW (87.1) and Unleashed (88.9) SDKs
static const SurveyPreset PRESETS[] = {
    {"AM650", subghz_device_cc1101_preset_ook_650khz_async_regs, "FuriHalSubGhzPresetOok650Async"},
    {"AM270", subghz_device_cc1101_preset_ook_270khz_async_regs, "FuriHalSubGhzPresetOok270Async"},
    {"FM238",
     subghz_device_cc1101_preset_2fsk_dev2_38khz_async_regs,
     "FuriHalSubGhzPreset2FSKDev238Async"},
    {"FM476",
     subghz_device_cc1101_preset_2fsk_dev47_6khz_async_regs,
     "FuriHalSubGhzPreset2FSKDev476Async"},
};
#define PRESET_N COUNT_OF(PRESETS)

// config enumerations, defined up here because settings_load() validates against them
static const char* const GAIN_TXT[] = {"Auto", "Mid", "Low"};
static const char* const RADIO_TXT[] = {"Internal", "External"};
// PORT: this firmware has no external CC1101. "cc1101_ext" is registered only as an alias that
// drives the INTERNAL chip, and its is_connect() always returns true. Upstream's auto-detect
// therefore always picked "External", and the External path then powered the OTG 5 V boost for
// nothing, re-ran furi_hal_subghz_init() on every worker start (subghz_devices_begin) and called
// furi_hal_subghz_shutdown() on the internal radio on every worker stop (subghz_devices_end).
// With 0, the radio is pinned to Internal: no probe, no OTG, the row shows only "Internal", and a
// saved "Radio: 1" from an older build is ignored.
#define RFS_HAS_EXT_RADIO 0
#define RADIO_CHOICES     (RFS_HAS_EXT_RADIO ? COUNT_OF(RADIO_TXT) : 1u)
static const uint8_t SETTLE_CHOICES[] = {1, 2, 3, 5, 8, 12, 20};
#define SETTLE_N COUNT_OF(SETTLE_CHOICES)
// Trigger floor: -100..-45 dBm in 5 dB steps. Also the burst threshold for parked mode.
#define TRIG_N   12
#define TRIG_MIN -100
#define TRIG_MAX (-45)
// config rows with an action instead of a value (see conf_enter)
// Row indices, in the exact order the rows are constructed. These were off by four from
// Help onwards for a while, which made OK on "Help" and "Path loss" start a scan and, in the
// lab build, made "Help" fire the transmitter. Keep them adjacent to the construction site.
// PAGE 1 (scan): what the sweep measures. PAGE 2 (tools): one-shot actions and exports.
// Eighteen rows on a five-row screen is thirteen presses to the bottom; two pages of about
// ten is at most five. Each page has its OWN index space -- keep the constants next to the
// construction and the simulator's config walk will catch any drift.
#define CONF_ROW_TRIGGER 4
#define CONF_ROW_PATH    8
#define CONF_ROW_TRACE   9
#define CONF_ROW_TOOLS   10 // switching row

#define CONF2_ROW_CLEARPK 1
#define CONF2_ROW_WFSAVE  2
#define CONF2_ROW_BM      3
#define CONF2_ROW_SAVEBM  4
#define CONF2_ROW_PROFILE 5
#define CONF2_ROW_SAVEPRF 6
#define CONF2_ROW_HELP    7
#define CONF2_ROW_SCAN    8 // switching row
#define PROFILE_N        4 // numbered slots: no text input on this device
#ifdef RFS_LAB
#define CONF2_ROW_TXTEST  9
#define CONF2_ROW_REPLAY  10
#define CONF2_ROW_TXFRAME 11
#define CONF2_ROW_BRUTE   12
#define BRUTE_PAGE       32 // codes per press: ~3 s of airtime, never an open-ended loop
#define REP_MAX_EDGES   2048 // ~8 KB of the last capture, kept for replay
#endif

// A permissive region covering the three CC1101 bands, installed for the app's lifetime
// (restored on exit). Receiving is legal everywhere; without this, out-of-"default-range"
// frequencies (e.g. cellular 816/836) fail to tune cleanly -> the radio stalls on a 10 ms
// settle timeout per bin (whole-sweep freeze) and can furi_crash. We never transmit.
// Layout matches FuriHalRegion (flexible array) with a fixed 3-band tail.
static struct {
    char country_code[4];
    uint16_t bands_count;
    FuriHalRegionBand bands[3];
} s_region = {
    .country_code = {'W', 'W', 0, 0},
    .bands_count = 3,
    .bands =
        {
            {299999755, 348000335, 20, 100},
            {386999938, 464000000, 20, 100},
            {778999847, 928000000, 20, 100},
        },
};

// non-NULL => the external radio (cc1101_ext) is the active owner; NULL => internal furi_hal.
// Set by radio_setup(), cleared by radio_teardown(). Only the single current worker touches the
// radio (sweep and capture are strictly sequential), so this file-static has one owner at a time.
static const SubGhzDevice* s_ext = NULL;

// set the radio to freq + matching RF path for RX. Internal path avoids
// furi_hal_subghz_set_frequency_and_path (furi_crash()es on a band-gap); the external device
// folds the path into set_frequency. Returns false on an internal band-gap (caller skips the bin).
static bool tune_rx(uint32_t f) {
    if(s_ext) {
        subghz_devices_idle(s_ext);
        subghz_devices_set_frequency(s_ext, f);
        subghz_devices_set_rx(s_ext);
        return true;
    }
    furi_hal_subghz_idle();
    uint32_t real = furi_hal_subghz_set_frequency(f);
    if(real >= 299999755 && real <= 348000335)
        furi_hal_subghz_set_path(FuriHalSubGhzPath315);
    else if(real >= 386999938 && real <= 464000000)
        furi_hal_subghz_set_path(FuriHalSubGhzPath433);
    else if(real >= 778999847 && real <= 928000000)
        furi_hal_subghz_set_path(FuriHalSubGhzPath868);
    else
        return false; // out of band after quantisation -> skip, never crash
    furi_hal_subghz_rx();
    return true;
}

// Verbose debug logging: every lifecycle step, config change, worker transition and
// (coming) logging/capture op is traced so a crash on the remote device can be pinned
// from the serial `log` without a redeploy. Stream it with the CLI `log` command.
#define TAG "RFSurvey"

static void boot_trace_to(const char* name, const char* fmt, ...);

#define MAX_BINS   1024 // enough for a full 300-928 MHz sweep @650k (~967 bins); int8 -> 2 KB
#define VIEW_SPEC  0
#define VIEW_CONF  1
#define VIEW_RANGE 2 // custom start/end frequency editor
#define VIEW_CAP   3 // .sub capture screen
#define VIEW_HELP  4 // key reference
#define VIEW_CONF2 5 // second config page: the action rows
#ifdef RFS_LAB
#define PAGE_N     8 // + carrier test
#else
#define PAGE_N     7 // bars, connected, waterfall, numeric, history, cadence, hunt
#endif
#define SCR_W      128
#define RE_MIN     3000 // 300.0 MHz in 100 kHz units (the CC1101 low-band floor)
#define RE_MAX     9280 // 928.0 MHz
#define RE_SPAN    10 // minimum span: 1.0 MHz
#define BASE_Y     62 // spectrum baseline: the plot now runs to the bottom edge
#define MAX_H      48 // tallest bar
#define BODY_Y0    14 // top of the plot
#define WF_ROWS    40 // waterfall freq rows (full-bleed height)
#define WF_COLS    120 // waterfall time columns: one per completed sweep, ring
#define WF_H       49 // waterfall heatmap height; y 49..63 holds the time scale + hints
#define NBARS      32 // thick grouped bars (4px pitch) -> easy cursor stepping
#define BAR_PITCH  (SCR_W / NBARS)
#define RSSI_FLOOR -100.0f
#define RSSI_CEIL  -40.0f
#define CSV_BUF    512 // one FAT sector: CSV rows flush in chunks instead of a 5 KB line buffer

// ---- parked (zero-span) cadence -------------------------------------------
// A swept survey sees a given bin once per sweep (~1-2 s at the default step), so anything
// shorter than a sweep -- a 25 ms keyfob frame, a 100 ms repeat group -- is aliased into a
// single sample or missed outright. Parked mode stops the sweep, sits on one frequency and
// samples RSSI at PARK_SAMPLE_MS, which turns the page into a burst detector: rising edges
// give the repetition period, and the period's spread (CV) separates a crystal-locked machine
// (weather station, TPMS, sensor: CV ~ 0.00) from a human pressing a button (CV >> 0.05).
#define PARK_SAMPLE_MS 8 // ~125 Hz; 25 ms frame -> 3 samples (presence, not pulse detail)
#define PARK_N         128 // envelope ring, 1 px per sample on the 128 px screen
#define PARK_RING      RFS_PARK_RING // period + duration ring, owned by rf_logic.h
#define PARK_PROBE_MS   20 // settle after the probe retune
#define PARK_PROBE_N    25 // 25 x 8 ms = 200 ms per preset
#define PARK_AB_SPAN    200000u // moving further than this invalidates the A/B result
// Gap threshold and the burst state machine live in rf_logic.h (RFS_PARK_GAP_MS), so the PC
// test build runs the same code the firmware does.

// ---- pulse-width signature ------------------------------------------------
// The .sub capture IS the demodulated envelope, so the |duration| histogram identifies the
// encoding family without decoding it: a base pulse plus the ratio of the second cluster.
// 1:3 -> pulse-width ternary (PT2262/EV1527/CAME), 1:2 -> biphase/Manchester, 1:1 -> steady
// bit clock. Fine buckets where base pulses live (50-1010 us, 40 us), log-ish coarse above.
#define PW_FINE_LO   50
#define PW_FINE_STEP 40
#define PW_FINE_N    24 // buckets 1..24 cover 50..1009 us
#define PW_COARSE_N  14 // buckets 25..38, upper edges in PW_COARSE_EDGE
#define PW_COARSE_0  (PW_FINE_N + 1) // index of the first coarse bucket
#define PW_N         (PW_COARSE_0 + PW_COARSE_N + 1) // +1 for the >=400 ms overflow bucket

// Known sub-GHz allocations across the whole CC1101 range (global: US/EU/JP/CN/ITU),
// each with a typical modulation (AM = OOK, FM = 2-FSK). Used two ways: to tag a detected
// peak with its likely band, and as the selectable presets in the config screen. Ranges
// are NON-OVERLAPPING and ascending, so the same table works for both and a lookup just
// returns the one slot a frequency falls in. Names kept short for the 128px screen.
typedef enum {
    ModAM, // OOK (most remotes, garage, keyfobs, weather)
    ModFM, // 2-FSK (LoRa/Z-Wave, PMR/FRS voice, cellular, modern sensors)
} Mod;

typedef struct {
    uint32_t lo;
    uint32_t hi;
    const char* name;
    Mod mod;
} FreqBand;

static const FreqBand KNOWN_BANDS[] = {
    // 300-348 MHz (CC1101 low): US/Asia remotes, garages, car keys, TPMS -- mostly OOK
    {300000000, 306000000, "300 remote", ModAM},
    {306000000, 312000000, "310 garage", ModAM}, // US
    {312000000, 316500000, "315 remote", ModAM}, // US/JP/Asia car/garage/TPMS
    {316500000, 320000000, "318 garage", ModAM}, // US
    {320000000, 335000000, "330 car", ModAM}, // US/CN
    {335000000, 348000000, "345 remote", ModAM},
    // 387-464 MHz (CC1101 mid): 433 ISM (global), PMR/FRS, UK/US remotes, LMR
    {387000000, 400000000, "390 remote", ModAM}, // US
    {400000000, 433050000, "UHF SRD", ModAM},
    {433050000, 434790000, "433 ISM", ModAM}, // GLOBAL keyfob/TPMS/weather (433.92)
    {434790000, 438000000, "434 SRD", ModAM},
    {438000000, 440000000, "70cm ham", ModFM}, // amateur
    {440000000, 446000000, "UHF LMR", ModFM},
    {446000000, 446200000, "PMR446", ModFM}, // EU walkie-talkie
    {446200000, 462000000, "UHF LMR", ModFM},
    {462000000, 464000000, "FRS/GMRS US", ModFM}, // US walkie-talkie (462-467)
    // 779-928 MHz (CC1101 high): 868 EU ISM, 915 US ISM, 920 JP, 800/900 cellular
    {779000000, 787000000, "780 ISM CN", ModFM},
    {791000000, 821000000, "LTE800 DL", ModFM}, // cell tower -> you (EU b20)
    {832000000, 862000000, "LTE800 UL", ModFM}, // device -> tower (chase this)
    {863000000, 868000000, "863 SRD EU", ModFM}, // audio/alarms
    {868000000, 870000000, "868 ISM EU", ModFM}, // LoRa/Z-Wave/keyfob/meter (868.3)
    {870000000, 876000000, "SRD 870", ModFM},
    {876000000, 880000000, "GSM-R rail", ModFM},
    {880000000, 902000000, "GSM900 UL", ModFM}, // mobile uplink (EU/global)
    {902000000, 915000000, "915 ISM US", ModFM}, // US/ITU-2 LoRa/Zigbee/cordless
    {915000000, 921000000, "915 ISM US", ModFM},
    {921000000, 925000000, "920 ISM JP", ModFM}, // Japan/Asia 920 MHz band
    {925000000, 928000000, "GSM900 DL", ModFM}, // tower downlink
};


static const FreqBand* band_lookup(uint32_t f) {
    for(size_t i = 0; i < COUNT_OF(KNOWN_BANDS); i++) {
        if(f >= KNOWN_BANDS[i].lo && f < KNOWN_BANDS[i].hi) return &KNOWN_BANDS[i];
    }
    return NULL;
}

// Config "Band" selector = full-band sweeps first (incl. the whole 300-928 MHz, gaps
// auto-skipped), then every known allocation. Selecting one fills the range + modulation.
static const FreqBand FULL_BANDS[] = {
    {300000000, 928000000, "Full spectrum", ModAM}, // all three CC1101 bands end to end
    {300000000, 348000000, "Full low", ModAM},
    {387000000, 464000000, "Full mid", ModAM},
    {779000000, 928000000, "Full high", ModFM},
};
#define FULL_PRESETS (COUNT_OF(FULL_BANDS))
#define PRESET_COUNT (FULL_PRESETS + COUNT_OF(KNOWN_BANDS))

static const FreqBand* preset_at(uint16_t i) {
    return (i < FULL_PRESETS) ? &FULL_BANDS[i] : &KNOWN_BANDS[i - FULL_PRESETS];
}

// step choices (Hz) and their short labels for the config "Step" item
static const uint32_t STEP_HZ[] = {50000, 100000, 200000, 325000, 650000};
static const char* const STEP_TXT[] = {"50k", "100k", "200k", "325k", "650k"};

// time-window presets (seconds) for the connected/waterfall pages, Up/Down cycles them
static const uint16_t WIN_SECS[] = {5, 10, 15, 20, 30, 45, 60, 90, 120, 180, 300, 420, 600};
#define WIN_N       COUNT_OF(WIN_SECS)
#define WIN_DEFAULT 6 // 60 s

// "60s" / "2m" / "10m" into buf
static void fmt_win(uint16_t s, char* buf, size_t n) {
    if(s >= 60 && (s % 60) == 0)
        snprintf(buf, n, "%um", (unsigned)(s / 60));
    else
        snprintf(buf, n, "%us", (unsigned)s);
}

typedef struct {
    // scan config
    uint32_t f_start;
    uint32_t f_end;
    uint32_t f_step;
    uint16_t nbins;
    uint8_t settle_ms;
    volatile uint8_t preset_idx; // index into PRESETS[]; worker reloads the CC1101 on change
    volatile uint8_t gain; // 0 Auto(max) / 1 Mid / 2 Low -- caps the AGC's max usable gain
    volatile uint8_t radio; // 0 internal CC1101 (furi_hal) / 1 external cc1101_ext (bigger antenna)
    uint8_t preset_buf[128]; // RAM copy of a preset with AGCCTRL2 patched (for Mid/Low gain)
    volatile bool reconfig; // config changed -> worker recomputes bins + resets buffers
    volatile bool trig_cal; // one-shot: worker derives the Trigger from the noise floor
    volatile bool clear_peaks; // one-shot: worker drops the peak-hold + min-hold
    // bookmarks from /ext/apps_data/rfsurvey/freqs.txt ("name, lo_hz, hi_hz" per line)
    struct {
        char name[14];
        uint32_t lo, hi;
    } bm[8];
    uint8_t bm_n;
    volatile bool shot_auto; // capture frames to SD (heartbeat + every key event)
    bool fb_registered; // the GUI furi_checks a removal of a callback that was never added
    uint8_t shot_buf[1024]; // latest committed frame, copied under the canvas lock
    volatile uint32_t shot_seq; // bumped by the framebuffer callback
    volatile uint32_t shot_written; // last sequence written to the card
    uint32_t shot_last; // framebuffer dump throttle
    uint32_t shot_hash; // last dumped frame, so an unchanged screen costs nothing
    volatile uint8_t batt_pct; // cached: the worker polls it, the draw callback must not
    volatile bool frozen; // traces hold: the worker stops measuring and leaves the buffers alone
    uint8_t trace; // 0 Live / 1 Max hold / 2 Min hold -- what the Bars page draws
    volatile bool wf_export; // one-shot: worker writes the waterfall as a PGM between sweeps

    // scan data as int8 dBm (worker writes, GUI reads; per-byte loads/stores are
    // atomic on Cortex-M4, so a torn mix is only cosmetic). Sentinel = -128.
    int8_t rssi[MAX_BINS];
    int8_t peak[MAX_BINS];
    int8_t minv[MAX_BINS]; // min-hold: the quietest this bin has been. A channel that never
        // drops is a continuous carrier; one that does was only passing traffic. Mayhem's
        // "MinH" trace, and the cheapest complement to max-hold.
    uint32_t sweeps;

    // waterfall: one column per completed sweep (1024 bins max-folded to WF_ROWS freq rows),
    // ring over time. col_ms stamps each column's wall-clock (ms) so both new pages window by
    // real time, not sweep count. The lit/unlit cut is the user Trigger (a fixed dBm floor),
    // so it never flickers under autoscale and separates weak from strong deterministically.
    // (col_ms is not byte-atomic, but a torn read only mis-windows one column -> cosmetic.)
    int8_t wf[WF_ROWS][WF_COLS];
    uint32_t col_ms[WF_COLS];
    uint8_t wf_head;
    uint8_t wf_count;
    volatile uint8_t win_idx; // index into WIN_SECS[] (connected/waterfall window)
    volatile int8_t trigger; // dBm floor: waterfall-lit / haptic / history / capture gate
    volatile bool haptic; // vibro pulse when a bin exceeds the trigger (hands-free locate)

    // Per-bin occupancy: how many of the recent sweeps saw this bin above the trigger. The old
    // hist[] kept only ONE winner per sweep, so a strong carrier hid every weaker active
    // channel -- Codex flagged exactly that. A per-bin counter costs 1 byte per bin and the
    // answer is a real fraction, not a popularity contest. Saturating at 255 then halving every
    // counter keeps it a rolling window with no wide arithmetic.
    uint8_t occ[MAX_BINS];
    volatile uint8_t occ_n; // sweeps counted since the last halving (window length)

    // CSV logging. The worker owns the File*; the GUI (long-OK) only flips `recording`.
    // Stopped on any reconfig so the fixed-column CSV never mixes two ranges. Rows flush in
    // CSV_BUF chunks (one FAT sector) so this stays 512 B, not a 5 KB per-row line buffer.
    volatile bool recording;
    uint32_t rec_rows;
    char csv_line[CSV_BUF];

    // spectrum UI state
    uint8_t page; // 0=bars 1=connected 2=waterfall 3=numeric 4=history
    uint16_t cursor; // bars cursor column (framed); OK zooms into it
    uint8_t num_sel; // numeric page: which of the top-4 is selected (OK captures it)
    volatile bool spec_active; // true only while the spectrum view is shown (gates the haptic)
    // locate-haptic state (persists across sweeps so the filter settles; see haptic_sample/pump)
    int16_t hap_level; // peak-hold-with-decay of the selected freq's RSSI (dBm); drives rate + gate
    uint32_t hap_next; // tick at which the vibro toggles next
    bool hap_on; // vibro currently held on
    struct {
        uint32_t s, e, st;
    } zstack[4]; // zoom-out stack (previous ranges)
    uint8_t zdepth;

    FuriThread* worker;
    volatile bool running;

    NotificationApp* notif; // haptic feedback (vibro)
    Gui* gui;
    ViewDispatcher* vd;
    View* view; // spectrum
    VariableItemList* conf; // config page 1: what the sweep measures
    VariableItemList* conf2; // config page 2: one-shot actions and exports
    VariableItem* it_range; // "Range MHz" readout, updated when a band is picked
    VariableItem* it_preset; // "Modulation" preset (AM650/AM270/FM238/FM476)
    VariableItem* it_step; // "Step" -> force-synced when a band change would overflow MAX_BINS
    VariableItem* it_band; // "Band" -> re-selected after a profile load
    uint8_t profile_slot; // 0 = none, else 1..PROFILE_N
    uint8_t path_n; // 0 open / 1 urban / 2 indoor / 3 dense -- feeds the distance estimate
    VariableItem* it_trigger; // "Trigger" -> text re-synced when Auto trig moves the floor
    VariableItem* it_settle; // "Settle" -> per-bin dwell time

    // custom-range editor (VIEW_RANGE): start/end in 100 kHz units (779.0 MHz = 7790)
    View* range_view;
    uint32_t re_start, re_end;
    uint8_t re_cur; // active digit 0..3 (hundreds, tens, ones, tenths)

    // .sub capture (VIEW_CAP): a dedicated thread owns the file+stream; the ISR only feeds
    // the stream; the GUI flips `capturing`. The survey worker is stopped while capturing so
    // there is never two owners of the one radio.
    View* cap_view;
    View* help_view; // key reference; Back returns to config
    FuriThread* cap_worker;
    FuriStreamBuffer* cap_stream; // ISR -> capture thread (int32 signed durations)
    uint32_t cap_freq; // locked capture frequency (Hz)
    uint8_t cap_preset; // preset index for capture (user can toggle OOK/FSK)
    volatile bool capturing;
    volatile bool cap_gate; // Up/Down: RSSI-gate the .sub (default off = record everything).
        // Instantaneous RSSI of an OOK burst is mostly at the noise floor,
        // so gating on the survey Trigger over-trims; off records the whole
        // capture (the >=1 s / <50 us duration filter still drops junk).
    volatile bool cap_paused; // gate engaged and RSSI below trigger -> not writing
    volatile int8_t cap_rssi; // last polled RSSI for the capture screen
    volatile uint32_t cap_samples; // durations written
    volatile uint32_t cap_overflow; // ISR drops (stream full) -> capture may be corrupt
    int32_t cap_edge[RFS_FRAME_EDGES]; // first edges of the capture, for frame estimation
    uint16_t cap_edge_n;
#ifdef RFS_LAB
    // last capture's edges, kept so it can be replayed (lab builds only)
    int32_t* rep_buf;
    uint16_t rep_n;
    uint32_t rep_freq;
    uint8_t rep_preset;
    uint32_t brute_next; // next Princeton code the brutal page will start from
    // carrier test page: bounded, cancellable, and the only TX that can run for long
    volatile bool carrier_on;
    volatile bool carrier_abort;
    uint8_t carrier_secs; // index into CARRIER_SECS
    uint32_t carrier_t0;
    FuriThread* carrier_worker;
    VariableItem* it_brute;
#endif
    volatile bool cap_listen; // listen mode: decode continuously, write no file
    volatile uint16_t cap_packets; // decoded packets this session
    char cap_decode[40]; // decoded protocol string from the firmware's decoder chain
    volatile bool cap_decoded;
    uint16_t cap_pw[PW_N]; // |duration| histogram of this capture (signature line)
    volatile uint32_t cap_pw_n; // total durations histogrammed
    // ISR run-length merge: one pending (level,duration) run that has not been emitted yet.
    // A sub-50 us glitch is folded into it instead of being dropped, which is what used to
    // invert the polarity of every following edge and make the .sub unreplayable.
    volatile bool cap_run_active;
    volatile bool cap_run_level;
    volatile uint32_t cap_run_dur;
    volatile bool cap_redraw; // capture stopped on its own -> one more paint of the cap view
    FuriTimer* redraw;

    // parked (zero-span) cadence mode. The worker leaves the sweep loop entirely and sits on
    // park_freq, sampling into park_env; bursts are segmented by the user Trigger.
    volatile bool parked;
    volatile bool park_restart; // set by the GUI, consumed by the worker (single-writer stats)
    volatile uint32_t park_freq; // frequency under observation (Hz), Up/Down live-tunes it
    // one-shot modulation A/B, measured by the worker at park entry: contrast (peak above the
    // local median, dB) for OOK vs 2FSK. This is the discriminator the user used to make by
    // toggling the preset and squinting; the winner is loaded for the session.
    int8_t park_ab[2]; // [0] = best OOK contrast, [1] = best 2-FSK contrast
    uint32_t park_ab_freq; // frequency the A/B was measured at
    bool park_ab_done;
    int8_t park_env[PARK_N]; // RSSI ring (oldest at head-count), dBm
    volatile uint16_t park_head;
    volatile uint16_t park_count;
    // burst segmentation + stats: the state machine lives in rf_logic.h so it can be
    // compiled and hammered on a PC (tests/logic_test.c) with no radio attached
    RfsPark park;
} App;

static App* g_app;

static void autoscale(const App* app, int* floor, int* ceil); // used by the worker too
// defined with the config screen, needed earlier by the range editor
static uint32_t step_fit(uint32_t lo, uint32_t hi, uint32_t want);
static uint8_t step_index(uint32_t step);
static uint16_t band_index(uint32_t lo, uint32_t hi, bool* exact);

// max rssi over the bins that map to waterfall freq-row r (frequency fold is always max,
// never average -- a narrow burst must not wash out).
static int8_t wf_row_max(const App* app, int r) {
    uint32_t b0 = (uint32_t)r * app->nbins / WF_ROWS;
    uint32_t b1 = (uint32_t)(r + 1) * app->nbins / WF_ROWS;
    if(b1 <= b0) b1 = b0 + 1;
    if(b1 > app->nbins) b1 = app->nbins;
    int8_t m = -128;
    for(uint32_t b = b0; b < b1; b++)
        if(app->rssi[b] > m) m = app->rssi[b];
    return m;
}


// ---- CSV logging -----------------------------------------------------------

// Open /ext/apps_data/rfsurvey/survey_<rtc>.csv and write the metadata header. The
// columns are fixed for the file's lifetime (one per bin of the current range), so the
// worker stops recording on any reconfig rather than appending a second header.
static File* csv_open(App* app, Storage* storage) {
    storage_common_mkdir(storage, APP_DATA_PATH("")); // idempotent; ignore "exists"
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);
    char path[96];
    // tick suffix makes the name unique even for two recordings within the same RTC second,
    // so CREATE_ALWAYS can never truncate a previous file.
    snprintf(
        path,
        sizeof(path),
        APP_DATA_PATH("survey_%04u%02u%02u_%02u%02u%02u_%03lu.csv"),
        dt.year,
        dt.month,
        dt.day,
        dt.hour,
        dt.minute,
        dt.second,
        (unsigned long)(furi_get_tick() % 1000));
    File* f = storage_file_alloc(storage);
    if(!storage_file_open(f, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        FURI_LOG_E(TAG, "csv: open failed %s", path);
        storage_file_close(f);
        storage_file_free(f);
        return NULL;
    }
    int n = snprintf(
        app->csv_line,
        sizeof(app->csv_line),
        "# RF Survey\n# start=%lu end=%lu step=%lu nbins=%u settle=%u preset=%s gain=%u "
        "trigger=%d\n"
        "# rtc=%04u-%02u-%02u %02u:%02u:%02u\n"
        "# t_ms,bin0..bin%u (dBm; -128=invalid)  binfreq=start+idx*step\n",
        (unsigned long)app->f_start,
        (unsigned long)app->f_end,
        (unsigned long)app->f_step,
        app->nbins,
        app->settle_ms,
        PRESETS[app->preset_idx].name,
        (unsigned)app->gain,
        app->trigger,
        dt.year,
        dt.month,
        dt.day,
        dt.hour,
        dt.minute,
        dt.second,
        (unsigned)(app->nbins ? app->nbins - 1 : 0));
    if(storage_file_write(f, app->csv_line, n) != (size_t)n) { // SD full -> no half header
        FURI_LOG_E(TAG, "csv: header write failed");
        storage_file_close(f);
        storage_file_free(f);
        return NULL;
    }
    app->rec_rows = 0;
    FURI_LOG_I(TAG, "csv: recording -> %s", path);
    return f;
}

// flush the pending CSV chunk; returns false on a short write (SD full/failing)
static bool csv_flush(File* csv, const char* buf, int* off) {
    if(*off == 0) return true;
    bool ok = storage_file_write(csv, buf, *off) == (size_t)*off;
    *off = 0;
    return ok;
}

// smallest CC1101 RX-filter bandwidth >= step, as the MDMCFG4 top nibble (CHANBW). Used when
// zoomed so a carrier resolves to a sharp peak instead of a 650 kHz-wide mountain. 0xFF = leave
// the preset's own wide BW (for the un-zoomed survey, so nothing falls between bins).
static uint8_t bw_nibble(uint32_t step) {
    if(step <= 58000) return 0xF; // 58 kHz
    if(step <= 101000) return 0xC; // 101 kHz
    if(step <= 270000) return 0x6; // 270 kHz
    return 0xFF; // wider than 270 kHz -> keep the stock bandwidth
}

// Return the register array to load for preset p. Auto gain + wide BW (bwn==0xFF) use the stock
// array untouched; otherwise copy it into preset_buf and patch AGCCTRL2 (0x1B, gain cap) and/or
// MDMCFG4 (0x10, RX bandwidth -- keep the low data-rate nibble). RX config only.
static const uint8_t* preset_regs(App* app, uint8_t p, uint8_t gain, uint8_t bwn) {
    const uint8_t* src = PRESETS[p].regs;
    if(gain == 0 && bwn == 0xFF) return src; // nothing to patch -> stock preset
    // {0,0} terminates the reg pairs. The scan is bounded by the buffer we would copy into:
    // a table that is not terminated (a different firmware's layout, a truncated array) would
    // otherwise walk off the end and read whatever follows it in flash.
    size_t i = 0;
    while(i + 1 < sizeof(app->preset_buf) && !(src[i] == 0 && src[i + 1] == 0))
        i += 2;
    size_t total = i + 2 + 8; // pairs + terminator + 8-byte PA table
    if(total > sizeof(app->preset_buf)) return src; // too big -> fall back, never overflow
    memcpy(app->preset_buf, src, total);
    for(size_t j = 0; j < i; j += 2) {
        if(gain != 0 && app->preset_buf[j] == 0x1B) // CC1101_AGCCTRL2
            app->preset_buf[j + 1] = (gain == 1) ? 0x1F : 0xB7; // Mid / Low gain cap
        if(bwn != 0xFF && app->preset_buf[j] == 0x10) // CC1101_MDMCFG4
            app->preset_buf[j + 1] = (uint8_t)((bwn << 4) | (app->preset_buf[j + 1] & 0x0F));
    }
    return app->preset_buf;
}

// defined later; the worker needs them for the "selected frequency" haptic
static uint32_t grp_freq(const App* app, uint16_t k);
static void numeric_top4(const App* app, int tv[4], uint16_t tb[4]);

// stop the locate vibro and reset the filter. Call on every exit path (worker stop, view leave,
// capture end, haptic turned off) -- otherwise sequence_set_vibro_on holds the motor ON forever.
static void haptic_off(App* app) {
    if(app->hap_on && app->notif) notification_message(app->notif, &sequence_reset_vibro);
    app->hap_on = false;
    app->hap_level = -127;
    app->hap_next = 0;
}

#define HAP_PULSE   22 // fixed vibro pulse length (ms); only the GAP between pulses encodes strength
#define HAP_GAP_MIN 100 // at the ceiling (right on top of the source) -> ~0.1 s gap (rapid ticks)
#define HAP_GAP_MAX 4000 // at the Trigger floor -> ~4 s gap (quadratic ramp between the two)
#define HAP_CEIL    -30 // ABSOLUTE ceiling (dBm): fastest ticks here, so you can feel the source's
// very peak (CC1101 saturates near 0 dBm; a source right against the antenna reads ~-30..-20).

// Feed one FRESH RSSI reading of the selected frequency into the locate filter. Peak-hold with slow
// decay so the rate stays steady through OOK keying gaps instead of flickering; +1 so the integer
// >>3 can't stall ~7 dB short. State lives in App so it settles across sweeps.
static void haptic_sample(App* app, int8_t raw) {
    if(raw > app->hap_level) {
        app->hap_level = raw;
    } else {
        int16_t d = app->hap_level - raw;
        app->hap_level -= (int16_t)((d >> 3) + (d ? 1 : 0));
    }
}

// Non-blocking scheduler, called frequently (every few ms) by both workers so the tick cadence is
// STEADY and decoupled from the sweep period -- that is what keeps it consistent. Pure RATE cue
// (Geiger-counter): every pulse is the SAME short length, only the GAP tracks strength -- strong =
// short gap (fast, close ticks), weak = long gap (slow, far ticks). The silence gate uses the
// SMOOTHED level, not the raw read: CC1101 RSSI is noisy (+-several dB) and OOK is on/off keyed, so
// gating on the instantaneous value flaps the motor on/off around the Trigger and feels erratic.
// The peak-hold level is stable; it still falls below the Trigger within a few hundred ms once the
// signal is really gone (bounded by the decay), so "silent below Trigger" still holds.
static void haptic_pump(App* app, uint32_t now) {
    if(app->hap_level < app->trigger) {
        if(app->hap_on) {
            notification_message(app->notif, &sequence_reset_vibro);
            app->hap_on = false;
        }
        return;
    }
    if((int32_t)(now - app->hap_next) < 0) return; // wrap-safe: not time to toggle yet

    if(app->hap_on) { // end the fixed pulse, then wait a strength-dependent gap
        notification_message(app->notif, &sequence_reset_vibro);
        app->hap_on = false;
        // scale from the user Trigger (floor) up to the ABSOLUTE ceiling -30 dBm, so getting right
        // on top of a source keeps speeding up instead of pegging at trigger+30 (Trigger maxes at
        // -45, so the span is always >= 15 dB -> no divide-by-zero).
        int32_t span = HAP_CEIL - app->trigger;
        int32_t lvl = ((int32_t)(app->hap_level - app->trigger) * 255) / span;
        if(lvl < 0) lvl = 0;
        if(lvl > 255) lvl = 255;
        // quadratic in "weakness" (w = 255-lvl): the gap stays near MIN across the strong range and
        // shoots up as a square once the signal weakens -> sharper "getting warmer" as you close in.
        int32_t w = 255 - lvl;
        uint16_t gap = (uint16_t)(HAP_GAP_MIN + (HAP_GAP_MAX - HAP_GAP_MIN) * w * w / (255 * 255));
        app->hap_next = now + gap;
    } else { // start a fixed-length pulse
        notification_message(app->notif, &sequence_set_vibro_on);
        app->hap_on = true;
        app->hap_next = now + HAP_PULSE;
    }
}

// the frequency the user has SELECTED to home in on: the numeric page's chosen top-4 target,
// otherwise the bars cursor's grouped-bar center.
static uint32_t selected_freq(const App* app) {
    if(app->page == 3) {
        int tv[4];
        uint16_t tb[4];
        numeric_top4(app, tv, tb);
        if(tv[app->num_sel] > -200) return app->f_start + (uint32_t)tb[app->num_sel] * app->f_step;
    }
    return grp_freq(app, app->cursor);
}

// ---- radio abstraction (internal furi_hal vs external cc1101_ext) ----------
// One worker owns the radio at a time (sweep and capture never run concurrently -- each is
// joined before the other starts), so OTG power and the device session bracket that one owner.

// bring the selected radio up for the worker that is starting. External: power the module's 5V
// (OTG, retried like the stock subghz app), init + fetch + begin the device; if it's absent or
// disconnected, fall back to internal so we never call a subghz_devices_* on NULL (that furi_checks).
static void radio_setup(App* app) {
    s_ext = NULL;
    if(!RFS_HAS_EXT_RADIO) app->radio = 0; // PORT: internal only (see RFS_HAS_EXT_RADIO)
    if(app->radio == 1) {
        for(uint8_t i = 0; i < 5 && !furi_hal_power_is_otg_enabled(); i++) {
            furi_hal_power_enable_otg();
            furi_delay_ms(10);
        }
        subghz_devices_init();
        const SubGhzDevice* d = subghz_devices_get_by_name("cc1101_ext");
        if(d && subghz_devices_is_connect(d)) {
            subghz_devices_begin(d);
            subghz_devices_reset(d);
            subghz_devices_idle(d);
            s_ext = d;
            FURI_LOG_I(TAG, "radio: external cc1101_ext up");
            return;
        }
        subghz_devices_deinit(); // not found/connected -> undo and fall back to internal
        if(furi_hal_power_is_otg_enabled()) furi_hal_power_disable_otg();
        app->radio = 0;
        FURI_LOG_W(TAG, "radio: external not found -> internal");
    }
    furi_hal_subghz_reset();
}

// true if an external cc1101_ext module is physically present (brief powered probe, then undone).
// Used once at startup to auto-default the radio to External when the board is plugged in.
static bool radio_external_present(void) {
    // PORT: the alias "cc1101_ext" always reports connected; never probe (it would also toggle OTG
    // and reset the internal radio).
    if(!RFS_HAS_EXT_RADIO) return false;
    for(uint8_t i = 0; i < 5 && !furi_hal_power_is_otg_enabled(); i++) {
        furi_hal_power_enable_otg();
        furi_delay_ms(10);
    }
    subghz_devices_init();
    const SubGhzDevice* d = subghz_devices_get_by_name("cc1101_ext");
    bool present = d && subghz_devices_is_connect(d);
    subghz_devices_deinit();
    if(furi_hal_power_is_otg_enabled()) furi_hal_power_disable_otg();
    return present;
}

// tear the active radio down at the owning worker's exit (idempotent).
static void radio_teardown(void) {
    if(s_ext) {
        subghz_devices_sleep(s_ext);
        subghz_devices_end(s_ext);
        subghz_devices_deinit();
        s_ext = NULL;
        if(furi_hal_power_is_otg_enabled()) furi_hal_power_disable_otg();
    } else {
        furi_hal_subghz_idle();
        furi_hal_subghz_sleep();
    }
}

static void radio_load_preset(const uint8_t* regs) {
    if(s_ext) {
        subghz_devices_idle(s_ext);
        subghz_devices_load_preset(s_ext, FuriHalSubGhzPresetCustom, (uint8_t*)regs);
    } else {
        furi_hal_subghz_idle();
        furi_hal_subghz_load_custom_preset((uint8_t*)regs);
    }
}

static float radio_rssi(void) {
    return s_ext ? subghz_devices_get_rssi(s_ext) : furi_hal_subghz_get_rssi();
}

// set a fixed RX frequency WITHOUT starting sync rx (for .sub async capture).
static void radio_set_freq(uint32_t f) {
    if(s_ext) {
        subghz_devices_idle(s_ext);
        subghz_devices_set_frequency(s_ext, f);
        return;
    }
    furi_hal_subghz_idle();
    furi_hal_subghz_set_frequency(f);
    if(f >= 299999755 && f <= 348000335)
        furi_hal_subghz_set_path(FuriHalSubGhzPath315);
    else if(f >= 386999938 && f <= 464000000)
        furi_hal_subghz_set_path(FuriHalSubGhzPath433);
    else
        furi_hal_subghz_set_path(FuriHalSubGhzPath868);
}

static void radio_async_start(void* cb, void* ctx) {
    if(s_ext)
        subghz_devices_start_async_rx(s_ext, cb, ctx);
    else
        furi_hal_subghz_start_async_rx(cb, ctx);
}

static void radio_async_stop(void) {
    if(s_ext)
        subghz_devices_stop_async_rx(s_ext);
    else
        furi_hal_subghz_stop_async_rx();
}

// path loss exponent x10: the single biggest source of error in any RSSI distance number, so
// it is the user's choice rather than a hidden constant
static const uint8_t PATH_N10[] = {20, 27, 35, 45};

// CC1101 RSSI needs a few symbol periods to settle after a retune; below this the reading is
// biased low and a weak carrier reads as noise. Rough but honest, and monotonic in bandwidth.
static uint8_t settle_min_ms(uint32_t step) {
    if(step <= 100000) return 4; // narrow filter, longest settling
    if(step <= 325000) return 2;
    return 1;
}

// ---- pulse-width signature helpers ----------------------------------------

// upper edges of the coarse buckets (us), log-ish, 1010 -> 400 ms
static const uint32_t PW_COARSE_EDGE[PW_COARSE_N] = {
    1200, 1600, 2500, 4000, 6300, 10000, 16000, 25000,
    40000, 63000, 100000, 160000, 250000, 400000};

// duration (us) -> histogram bucket: 0 = <50 us, 1..24 = fine (40 us wide, 50..1009 us),
// 25..38 = coarse, 39 = overflow (>=400 ms).
static uint8_t pw_bucket(uint32_t d) {
    if(d < PW_FINE_LO) return 0;
    uint32_t k = 1 + (d - PW_FINE_LO) / PW_FINE_STEP;
    if(k <= PW_FINE_N) return (uint8_t)k; // 1..24: 40 us wide, 50..1009 us
    for(uint8_t i = 0; i < PW_COARSE_N; i++)
        if(d < PW_COARSE_EDGE[i]) return (uint8_t)(PW_COARSE_0 + i); // 25..38
    return (uint8_t)(PW_COARSE_0 + PW_COARSE_N); // 39: >= 400 ms
}

// bucket -> representative duration (us), the value shown on the capture screen.
// uint32_us: the coarse tail reaches 400 ms, which overflows 16 bits by design.
static uint32_t pw_center(uint8_t b) {
    if(b == 0) return PW_FINE_LO / 2;
    if(b <= PW_FINE_N) return (uint32_t)(PW_FINE_LO + (b - 1) * PW_FINE_STEP + PW_FINE_STEP / 2);
    if(b < PW_COARSE_0) return PW_FINE_LO; // unreachable, keeps the math obviously total
    uint8_t i = (uint8_t)(b - PW_COARSE_0);
    if(i >= PW_COARSE_N) return 500000u; // 400 ms+ overflow bucket
    uint32_t lo = (i == 0) ? 1010u : PW_COARSE_EDGE[i - 1];
    return (lo + PW_COARSE_EDGE[i]) / 2;
}


// ---- parked (zero-span) cadence -------------------------------------------


// Contrast of one preset at one frequency: peak RSSI above the median of a short window.
// Measured with the preset's OWN bandwidth (bwn = 0xFF), because forcing a common filter would
// erase exactly the AM650-vs-AM270 distinction the number is meant to expose.
//
// This is a HINT, not a decision. RSSI measures channel power, not modulation, so a sustained
// carrier scores ~0 on both families and a one-off button press may land in neither window.
// The value of the probe is that it saves the user a preset-toggle round trip when the signal
// happens to be repeating; it must never silently choose the radio's settings for them.
static int8_t park_probe(App* app, uint8_t preset) {
    if(preset >= PRESET_N) return 0;
    radio_load_preset(preset_regs(app, preset, app->gain, 0xFF));
    if(!tune_rx(app->park_freq)) return 0;
    furi_delay_ms(PARK_PROBE_MS);
    int8_t v[PARK_PROBE_N];
    for(int i = 0; i < PARK_PROBE_N; i++) {
        float rf = radio_rssi();
        v[i] = (rf < -127.0f) ? (int8_t)-127 : (int8_t)rf;
        furi_delay_ms(PARK_SAMPLE_MS);
    }
    for(int i = 1; i < PARK_PROBE_N; i++) { // insertion sort (15 elements)
        int8_t k = v[i];
        int j = i - 1;
        while(j >= 0 && v[j] > k) {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = k;
    }
    int c = (int)v[PARK_PROBE_N - 1] - (int)v[PARK_PROBE_N / 2]; // peak - median
    if(c < 0) c = 0;
    if(c > 60) c = 60;
    return (int8_t)c;
}

// Clear the per-session stats. WORKER THREAD ONLY: the GUI used to call this from the input
// callback while park_loop was still sampling, which tore every counter (and made the duty
// divisor transiently zero). The GUI now only sets park_restart and lets the owner reset.
static void park_clear(App* app) {
    rfs_park_clear(&app->park);
    app->park_head = 0;
    app->park_count = 0;
    app->park_ab[0] = 0;
    app->park_ab[1] = 0;
    app->park_ab_freq = 0;
    app->park_ab_done = false;
    memset(app->park_env, -128, sizeof(app->park_env));
}

// GUI thread: pick the target frequency and ask the worker to start a fresh session. Only
// these two writes happen off the worker, and both are single-word.
static void park_begin(App* app) {
    app->park_freq = selected_freq(app);
    app->park_restart = true;
    app->parked = true;
}

// Parked sessions log their own file. A cadence log is a time series at ONE frequency
// (t_ms,rssi) at the sample rate, not a per-bin sweep row, so it can never share the sweep
// CSV -- and it is the input the offline analysis actually wants: burst edges, period
// histogram, duty, all recoverable at 125 Hz instead of one sample per sweep.
static File* cadence_open(App* app, Storage* storage) {
    storage_common_mkdir(storage, APP_DATA_PATH("")); // idempotent; ignore "exists"
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);
    char path[96];
    snprintf(
        path,
        sizeof(path),
        APP_DATA_PATH("cadence_%04u%02u%02u_%02u%02u%02u_%03lu.csv"),
        dt.year,
        dt.month,
        dt.day,
        dt.hour,
        dt.minute,
        dt.second,
        (unsigned long)(furi_get_tick() % 1000));
    File* f = storage_file_alloc(storage);
    if(!storage_file_open(f, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_close(f);
        storage_file_free(f);
        FURI_LOG_E(TAG, "cadence csv: open failed %s", path);
        return NULL;
    }
    uint8_t pi = app->preset_idx < PRESET_N ? app->preset_idx : 0;
    int n = snprintf(
        app->csv_line,
        CSV_BUF,
        "# RF Survey cadence\n# freq=%lu preset=%s sample_ms=%u trigger=%d gain=%u\n"
        "# rtc=%04u-%02u-%02u %02u:%02u:%02u\n# t_ms,rssi\n",
        (unsigned long)app->park_freq,
        PRESETS[pi].name,
        (unsigned)PARK_SAMPLE_MS,
        app->trigger,
        (unsigned)app->gain,
        dt.year,
        dt.month,
        dt.day,
        dt.hour,
        dt.minute,
        dt.second);
    if(storage_file_write(f, app->csv_line, n) != (size_t)n) {
        storage_file_close(f);
        storage_file_free(f);
        FURI_LOG_E(TAG, "cadence csv: header failed");
        return NULL;
    }
    app->rec_rows = 0;
    FURI_LOG_I(TAG, "cadence csv: %s", path);
    return f;
}

// Runs instead of the sweep while app->parked. Owns the radio (the sweep worker brought it up),
// so it loads a narrowed filter once and thereafter only retunes when park_freq changes (the
// user can walk the frequency while parked and watch RSSI rise). Returns when parked clears or
// the app stops -- the caller then re-syncs the sweep's preset cache.
static void park_loop(App* app, Storage* storage) {
    if(app->park_restart) { // consume the GUI's request on the owning thread
        app->park_restart = false;
        park_clear(app);
    }
    uint8_t p = app->preset_idx;
    if(p >= PRESET_N) p = 0;
    radio_load_preset(preset_regs(app, p, app->gain, bw_nibble(100000)));
    uint32_t cur_f = 0;
    // cadence CSV: lines accumulate in app->csv_line (the sweep's row buffer, idle while
    // parked) and flush a sector at a time, so we are not doing 125 FAT writes a second.
    File* cf = NULL;
    int coff = 0;
    while(app->running && app->parked) {
        uint32_t f = app->park_freq;
        if(f != cur_f) {
            if(!furi_hal_subghz_is_frequency_valid(f) || !tune_rx(f)) {
                FURI_LOG_W(TAG, "park: %lu Hz untunable -> sweep", (unsigned long)f);
                app->parked = false;
                break;
            }
            cur_f = f;
            furi_delay_ms(2); // let the PLL + RSSI settle after the retune
            FURI_LOG_I(TAG, "park: on %lu Hz", (unsigned long)f);
            // decide OOK vs 2-FSK on arrival, and again if the user walks the frequency far
            // enough that the old answer is about a different signal
            uint32_t moved = (cur_f > app->park_ab_freq) ? cur_f - app->park_ab_freq :
                                                           app->park_ab_freq - cur_f;
            if(!app->park_ab_done || moved > PARK_AB_SPAN) {
                app->park_ab[0] = park_probe(app, 0); // AM650
                app->park_ab[1] = park_probe(app, 2); // FM238
                app->park_ab_done = true;
                app->park_ab_freq = cur_f;
                // the probe left the radio on FM238 -> put the parked filter back. The user's
                // preset is untouched: the numbers are reported, not acted on.
                radio_load_preset(preset_regs(app, p, app->gain, bw_nibble(100000)));
                tune_rx(f);
                furi_delay_ms(2);
                FURI_LOG_I(TAG, "park ab: AM %d FM %d (hint only)", app->park_ab[0], app->park_ab[1]);
            }
        }
        uint32_t now = furi_get_tick();
        float rf = radio_rssi();
        int8_t r = (rf < -127.0f) ? (int8_t)-127 : (int8_t)rf;
        // the same strength-tracked vibro the sweep uses: rate rises with the carrier, so you
        // can walk toward a source without looking at the screen
        if(app->haptic && app->notif) {
            haptic_sample(app, r);
            haptic_pump(app, now);
        } else if(app->hap_on) {
            haptic_off(app);
        }
        app->park_env[app->park_head] = r;
        app->park_head = (uint16_t)((app->park_head + 1) % PARK_N);
        if(app->park_count < PARK_N) app->park_count++;
        // reconcile the file handle with the user's recording flag, like the sweep does
        if(app->recording && !cf) {
            cf = cadence_open(app, storage);
            if(!cf) app->recording = false; // open failed -> don't get stuck "recording"
            coff = 0;
        } else if(!app->recording && cf) {
            if(coff) storage_file_write(cf, app->csv_line, (size_t)coff);
            storage_file_close(cf);
            storage_file_free(cf);
            cf = NULL;
            FURI_LOG_I(TAG, "cadence csv: stopped, %lu rows", (unsigned long)app->rec_rows);
        }
        if(cf) {
            coff += snprintf(
                app->csv_line + coff, (size_t)(CSV_BUF - coff), "%lu,%d\n", (unsigned long)now, r);
            app->rec_rows++;
            if(coff >= CSV_BUF - 24) { // keep room for the next line
                if(storage_file_write(cf, app->csv_line, (size_t)coff) != (size_t)coff) {
                    storage_file_close(cf);
                    storage_file_free(cf);
                    cf = NULL;
                    app->recording = false;
                    FURI_LOG_E(TAG, "cadence csv: write failed");
                }
                coff = 0;
            }
        }
        // one call into the pure state machine: gap handling, burst segmentation, repeat
        // grouping and the stats rings all live in rf_logic.h so tests/logic_test.c exercises
        // the exact same code the firmware runs
        rfs_park_feed(&app->park, now, r, app->trigger);

        furi_delay_ms(PARK_SAMPLE_MS);
    }
    rfs_park_stop(&app->park); // mid-burst stop: the elapsed time is a lower bound, not data
    haptic_off(app); // never leave the motor latched when parked mode ends
    if(cf) { // flush + close whatever the user left open
        if(coff) storage_file_write(cf, app->csv_line, (size_t)coff);
        storage_file_close(cf);
        storage_file_free(cf);
        FURI_LOG_I(TAG, "cadence csv: closed on park exit, %lu rows", (unsigned long)app->rec_rows);
    }
    FURI_LOG_I(
        TAG, "park: exit, %u bursts, %u gaps", (unsigned)app->park.bursts, (unsigned)app->park.gaps);
}

// ---- bookmarks ------------------------------------------------------------
// One line per bookmark: "name, start_hz, end_hz". A hand-editable file beats another menu:
// the interesting ranges are personal (a specific gate, a specific meter), and hardcoding 26
// bands was never going to cover them.
// one 1 MHz channel's occupancy aggregate: the unit the History page and the auto-bookmark
// writer both think in
typedef struct {
    uint16_t mhz;
    uint32_t sum;
    uint16_t cnt;
} OccBucket;

#define BM_PATH APP_DATA_PATH("freqs.txt") // hand-written, never touched by the app
#define BM_AUTO APP_DATA_PATH("freqs_auto.txt") // regenerated from measured occupancy

static void bm_parse(App* app, char* line) {
    if(!line[0] || line[0] == '#' || app->bm_n >= COUNT_OF(app->bm)) return;
    char* c1 = strchr(line, ',');
    if(!c1) return;
    *c1++ = 0;
    char* c2 = strchr(c1, ',');
    if(!c2) return;
    *c2++ = 0;
    char* end = NULL;
    unsigned long vlo = strtoul(c1, &end, 10);
    if(end == c1 || (*end && *end != ' ' && *end != '\t' && *end != '\r')) return;
    unsigned long vhi = strtoul(c2, &end, 10);
    if(end == c2 || (*end && *end != ' ' && *end != '\t' && *end != '\r')) return;
    if(vlo > 0xFFFFFFFFul || vhi > 0xFFFFFFFFul) return; // strtoul overflows differently per ABI
    uint32_t lo = (uint32_t)vlo;
    uint32_t hi = (uint32_t)vhi;
    if(lo < 300000000u || hi > 928000000u || hi <= lo) return; // same gate as settings_load
    size_t l = strlen(line);
    while(l && (line[l - 1] == ' ' || line[l - 1] == '\t'))
        line[--l] = 0;
    snprintf(app->bm[app->bm_n].name, sizeof(app->bm[0].name), "%s", line);
    app->bm[app->bm_n].lo = lo;
    app->bm[app->bm_n].hi = hi;
    app->bm_n++;
}

static void bookmarks_load_file(App* app, Storage* storage, const char* path) {
    File* f = storage_file_alloc(storage);
    if(!storage_file_open(f, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        storage_file_free(f);
        return; // no file is a normal state, not an error
    }
    char line[96];
    size_t len = 0;
    char c;
    while(storage_file_read(f, &c, 1) == 1) {
        if(c == '\n') {
            line[len] = 0;
            bm_parse(app, line);
            len = 0;
            if(app->bm_n >= COUNT_OF(app->bm)) break;
        } else if(c != '\r' && len + 1 < sizeof(line)) {
            line[len++] = c;
        }
    }
    if(len && app->bm_n < COUNT_OF(app->bm)) {
        line[len] = 0;
        bm_parse(app, line);
    }
    storage_file_close(f);
    storage_file_free(f);
}

static void bookmarks_load(App* app, Storage* storage) {
    app->bm_n = 0;
    bookmarks_load_file(app, storage, BM_PATH); // the user's own list first
    bookmarks_load_file(app, storage, BM_AUTO); // then whatever the last scan measured
    FURI_LOG_I(TAG, "bookmarks: %u", (unsigned)app->bm_n);
}

// Top channels by measured occupancy, written as bookmarks. Deliberately a SEPARATE file: the
// app must never rewrite a list the user hand-edited, and the file is regenerated wholesale
// each time so stale entries cannot accumulate.
static void bookmarks_write_auto(App* app, Storage* storage, const OccBucket* b, uint8_t n) {
    File* f = storage_file_alloc(storage);
    if(!storage_file_open(f, BM_AUTO, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_close(f);
        storage_file_free(f);
        return;
    }
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);
    int w = snprintf(
        app->csv_line,
        CSV_BUF,
        "# RF Survey auto bookmarks %04u-%02u-%02u %02u:%02u  (n=%u sweeps)\n"
        "# name, start_hz, end_hz -- one 1 MHz channel each, by measured occupancy\n",
        dt.year,
        dt.month,
        dt.day,
        dt.hour,
        dt.minute,
        (unsigned)app->occ_n);
    if(w > 0 && w < CSV_BUF) storage_file_write(f, app->csv_line, (size_t)w);
    for(uint8_t k = 0; k < n; k++) {
        uint32_t lo = (uint32_t)b[k].mhz * 1000000u;
        w = snprintf(
            app->csv_line,
            CSV_BUF,
            "b%u, %lu, %lu\n",
            (unsigned)b[k].mhz,
            (unsigned long)lo,
            (unsigned long)(lo + 999999u));
        if(w > 0 && w < CSV_BUF) storage_file_write(f, app->csv_line, (size_t)w);
    }
    storage_file_close(f);
    storage_file_free(f);
}

// ---- settings persistence -------------------------------------------------
// One small file so a field session does not start by re-entering the same six values. Uses
// FlipperFormat (the .sub container already linked) instead of hand-rolled parsing: it gives a
// version header and typed reads, and a corrupt/half-written file just fails the version check
// and falls back to the in-code defaults.
#define SETTINGS_PATH APP_DATA_PATH("settings.txt")
#define SETTINGS_VER  1

static void settings_save(App* app, Storage* storage, const char* path) {
    FlipperFormat* ff = flipper_format_file_alloc(storage);
    bool ok = ff && flipper_format_file_open_always(ff, path) &&
              flipper_format_write_header_cstr(ff, "Flipper RF Survey Settings", SETTINGS_VER);
    if(ok) {
        uint32_t v;
        v = app->f_start;
        ok = flipper_format_write_uint32(ff, "Start", &v, 1);
        v = app->f_end;
        ok = ok && flipper_format_write_uint32(ff, "End", &v, 1);
        v = app->f_step;
        ok = ok && flipper_format_write_uint32(ff, "Step", &v, 1);
        int32_t t = app->trigger;
        ok = ok && flipper_format_write_int32(ff, "Trigger", &t, 1);
        v = app->preset_idx;
        ok = ok && flipper_format_write_uint32(ff, "Preset", &v, 1);
        v = app->gain;
        ok = ok && flipper_format_write_uint32(ff, "Gain", &v, 1);
        v = app->radio;
        ok = ok && flipper_format_write_uint32(ff, "Radio", &v, 1);
        v = app->haptic ? 1 : 0;
        ok = ok && flipper_format_write_uint32(ff, "Haptic", &v, 1);
        v = app->settle_ms;
        ok = ok && flipper_format_write_uint32(ff, "Settle", &v, 1);
        v = app->win_idx;
        ok = ok && flipper_format_write_uint32(ff, "Window", &v, 1);
        if(!ok) FURI_LOG_W(TAG, "settings: write failed");
    }
    flipper_format_free(ff);
}

// Every field is optional: a missing or out-of-range value keeps the default set in the app
// init, so an older settings file (or a truncated one) degrades instead of misconfiguring.
//
// Each read is preceded by flipper_format_rewind: the reader advances a cursor and a MISSING
// key leaves it at EOF (Confirmed against the OFW 87 and Unleashed 88 FlipperFormat
// implementations). Without the rewind, one dropped field would silently discard every field
// after it -- a stale file would load as "half the settings are defaults" with no error.
#define SETTINGS_READ(fn, key, dst, cnt) \
    (flipper_format_rewind(ff), fn(ff, key, dst, cnt))

static void settings_load(App* app, Storage* storage, const char* path) {
    FlipperFormat* ff = flipper_format_file_alloc(storage);
    FuriString* hdr = furi_string_alloc();
    uint32_t ver = 0, v;
    bool ok = ff && flipper_format_file_open_existing(ff, path) &&
              flipper_format_read_header(ff, hdr, &ver) &&
              furi_string_cmp_str(hdr, "Flipper RF Survey Settings") == 0 && ver == SETTINGS_VER;
    if(ok) {
        uint32_t start = app->f_start, end = app->f_end, step = app->f_step;
        bool have_range = SETTINGS_READ(flipper_format_read_uint32, "Start", &start, 1) &&
                          SETTINGS_READ(flipper_format_read_uint32, "End", &end, 1) &&
                          SETTINGS_READ(flipper_format_read_uint32, "Step", &step, 1);
        // accept only a range inside the CC1101 window with a sane step: a corrupt or
        // hand-edited file must never make the nbins / f_end arithmetic overflow later
        if(have_range && start >= 300000000 && end <= 928000000 && end > start &&
           step >= 10000 && step <= 2000000) {
            app->f_start = start;
            app->f_end = end;
            app->f_step = step;
        }
        int32_t t = app->trigger;
        if(SETTINGS_READ(flipper_format_read_int32, "Trigger", &t, 1) && t >= -100 && t <= -45)
            app->trigger = (int8_t)t;
        if(SETTINGS_READ(flipper_format_read_uint32, "Preset", &v, 1) && v < PRESET_N)
            app->preset_idx = (uint8_t)v;
        if(SETTINGS_READ(flipper_format_read_uint32, "Gain", &v, 1) && v < COUNT_OF(GAIN_TXT))
            app->gain = (uint8_t)v;
        if(SETTINGS_READ(flipper_format_read_uint32, "Radio", &v, 1) && v < RADIO_CHOICES)
            app->radio = (uint8_t)v;
        if(SETTINGS_READ(flipper_format_read_uint32, "Haptic", &v, 1))
            app->haptic = v ? true : false;
        if(SETTINGS_READ(flipper_format_read_uint32, "Settle", &v, 1) && v >= 1 && v <= 20)
            app->settle_ms = (uint8_t)v;
        if(SETTINGS_READ(flipper_format_read_uint32, "Window", &v, 1) && v < WIN_N)
            app->win_idx = (uint8_t)v;
        FURI_LOG_I(TAG, "settings: loaded");
    } else {
        FURI_LOG_I(TAG, "settings: none/foreign, using defaults");
    }
    furi_string_free(hdr);
    flipper_format_free(ff);
}

// Waterfall snapshot as a binary PGM. Deliberately one fixed mapping and no options: the point
// is a file a report can embed, not an image editor. Written by the SWEEP WORKER between
// sweeps (never from the GUI) so the ring cannot move under the exporter, and chronologically
// ordered from the oldest column so the file is a picture of the session, not of the ring.
// Resolution honesty: rows are max-folded from up to 1024 bins into WF_ROWS, and one column is
// one sweep, so this is a coarse activity map -- the header says so.
#define WF_GRAY_LO (-125) // dBm mapped to gray 1
#define WF_GRAY_HI (-40) // dBm mapped to gray 255
#define WF_GRAY_SPAN (WF_GRAY_HI - WF_GRAY_LO)

// Ring index of the k-th column, oldest first. wf_head is the NEWEST written column, so the
// oldest of `cnt` columns is (wf_head - cnt + 1) mod WF_COLS -- getting this wrong by one
// exports an unwritten slot when cnt == 1 and rotates the chronology when the ring is full.
static uint16_t wf_col_at(const App* app, uint16_t cnt, uint16_t k) {
    return rfs_wf_col_at(app->wf_head, WF_COLS, cnt, k); // rf_logic.h
}

static void wf_export_pgm(App* app, Storage* storage) {
    uint16_t cnt = app->wf_count;
    if(cnt == 0) {
        FURI_LOG_W(TAG, "wf export: nothing captured yet");
        return;
    }
    storage_common_mkdir(storage, APP_DATA_PATH(""));
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);
    char path[96];
    snprintf(
        path,
        sizeof(path),
        APP_DATA_PATH("waterfall_%04u%02u%02u_%02u%02u%02u.pgm"),
        dt.year,
        dt.month,
        dt.day,
        dt.hour,
        dt.minute,
        dt.second);
    File* f = storage_file_alloc(storage);
    if(!f || !storage_file_open(f, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        FURI_LOG_E(TAG, "wf export: open failed");
        if(f) {
            storage_file_close(f);
            storage_file_free(f);
        }
        return;
    }
    uint8_t pi = app->preset_idx < PRESET_N ? app->preset_idx : 0;
    int n = snprintf(
        app->csv_line,
        CSV_BUF,
        "P5\n"
        "# RF Survey waterfall snapshot\n"
        "# rtc %04u-%02u-%02u %02u:%02u:%02u\n"
        "# range %lu.%03lu-%lu.%03lu MHz step %lu preset %s\n"
        "# x=time (oldest first, one column per sweep)  y=frequency (%lu at top, %lu bottom)\n"
        "# rows are max-folded from up to %u bins -> a coarse activity map, not a spectrum\n"
        "# gray 1..255 = %d..%d dBm, 0 = no data\n"
        "# columns %u\n",
        dt.year,
        dt.month,
        dt.day,
        dt.hour,
        dt.minute,
        dt.second,
        (unsigned long)(app->f_start / 1000000),
        (unsigned long)((app->f_start / 1000) % 1000),
        (unsigned long)(app->f_end / 1000000),
        (unsigned long)((app->f_end / 1000) % 1000),
        (unsigned long)app->f_step,
        PRESETS[pi].name,
        (unsigned long)(app->f_start / 1000000),
        (unsigned long)(app->f_end / 1000000),
        (unsigned)MAX_BINS,
        WF_GRAY_LO,
        WF_GRAY_HI,
        (unsigned)cnt);
    // every write is checked: a full SD would otherwise leave a truncated PGM behind and the
    // log line would still claim success
    bool ok = (n > 0 && n < CSV_BUF) && storage_file_write(f, app->csv_line, (size_t)n) == (size_t)n;

    // column timestamps, 12 per comment line: the sweep interval is not constant, so the image
    // alone cannot tell a report reader how fast time runs
    for(uint16_t k = 0; ok && k < cnt; k += 12) {
        int off = snprintf(app->csv_line, CSV_BUF, "# t_ms");
        for(uint16_t j = k; j < k + 12 && j < cnt; j++)
            off += snprintf(
                app->csv_line + off,
                (size_t)(CSV_BUF - off),
                " %lu",
                (unsigned long)app->col_ms[wf_col_at(app, cnt, j)]);
        off += snprintf(app->csv_line + off, (size_t)(CSV_BUF - off), "\n");
        ok = off > 0 && off < CSV_BUF &&
             storage_file_write(f, app->csv_line, (size_t)off) == (size_t)off;
    }
    if(ok) {
        n = snprintf(app->csv_line, CSV_BUF, "%u %d\n255\n", (unsigned)cnt, WF_ROWS);
        ok = n > 0 && n < CSV_BUF && storage_file_write(f, app->csv_line, (size_t)n) == (size_t)n;
    }

    uint8_t row[WF_COLS];
    for(int r = 0; ok && r < WF_ROWS; r++) {
        for(uint16_t k = 0; k < cnt; k++) {
            int v = app->wf[r][wf_col_at(app, cnt, k)];
            if(v == -128) {
                row[k] = 0; // band gap / not measured
                continue;
            }
            if(v < WF_GRAY_LO) v = WF_GRAY_LO;
            if(v > WF_GRAY_HI) v = WF_GRAY_HI;
            row[k] = (uint8_t)(1 + (v - WF_GRAY_LO) * 254 / WF_GRAY_SPAN);
        }
        ok = storage_file_write(f, (const char*)row, cnt) == (size_t)cnt;
    }
    storage_file_close(f);
    storage_file_free(f);

    // The same snapshot as text. A PGM cannot be diffed, plotted or pasted into a report; this
    // can. Columns are time (oldest first, one per sweep), rows are frequency.
    if(ok) {
        char cpath[96];
        snprintf(cpath, sizeof(cpath), "%s", path);
        char* dot = strrchr(cpath, '.');
        if(dot) {
            snprintf(dot, 5, ".csv");
            File* cf = storage_file_alloc(storage);
            if(cf && storage_file_open(cf, cpath, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
                int m = snprintf(
                    app->csv_line,
                    CSV_BUF,
                    "# RF Survey waterfall %04u-%02u-%02u %02u:%02u:%02u\n"
                    "# columns = time (oldest first), rows = frequency %lu..%lu MHz\n"
                    "# y=0 is the lowest frequency row; -128 = no data\n"
                    "row,t_ms,%s\n",
                    dt.year,
                    dt.month,
                    dt.day,
                    dt.hour,
                    dt.minute,
                    dt.second,
                    (unsigned long)(app->f_start / 1000000),
                    (unsigned long)(app->f_end / 1000000),
                    "rssi");
                if(m > 0 && m < CSV_BUF) storage_file_write(cf, app->csv_line, (size_t)m);
                for(int r = 0; r < WF_ROWS; r++) {
                    int off = snprintf(app->csv_line, CSV_BUF, "%d", r);
                    for(uint16_t k = 0; k < cnt && off < CSV_BUF - 8; k++)
                        off += snprintf(
                            app->csv_line + off,
                            (size_t)(CSV_BUF - off),
                            ",%d",
                            app->wf[r][wf_col_at(app, cnt, k)]);
                    if(off < CSV_BUF - 1) app->csv_line[off++] = '\n';
                    storage_file_write(cf, app->csv_line, (size_t)off);
                }
                storage_file_close(cf);
                FURI_LOG_I(TAG, "wf export csv: %s", cpath);
            }
            if(cf) storage_file_free(cf);
        }
    }

    if(ok)
        FURI_LOG_I(TAG, "wf export: %s (%u cols)", path, (unsigned)cnt);
    else
        FURI_LOG_E(TAG, "wf export: write failed (SD full?)");
}

// Numbered slots, not named ones: this device has no keyboard, and a profile you cannot type a
// name for is still worth having when the alternative is re-entering six values.
static void profile_path(char* buf, size_t n, uint8_t slot) {
    snprintf(buf, n, APP_DATA_PATH("profile%u.txt"), (unsigned)slot);
}

// Re-read every row from app state. Called after a profile load, where the values changed
// behind the config screen's back.
static void conf_sync_rows(App* app) {
    if(app->it_range) {
        char r[24];
        snprintf(
            r,
            sizeof(r),
            "%lu-%lu",
            (unsigned long)(app->f_start / 1000000),
            (unsigned long)(app->f_end / 1000000));
        variable_item_set_current_value_text(app->it_range, r);
    }
    if(app->it_step) {
        uint8_t si = step_index(app->f_step);
        variable_item_set_current_value_index(app->it_step, si);
        variable_item_set_current_value_text(app->it_step, STEP_TXT[si]);
    }
    if(app->it_preset) {
        uint8_t pi = app->preset_idx < PRESET_N ? app->preset_idx : 0;
        variable_item_set_current_value_index(app->it_preset, pi);
        variable_item_set_current_value_text(app->it_preset, PRESETS[pi].name);
    }
    if(app->it_trigger) {
        variable_item_set_current_value_index(
            app->it_trigger, (uint8_t)((app->trigger - TRIG_MIN) / 5));
        char t[8];
        snprintf(t, sizeof(t), "%d", app->trigger);
        variable_item_set_current_value_text(app->it_trigger, t);
    }
    if(app->it_band) {
        bool exact = false;
        uint16_t bi = band_index(app->f_start, app->f_end, &exact);
        variable_item_set_current_value_index(app->it_band, bi);
        variable_item_set_current_value_text(app->it_band, exact ? preset_at(bi)->name : "custom");
    }
}

// ---- radio sweep worker ----------------------------------------------------

static int32_t sweep_worker(void* ctx) {
    App* app = ctx;
    boot_trace_to("w0.txt", "entered free=%u", (unsigned)memmgr_get_free_heap());
    FURI_LOG_I(TAG, "worker: start, radio setup");
    radio_setup(app);
    boot_trace_to("w1.txt", "radio ok ext=%d", s_ext ? 1 : 0);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* csv = NULL; // open while recording; owned entirely by this thread
    uint8_t cur_preset = 0xFF, cur_gain = 0xFF, cur_bw = 0x00; // force a load on the first loop
    uint32_t hap_last =
        0; // last locate-haptic sample time; persists across sweeps (not per-sweep,
    // or a short zoomed sweep would resample every bin instead of every 30 ms)

    while(app->running) {
        // parked mode owns the radio instead of the sweep: one frequency, one sample clock.
        // A parked session produces no per-bin columns, so an open CSV has to be closed here
        // (its column count is fixed for the file's lifetime).
        if(app->parked) {
            if(csv) { // close the sweep file; recording carries over into a cadence CSV
                storage_file_close(csv);
                storage_file_free(csv);
                csv = NULL;
                FURI_LOG_I(TAG, "csv: stopped for park, %lu rows", (unsigned long)app->rec_rows);
            }
            park_loop(app, storage);
            cur_preset = 0xFF; // force the sweep to reload its wide survey filter
            cur_bw = 0x00;
            app->reconfig = true; // parked left stale bins + waterfall behind
            continue;
        }
        if(app->frozen) { // HOLD: measure nothing, touch nothing -- the traces stay put
            furi_delay_ms(50);
            continue;
        }
        // (re)load the CC1101 preset on a preset/gain change OR a zoom bandwidth change. When
        // zoomed we narrow the RX filter to ~the step so a carrier shows a sharp peak, not a
        // 650 kHz mountain; un-zoomed keeps the wide survey BW. Snapshot once each.
        uint8_t p = app->preset_idx, g = app->gain;
        if(p >= PRESET_N) p = 0;
        uint8_t bwn = (app->zdepth > 0) ? bw_nibble(app->f_step) : 0xFF;
        if(cur_preset != p || cur_gain != g || cur_bw != bwn) {
            radio_load_preset(preset_regs(app, p, g, bwn));
            cur_preset = p;
            cur_gain = g;
            cur_bw = bwn;
            FURI_LOG_I(
                TAG, "worker: preset -> %s gain %u bw %02X", PRESETS[p].name, (unsigned)g, bwn);
        }
        // apply a pending config change: recompute bins for the new range/step and
        // clear the buffers so stale readings from the old range don't linger
        if(app->reconfig) {
            // a range change invalidates an open CSV's fixed columns -> stop cleanly
            if(csv) {
                storage_file_close(csv);
                storage_file_free(csv);
                csv = NULL;
                app->recording = false;
                FURI_LOG_I(
                    TAG, "csv: stopped by reconfig, %lu rows", (unsigned long)app->rec_rows);
            }
            if(app->f_step == 0) app->f_step = 650000; // never divide by zero
            if(app->f_end < app->f_start) app->f_end = app->f_start;
            uint32_t n = (app->f_end - app->f_start) / app->f_step + 1;
            if(n > MAX_BINS) n = MAX_BINS;
            if(n < 1) n = 1;
            app->nbins = (uint16_t)n;
            // keep f_end honest: after the MAX_BINS cap (or a non-dividing step) the last bin
            // scanned is f_start+(nbins-1)*step, so pin the displayed end to it (labels can't lie)
            app->f_end = app->f_start + (uint32_t)(app->nbins - 1) * app->f_step;
            for(uint16_t i = 0; i < app->nbins; i++) {
                app->rssi[i] = -128;
                app->peak[i] = -128;
                app->minv[i] = 127;
                app->occ[i] = 0;
            }
            app->occ_n = 0;
            app->sweeps = 0;
            app->wf_head = 0;
            app->wf_count = 0;
            memset(app->wf, -128, sizeof(app->wf)); // waterfall history is per-range
            memset(app->col_ms, 0, sizeof(app->col_ms));
            app->occ_n = 0; // occupancy window is per-range too
            app->reconfig = false;
            FURI_LOG_I(
                TAG,
                "worker: reconfig start=%lu end=%lu step=%lu nbins=%u",
                (unsigned long)app->f_start,
                (unsigned long)app->f_end,
                (unsigned long)app->f_step,
                app->nbins);
        }
        // reconcile the file handle with the GUI's recording flag (long-OK toggles it)
        if(app->recording && !csv) {
            csv = csv_open(app, storage);
            if(!csv) app->recording = false; // open failed -> don't get stuck "recording"
        } else if(!app->recording && csv) {
            storage_file_close(csv);
            storage_file_free(csv);
            csv = NULL;
            FURI_LOG_I(TAG, "csv: stopped, %lu rows", (unsigned long)app->rec_rows);
        }
        // snapshot the range for this sweep: a mid-sweep config change (GUI) then only
        // takes effect next loop via reconfig, so one row is never a torn A/B mix.
        uint32_t fs = app->f_start, st = app->f_step;
        int8_t smax = -128; // this sweep's strongest bin (unused for now, kept for the log)
        (void)smax;
        uint16_t valid = 0; // bins actually measured this sweep (0 -> range is all band-gap)
        for(uint16_t i = 0; i < app->nbins && app->running; i++) {
            uint32_t f = fs + (uint32_t)i * st;
            if(furi_hal_subghz_is_frequency_valid(f) && tune_rx(f)) {
                valid++;
                furi_delay_ms(app->settle_ms);
                float rf = radio_rssi();
                int8_t r = (rf < -127.0f) ? (int8_t)-127 : (int8_t)rf; // keep -128 as the sentinel
                app->rssi[i] = r;
                if(r > app->peak[i]) app->peak[i] = r;
                if(r < app->minv[i]) app->minv[i] = r;
                if(r > app->trigger && app->occ[i] < 255) app->occ[i]++;
                if(r > smax) smax = r;
            } else {
                app->rssi[i] = -128; // sentinel: band gap / invalid / un-tunable
            }
            // Service the locate haptic DURING the sweep (not after it) so the ticks stay steady --
            // this is what kills the "fast burst then ~0.6 s silence" beat. Sample the SELECTED
            // frequency ~every 30 ms (responsive gate + rate; one extra tune, the next bin retunes
            // anyway) and pump the scheduler every bin. Pump is non-blocking: it only toggles the
            // motor when its own timer elapses, so it never stalls the sweep.
            if(app->haptic && app->notif && app->spec_active) {
                uint32_t tn = furi_get_tick();
                if(tn - hap_last >= 30) {
                    uint32_t sf = selected_freq(app);
                    if(furi_hal_subghz_is_frequency_valid(sf) && tune_rx(sf)) {
                        furi_delay_ms(app->settle_ms);
                        float hrf = radio_rssi();
                        haptic_sample(app, (hrf < -127.0f) ? (int8_t)-127 : (int8_t)hrf);
                    } else {
                        haptic_sample(
                            app, -127); // selected freq in a band gap -> decay to silence
                    }
                    hap_last = tn;
                }
                haptic_pump(app, furi_get_tick());
            } else if(app->hap_on) {
                // spectrum left or haptic disabled while a pulse was latched on -> release it here.
                // This worker is the SOLE owner of the motor (spec_exit only clears spec_active), so
                // there is no cross-thread write to race and the motor can never stay stuck on.
                haptic_off(app);
            }
            // Yield every bin so the GUI/input threads get time even on the invalid-frequency
            // path (band gaps take the else branch above with no settle delay -> tight spin that
            // starved input). Valid bins already yield via furi_delay_ms(settle_ms); this also
            // covers settle_ms == 0. One cooperative yield per bin, negligible cost.
            furi_thread_yield();
        }
        // an all-band-gap range (e.g. a custom 350-360 MHz) measures zero bins and would otherwise
        // spin this loop at 100% CPU with no settle delay -- back off so it can't busy-wait.
        if(!valid && app->running) furi_delay_ms(80);
        // commit this sweep as one waterfall column: max-fold the bins to WF_ROWS freq rows,
        // timestamp it (one col == one sweep, so the real time resolution is the sweep period
        // -- honest, no fabricated sub-bins). The lit cut is the user Trigger, applied at draw.
        {
            uint8_t h = (uint8_t)((app->wf_head + 1) % WF_COLS);
            for(int r = 0; r < WF_ROWS; r++)
                app->wf[r][h] = wf_row_max(app, r);
            app->col_ms[h] = furi_get_tick();
            app->wf_head = h;
            if(app->wf_count < WF_COLS) app->wf_count++;
        }
        app->sweeps++;
        if(app->sweeps == 1) boot_trace_to("w2.txt", "first sweep done nbins=%u", (unsigned)app->nbins);
        if(app->wf_export) { // one-shot from the config screen, run by the ring's owner
            app->wf_export = false;
            wf_export_pgm(app, storage);
        }
        if((app->sweeps & 0x0F) == 0) {
            // the draw callback must not hit the fuel gauge every 150 ms, so the worker caches it
            app->batt_pct = furi_hal_power_get_pct();
            FURI_LOG_D(TAG, "worker: sweep %lu", (unsigned long)app->sweeps);
        }

        // one-shot: derive the Trigger from this sweep's own noise floor. The 25th percentile
        // bounds the floor without being dragged up by the handful of strong bins, and +8 dB
        // sits clear of it. Done here (not in the GUI) so the radio has one owner.
        if(app->trig_cal) {
            app->trig_cal = false;
            uint16_t h[128] = {0};
            uint16_t n = 0;
            for(uint16_t i = 0; i < app->nbins; i++) {
                int8_t v = app->rssi[i];
                if(v == -128) continue;
                int b = (int)v + 128;
                if(b < 0) b = 0;
                if(b > 127) b = 127;
                h[b]++;
                n++;
            }
            if(n) {
                uint32_t want = ((uint32_t)n + 3) / 4, acc = 0; // ceil(n/4), never 0
                int floor_dbm = TRIG_MIN;
                for(int b = 0; b < 128; b++) {
                    acc += h[b];
                    if(acc >= want) {
                        floor_dbm = b - 128;
                        break;
                    }
                }
                int t = floor_dbm + 8;
                if(t < TRIG_MIN) t = TRIG_MIN;
                if(t > TRIG_MAX) t = TRIG_MAX;
                app->trigger = (int8_t)t;
                FURI_LOG_I(TAG, "trig cal: floor %d -> %d", floor_dbm, t);
            }
        }
        if(app->clear_peaks) { // one-shot: re-seed peak-hold from the current sweep
            app->clear_peaks = false;
            for(uint16_t i = 0; i < app->nbins; i++) {
                app->peak[i] = app->rssi[i];
                app->minv[i] = app->rssi[i];
            }
            FURI_LOG_I(TAG, "peaks cleared");
        }

        // rolling occupancy window: bump the counters this sweep, halve the whole set when the
        // window fills so `occ[i] / occ_n` is always "fraction of recent sweeps"
        if(app->occ_n < 255) {
            app->occ_n++;
        } else {
            for(uint16_t i = 0; i < app->nbins; i++)
                app->occ[i] = (uint8_t)(app->occ[i] >> 1);
            app->occ_n = (uint8_t)((app->occ_n >> 1) + 1);
        }

        // log one CSV row for this sweep: "t_ms,rssi0,..,rssiN", flushed in CSV_BUF chunks
        // (one FAT sector) so the row buffer stays 512 B instead of ~5 KB.
        if(csv) {
            int off = snprintf(app->csv_line, CSV_BUF, "%lu", (unsigned long)furi_get_tick());
            bool ok = true;
            for(uint16_t i = 0; i < app->nbins; i++) {
                if(off > CSV_BUF - 8) { // keep room for the next ",-128" + newline
                    if(!(ok = csv_flush(csv, app->csv_line, &off))) break;
                }
                off += snprintf(app->csv_line + off, CSV_BUF - off, ",%d", app->rssi[i]);
            }
            if(ok && off < CSV_BUF - 1) app->csv_line[off++] = '\n';
            if(ok) ok = csv_flush(csv, app->csv_line, &off);
            if(!ok) { // SD full / failing -> stop cleanly, don't spin
                FURI_LOG_E(
                    TAG, "csv: write failed, stopping at %lu rows", (unsigned long)app->rec_rows);
                storage_file_close(csv);
                storage_file_free(csv);
                csv = NULL;
                app->recording = false;
            } else {
                app->rec_rows++;
            }
        }
    }

    if(csv) { // app closing mid-record -> flush + close
        storage_file_close(csv);
        storage_file_free(csv);
        FURI_LOG_I(TAG, "csv: closed on exit, %lu rows", (unsigned long)app->rec_rows);
    }
    furi_record_close(RECORD_STORAGE);
    haptic_off(app); // never leave the locate motor latched on
    FURI_LOG_I(TAG, "worker: exit, radio teardown");
    radio_teardown();
    return 0;
}

// Writes a one-line trace to boot.txt. The device's log level can be reset between CLI
// sessions, so a file is the only evidence that survives; this is how the screenshot path was
// debugged.
static void boot_trace_to(const char* name, const char* fmt, ...) {
    // PORT: upstream wrote a boot/debug trace file to SD on every call (w0..w2, t_heap*, t_boot,
    // t_timer, t_fb*). Those are developer artifacts that just spam apps_data on the device, so the
    // body is disabled here. Kept as a no-op (not deleted) so the ~10 call sites still compile.
    UNUSED(name);
    UNUSED(fmt);
    return;
#if 0
    Storage* st = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(st, APP_DATA_PATH(""));
    File* f = storage_file_alloc(st);
    char path[72];
    snprintf(path, sizeof(path), APP_DATA_PATH("%s"), name);
    if(f && storage_file_open(f, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        char buf[160];
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        if(n > 0) storage_file_write(f, buf, (size_t)n);
        storage_file_close(f);
    }
    if(f) storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
#endif
}

// ---- on-device screenshots -------------------------------------------------
// RPC is not an option for a screenshot: an RPC session takes ~20 KB of the FAP's heap and
// this app needs ~15 KB just to start, so a remote capture is an OOM crash on a deep screen.
// Instead the app dumps its own framebuffer to a file the moment the page changes, and a host
// script reads it over the CLI. One 1 KB write per page flip.

// 128x64 at 1 bpp, u8g2 page layout (vertical_top_lsb): one byte per column of eight vertical
// pixels, so a row of the file is 16 bytes = 32 hex characters.

static void dump_frame(const uint8_t* fb, uint8_t page) {
    if(!fb) return;
    Storage* st = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(st, APP_DATA_PATH("frames"));
    // one file per distinct frame, named by wall clock, so the sequence survives a crash and can
    // be sorted on the host. The newest file is always the last thing the device drew.
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);
    char path[72];
    snprintf(
        path,
        sizeof(path),
        APP_DATA_PATH("frames/f_%02u%02u%02u_%02u.hex"),
        dt.hour,
        dt.minute,
        dt.second,
        (unsigned)(furi_get_tick() % 100));
    File* f = storage_file_alloc(st);
    if(f && storage_file_open(f, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        char line[40];
        for(int y = 0; y < 64; y++) {
            for(int b = 0; b < 16; b++)
                snprintf(line + b * 2, 3, "%02x", fb[y * 16 + b]);
            line[32] = '\n';
            storage_file_write(f, line, 33);
        }
        storage_file_close(f);
    }
    if(f) storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
    (void)page;
}

// Fires on every committed frame, whoever drew it -- including the firmware's own widgets, which
// is why the config screens are capturable at all.
//
// This runs with the canvas mutex held (canvas_commit iterates its callback array under the
// lock), so it must NEVER touch storage: an SD write here stalls the GUI thread with the canvas
// locked and the watchdog reboots the device. It only copies 1 KB into RAM and bumps a counter.
static void fb_callback(uint8_t* data, size_t size, CanvasOrientation orientation, void* context) {
    UNUSED(orientation);
    App* app = context;
    if(!app || !data || size != 1024) {
        boot_trace_to("t_fb_bad.txt", "size=%u data=%p", size, (void*)data);
        return;
    }
    if(!app->shot_auto) return;
    memcpy(app->shot_buf, data, 1024);
    if(app->shot_seq == 0) boot_trace_to("t_fb.txt", "first frame, size=%u", (unsigned)size);
    app->shot_seq++;
}

// Writes the newest copied frame. Called from the redraw timer (500 ms heartbeat) and from the
// input handlers (so the screen BEFORE a key press is on the card even if the press crashes the
// app). Never called from the framebuffer callback.
static void shot_flush(App* app, bool force);
__attribute__((unused)) static void shot_flush(App* app, bool force) {
    uint32_t now = furi_get_tick();
    if(!force && now - app->shot_last < 500) return;
    if(app->shot_written == app->shot_seq && !force) return; // nothing new on screen
    if(app->parked) return; // a write mid-measurement is the hole the cadence page warns about
    app->shot_written = app->shot_seq;
    app->shot_last = now;
    dump_frame(app->shot_buf, 0);
}

// ---- bars view -------------------------------------------------------------

// bar height for a dBm against a dynamic [floor,ceil] window (autoscale)
static int bar_h(int dbm, int floor, int ceil) {
    if(ceil <= floor) ceil = floor + 1;
    if(dbm < floor) dbm = floor;
    if(dbm > ceil) dbm = ceil;
    return (dbm - floor) * MAX_H / (ceil - floor);
}

// autoscale the vertical window to the data in view, so the noise floor shows texture
// and weak signals aren't clamped flat. min 15 dB window.
static void autoscale(const App* app, int* floor, int* ceil) {
    int lo = 127, hi = -128;
    for(uint16_t i = 0; i < app->nbins; i++) {
        int8_t v = app->rssi[i];
        if(v == -128) continue; // sentinel / band gap
        if(v < lo) lo = v;
        if(v > hi) hi = v;
    }
    if(hi < lo) {
        lo = (int)RSSI_FLOOR;
        hi = (int)RSSI_CEIL;
    }
    if(hi - lo < 15) {
        hi = lo + 15;
    }
    *floor = lo;
    *ceil = hi;
}

// center frequency of grouped bar k (0..NBARS-1)
static uint32_t grp_freq(const App* app, uint16_t k) {
    uint32_t bin = ((uint32_t)k * 2 + 1) * app->nbins / (NBARS * 2);
    if(bin >= app->nbins) bin = app->nbins ? app->nbins - 1 : 0;
    return app->f_start + bin * app->f_step;
}

// max rssi/peak over the bins of grouped bar k
static void grp_max(const App* app, uint16_t k, int8_t* cur, int8_t* pk) {
    uint32_t b0 = (uint32_t)k * app->nbins / NBARS;
    uint32_t b1 = (uint32_t)(k + 1) * app->nbins / NBARS;
    if(b1 <= b0) b1 = b0 + 1;
    if(b1 > app->nbins) b1 = app->nbins;
    int8_t c = -128, p = -128;
    for(uint32_t b = b0; b < b1; b++) {
        if(app->rssi[b] > c) c = app->rssi[b];
        if(app->peak[b] > p) p = app->peak[b];
    }
    *cur = c;
    *pk = p;
}

static void spec_redraw(App* app) {
    with_view_model(app->view, void** m, { UNUSED(m); }, true);
}

static void zoom_in(App* app, uint16_t k) {
    if(app->zdepth >= COUNT_OF(app->zstack)) return;
    uint32_t fc = grp_freq(app, k); // center of the framed grouped bar
    uint32_t span = app->f_end - app->f_start;
    uint32_t ns_span = span / 5;
    if(ns_span < 2000000) ns_span = 2000000; // don't zoom below ~2 MHz
    uint32_t ns = (fc > ns_span / 2) ? (fc - ns_span / 2) : 300000000;
    uint32_t ne = ns + ns_span;
    if(ns < 300000000) ns = 300000000;
    if(ne > 928000000) {
        ne = 928000000;
        ns = (ne > ns_span) ? (ne - ns_span) : 300000000;
    }
    app->zstack[app->zdepth].s = app->f_start;
    app->zstack[app->zdepth].e = app->f_end;
    app->zstack[app->zdepth].st = app->f_step;
    app->zdepth++;
    app->f_start = ns;
    app->f_end = ne;
    uint32_t st = app->f_step / 4;
    if(st < 10000) st = 10000; // finer step, min 10 kHz
    app->f_step = st;
    app->cursor = NBARS / 2;
    app->reconfig = true;
    FURI_LOG_I(
        TAG,
        "zoom in -> %lu-%lu step=%lu depth=%u",
        (unsigned long)ns,
        (unsigned long)ne,
        (unsigned long)st,
        app->zdepth);
}

static void zoom_out(App* app) {
    if(!app->zdepth) return;
    app->zdepth--;
    app->f_start = app->zstack[app->zdepth].s;
    app->f_end = app->zstack[app->zdepth].e;
    app->f_step = app->zstack[app->zdepth].st;
    app->cursor = NBARS / 2;
    app->reconfig = true;
    FURI_LOG_I(TAG, "zoom out depth=%u", app->zdepth);
}


static void draw_bars(Canvas* canvas, App* app, int floor, int ceil) {
    // the Trigger IS the baseline: only signal that clears the floor draws a bar, and it gets the
    // full height. Same floor the waterfall/haptic/history use. Keep the autoscaled top (>= floor+15
    // so there's always a sane window when nothing is strong yet).
    UNUSED(floor);
    floor = app->trigger;
    if(ceil < floor + 15) ceil = floor + 15;
    for(uint16_t k = 0; k < NBARS; k++) {
        int8_t cur, pk;
        grp_max(app, k, &cur, &pk);
        // Trace selector: Live shows the current sweep, Max/Min draw the held extremes. The
        // opposite extreme stays on as a thin line so you always see the spread.
        int8_t shown = cur;
        if(app->trace == 1) shown = pk;
        if(app->trace == 2) {
            uint32_t b0 = (uint32_t)k * app->nbins / NBARS;
            uint32_t b1 = (uint32_t)(k + 1) * app->nbins / NBARS;
            if(b1 <= b0) b1 = b0 + 1;
            if(b1 > app->nbins) b1 = app->nbins;
            int8_t mn = 127;
            for(uint32_t b = b0; b < b1; b++)
                if(app->minv[b] < mn && app->minv[b] != -128) mn = app->minv[b];
            if(mn != 127) shown = mn;
        }
        int x = k * BAR_PITCH;
        int h = bar_h(shown, floor, ceil);
        if(h > 0) canvas_draw_box(canvas, x, BASE_Y - h, BAR_PITCH - 1, h);
        int ph = bar_h(pk, floor, ceil);
        if(ph > 0) canvas_draw_line(canvas, x, BASE_Y - ph, x + BAR_PITCH - 2, BASE_Y - ph);
    }
    // solid baseline = the Trigger floor (bars rise from here; below it is nothing)
    canvas_draw_line(canvas, 0, BASE_Y + 1, SCR_W - 1, BASE_Y + 1);
    // wide cursor frame around the selected grouped bar (the band you'd zoom into)
    int cx = (int)app->cursor * BAR_PITCH;
    if(cx < 1) cx = 1;
    canvas_draw_frame(canvas, cx - 1, BODY_Y0 - 2, BAR_PITCH + 1, BASE_Y - BODY_Y0 + 3);
}

// avg + peak of waterfall freq-row r over the columns captured within the last win_sec.
// One column == one sweep, so the mean of those columns IS the true per-sweep window mean
// (no "average of per-slot maxima" fudge); peak = max over the same columns.
static void wf_window_stats(const App* app, int r, uint32_t now, int* avg, int8_t* pk) {
    uint32_t win_ms = (uint32_t)WIN_SECS[app->win_idx] * 1000;
    int sum = 0, cnt = 0;
    int8_t p = -128;
    for(int c = 0; c < WF_COLS; c++) {
        uint32_t cm = app->col_ms[c];
        if(cm == 0 || now - cm > win_ms) continue; // unwritten or outside the window
        int8_t v = app->wf[r][c];
        if(v == -128) continue; // band gap / invalid
        sum += v;
        cnt++;
        if(v > p) p = v;
    }
    *avg = cnt ? sum / cnt : -128;
    *pk = p;
}

// Connected page: per-frequency AVERAGE over the window as a solid polyline, plus the window
// peak as a dotted envelope above it. "Is this frequency consistently there, and how hot does
// it get?" -- a steadier read than the instantaneous bars.
static void draw_connected(Canvas* canvas, App* app, int floor, int ceil) {
    uint32_t now = furi_get_tick();
    int px_prev = -1, y_prev = 0;
    for(int r = 0; r < WF_ROWS; r++) {
        int avg;
        int8_t pk;
        wf_window_stats(app, r, now, &avg, &pk);
        int x = r * (SCR_W - 1) / (WF_ROWS - 1);
        if(avg > -128) {
            int y = BASE_Y - bar_h(avg, floor, ceil);
            if(px_prev >= 0) canvas_draw_line(canvas, px_prev, y_prev, x, y);
            px_prev = x;
            y_prev = y;
        } else {
            px_prev = -1; // break the line across a gap
        }
        if(pk > -128) canvas_draw_dot(canvas, x, BASE_Y - bar_h(pk, floor, ceil));
    }
    canvas_draw_line(canvas, 0, BASE_Y + 1, SCR_W - 1, BASE_Y + 1);
}

// Horizontal waterfall: X = time (oldest left, newest right), Y = frequency (start at top,
// end at bottom). Full-bleed above the pills. A cell is lit if that sweep's row is above the
// current user Trigger (changing it re-lights all history instantly). Columns are placed by
// real timestamp over
// the last win_sec, nearest-earlier sample per screen x -> fills width honestly (repeats a
// real sweep when sweeps are slower than 1px, never invents sub-sweep detail). Shows WHEN a
// band is busy and the spacing of bursts. Edge labels give the range; center gives the span.
static void draw_waterfall(Canvas* canvas, App* app) {
    uint16_t win = WIN_SECS[app->win_idx];
    uint32_t newest = app->col_ms[app->wf_head];
    if(newest) {
        uint32_t span = (uint32_t)win * 1000;
        uint32_t oldest = (newest > span) ? newest - span : 0;
        for(int x = 0; x < SCR_W; x++) {
            // (newest-oldest) <= 600 s * 1000 = 6e5; *127 = 7.6e7 < 2^32, so 32-bit math is
            // exact here -- no 64-bit software divide per column.
            uint32_t t = oldest + (newest - oldest) * (uint32_t)x / (SCR_W - 1);
            int best = -1;
            uint32_t bestt = 0;
            for(int c = 0; c < WF_COLS; c++) {
                uint32_t cm = app->col_ms[c];
                if(cm == 0 || cm > t) continue;
                if(best < 0 || cm > bestt) {
                    best = c;
                    bestt = cm;
                }
            }
            if(best < 0) continue;
            for(int r = 0; r < WF_ROWS; r++) {
                if(app->wf[r][best] > app->trigger) { // lit only above the user floor
                    int y0 = r * WF_H / WF_ROWS;
                    int y1 = (r + 1) * WF_H / WF_ROWS;
                    if(y1 <= y0) y1 = y0 + 1;
                    canvas_draw_box(canvas, x, y0, 1, y1 - y0);
                }
            }
        }
    }
    // freq labels over the heatmap (XOR so they read on lit or dark cells); kept inside the
    // heatmap band (y < WF_H) so they never collide with the scale/pills below
    char s[12];
    canvas_set_color(canvas, ColorXOR);
    snprintf(s, sizeof(s), "%lu", (unsigned long)(app->f_start / 1000000));
    canvas_draw_str(canvas, 1, 8, s); // top edge = start freq
    snprintf(s, sizeof(s), "%lu", (unsigned long)(app->f_end / 1000000));
    canvas_draw_str(canvas, 1, WF_H - 2, s); // bottom of heatmap = end freq
    canvas_set_color(canvas, ColorBlack);

    // time scale: baseline + tick notches at a "nice" division, newest (0) on the right.
    // ~6 ticks; the division size is labelled so bursts' spacing can be read off the grid.
    static const uint16_t NICE[] = {5, 10, 15, 30, 60, 120, 300};
    uint16_t target = win / 6 ? win / 6 : 5;
    uint16_t div = NICE[0];
    for(size_t i = 0; i < COUNT_OF(NICE); i++)
        if(NICE[i] <= target) div = NICE[i];
    int sy = WF_H; // scale strip top
    canvas_draw_line(canvas, 0, sy, SCR_W - 1, sy);
    for(uint16_t dt = 0; dt <= win; dt += div) {
        int x = (SCR_W - 1) - (int)((uint32_t)dt * (SCR_W - 1) / win);
        canvas_draw_line(canvas, x, sy, x, sy + 3);
    }
    // bottom row: page hint, division size, window total
    canvas_draw_str(canvas, 2, 62, s_is_encoder ? "OK+turn:Pg" : "<>Pg"); // PORT: encoder flip
    char dv[12];
    if(div >= 60 && (div % 60) == 0)
        snprintf(dv, sizeof(dv), "%um/div", (unsigned)(div / 60));
    else
        snprintf(dv, sizeof(dv), "%us/div", (unsigned)div);
    canvas_draw_str_aligned(canvas, 64, 62, AlignCenter, AlignBottom, dv);
    char w[8];
    fmt_win(win, w, sizeof(w));
    canvas_draw_str_aligned(canvas, SCR_W - 1, 62, AlignRight, AlignBottom, w);
}

// the 4 strongest DISTINCT peaks (descending), tv=dBm tb=bin index. Shared by the numeric
// page and the capture entry so OK captures exactly the slot the cursor is on. Bins within
// GROUP_HZ are treated as one peak (a single transmitter leaks across adjacent bins -- the
// CC1101 RX filter is 270-650 kHz wide, so sub-MHz-apart bins are the same signal), keeping
// only the strongest so the list shows 4 real signals, not 4 slices of one.
// -6 dB occupied width of the peak at `bin`, walking outwards while the sweep stays within
// 6 dB of that peak. Returns 0 when the step is too coarse for the number to mean anything:
// at 650 kHz per bin every signal is "one bin wide" and calling that a bandwidth is a lie.
// This is the one discriminator pulse width and cadence cannot give: a chirp (LoRa 125/250/
// 500 kHz) and a narrowband OOK remote look identical in time but differ by 10x in width.
static uint32_t bw6(const App* app, uint16_t bin) {
    if(app->f_step > 100000) return 0;
    int8_t cur = app->rssi[bin]; // the LIVE value: peak[] is a max-hold and can be minutes old
    if(cur <= app->trigger) return 0; // nothing there right now -> no honest width to report
    int thr = (int)cur - 6;
    uint16_t l = bin, r = bin;
    while(l > 0 && app->rssi[l - 1] >= thr)
        l--;
    while(r + 1 < app->nbins && app->rssi[r + 1] >= thr)
        r++;
    return (uint32_t)(r - l + 1) * app->f_step;
}

// A peak only counts if it stands clear of the lowest point between it and the next higher
// peak -- topographic PROMINENCE, the rule PortaPack's NxtPk uses ("rises at least 3 dB above
// the valley"). The old 400 kHz grouping was a guess about the CC1101 filter width; this
// measures the valley instead of assuming it.
#define PEAK_PROMINENCE_DB 3
#define PROM_CANDIDATES    12

static int8_t prominence_at(const App* app, uint16_t i) {
    int8_t v = app->rssi[i];
    if(v == -128) return 0;
    int8_t lmin = v, rmin = v;
    for(int j = (int)i - 1; j >= 0; j--) { // descend until something higher bounds the basin
        if(app->rssi[j] > v || app->rssi[j] == -128) break;
        if(app->rssi[j] < lmin) lmin = app->rssi[j];
    }
    for(int j = (int)i + 1; j < app->nbins; j++) {
        if(app->rssi[j] > v || app->rssi[j] == -128) break;
        if(app->rssi[j] < rmin) rmin = app->rssi[j];
    }
    int key = (lmin > rmin) ? lmin : rmin; // the higher key col decides
    return (int8_t)(v - key);
}

// the 4 strongest PROMINENT peaks (descending), tv=dBm tb=bin index. Shared by the numeric
// page and the capture entry so OK captures exactly the slot the cursor is on.
static void numeric_top4(const App* app, int tv[4], uint16_t tb[4]) {
    tv[0] = tv[1] = tv[2] = tv[3] = -200;
    tb[0] = tb[1] = tb[2] = tb[3] = 0;

    // pass 1: the strongest PEAK_CANDIDATES bins above the floor. Limiting the candidate set
    // keeps the prominence walk (which is O(n) worst case) off the 8 Hz draw path's budget.
    uint16_t cand[PROM_CANDIDATES];
    int8_t cval[PROM_CANDIDATES];
    int nc = 0;
    for(uint16_t i = 0; i < app->nbins; i++) {
        int v = app->rssi[i];
        if(v <= app->trigger) continue;
        if(i > 0 && v <= app->rssi[i - 1]) continue; // leftmost bin of a plateau only
        if(nc == PROM_CANDIDATES && v <= cval[PROM_CANDIDATES - 1]) continue;
        int k = (nc < PROM_CANDIDATES) ? nc++ : PROM_CANDIDATES - 1;
        while(k > 0 && cval[k - 1] < v) { // insertion by value, descending
            cval[k] = cval[k - 1];
            cand[k] = cand[k - 1];
            k--;
        }
        cval[k] = (int8_t)v;
        cand[k] = i;
    }

    // pass 2: keep the prominent ones, best first
    int n = 0;
    for(int c = 0; c < nc; c++) {
        if(prominence_at(app, cand[c]) < PEAK_PROMINENCE_DB) continue;
        int v = cval[c];
        int slot = n < 4 ? n++ : 4;
        if(n == 4 && v <= tv[3]) {
            n = 4;
            continue;
        }
        int k = (slot == 4) ? 3 : slot;
        while(k > 0 && tv[k - 1] < v) {
            tv[k] = tv[k - 1];
            tb[k] = tb[k - 1];
            k--;
        }
        tv[k] = v;
        tb[k] = cand[c];
    }
}

// Numeric page: 4 full-width horizontal bars (precise top-4 bins). Bar length ~ strength;
// the "<freq> <dBm> ~<band>" label is drawn in XOR so it reads black on the empty part and
// white over the filled part. The cursor row (Up/Down) is bracketed; OK captures it.
static void draw_numeric(Canvas* canvas, App* app) {
    int tv[4];
    uint16_t tb[4];
    numeric_top4(app, tv, tb);
    int floor, ceil;
    autoscale(app, &floor, &ceil);
    const int pitch = 13, bh = 11;
    // M1 = the strongest peak; every other row is labelled as a delta from it (frequency offset
    // and dB below). "How far is this from the reference, and how much weaker" is the question
    // you actually ask when a known transmitter is sitting next to an unknown one.
    uint32_t f0 = app->f_start + (uint32_t)tb[0] * app->f_step;
    int nshown = 0;
    for(int k = 0; k < 4; k++) {
        if(tv[k] <= -200) continue; // empty slot -> draw nothing (no stray outline)
        nshown++;
        int y = 1 + k * pitch;
        uint32_t f = app->f_start + (uint32_t)tb[k] * app->f_step;
        int len = (tv[k] - floor) * SCR_W / ((ceil > floor) ? (ceil - floor) : 1);
        if(len < 2) len = 2;
        if(len > SCR_W) len = SCR_W;
        canvas_draw_box(canvas, 0, y, len, bh);
        const FreqBand* b = band_lookup(f);
        // zoomed enough to resolve a width? show it instead of the band tag, which the header
        // already carries -- never both, or the row overflows 128 px
        uint32_t bw = bw6(app, tb[k]);
        char tag[28];
        if(k > 0 && tv[0] > -200) { // delta marker (M1 is row 0)
            int d100 = (int)(((long)f - (long)f0) / 100); // 100 Hz units, bounded by 928 MHz
            if(d100 > 9999999) d100 = 9999999;
            if(d100 < -9999999) d100 = -9999999;
            unsigned a = (unsigned)(d100 < 0 ? -d100 : d100);
            // 100 Hz units. Peaks far apart produced "+59150.0k" and ran off the screen; above
            // 1 MHz the decimal is noise anyway, so switch unit rather than widen the line.
            if(a >= 10000)
                snprintf(
                    tag, sizeof(tag), "%s%u.%uM -%d", d100 < 0 ? "-" : "+", a / 10000,
                    (a / 1000) % 10, tv[0] - tv[k]);
            else
                snprintf(
                    tag, sizeof(tag), "%s%u.%uk -%d", d100 < 0 ? "-" : "+", a / 10, a % 10,
                    tv[0] - tv[k]);
        } else if(bw) {
            if(bw >= 1000000)
                snprintf(
                    tag,
                    sizeof(tag),
                    "bw%lu.%luM",
                    (unsigned long)(bw / 1000000),
                    (unsigned long)((bw / 100000) % 10));
            else
                snprintf(tag, sizeof(tag), "bw%luk", (unsigned long)(bw / 1000));
        } else {
            snprintf(tag, sizeof(tag), "%s", b ? b->name : "");
        }
        char s[64];
        snprintf(
            s,
            sizeof(s),
            "%lu.%03lu  %d  %s",
            (unsigned long)(f / 1000000),
            (unsigned long)((f / 1000) % 1000),
            tv[k],
            tag);
        canvas_set_color(canvas, ColorXOR); // reverse over the filled part, normal over empty
        canvas_draw_str(canvas, 3, y + bh - 2, s);
        // selected row (the one OK will capture): "<" pointer at the right end, no box
        if(k == app->num_sel)
            canvas_draw_str_aligned(canvas, SCR_W - 1, y + bh - 2, AlignRight, AlignBottom, "<");
        canvas_set_color(canvas, ColorBlack);
    }
    if(nshown == 0) canvas_draw_str(canvas, 2, 30, "no peaks above trigger");
}

// History page: the busy 1 MHz channels ranked by OCCUPANCY -- the fraction of the rolling
// window's sweeps that saw them above the trigger. This replaces a "one winner per sweep"
// counter, which let the strongest carrier hide every other active channel, and it is a real
// fraction rather than a popularity count.
// Top 1 MHz channels by mean occupancy. Shared by the History page and the auto-bookmark
// writer so the screen and the file can never disagree about what "busy" means.
static uint8_t occ_top(const App* app, OccBucket* b, uint8_t max) {
    uint8_t nb = 0;
    for(uint16_t i = 0; i < app->nbins; i++) {
        if(!app->occ[i]) continue;
        uint16_t mhz = (uint16_t)((app->f_start + (uint32_t)i * app->f_step) / 1000000);
        int k = -1;
        for(uint8_t j = 0; j < nb; j++)
            if(b[j].mhz == mhz) {
                k = (int)j;
                break;
            }
        if(k < 0) {
            if(nb == max) continue;
            k = (int)nb++;
            b[k].mhz = mhz;
            b[k].sum = 0;
            b[k].cnt = 0;
        }
        b[k].sum += app->occ[i];
        b[k].cnt++;
    }
    for(uint8_t i = 0; i < nb; i++) // rank by mean occupancy, cross-multiplied integers
        for(uint8_t j = (uint8_t)(i + 1); j < nb; j++)
            if(b[j].sum * b[i].cnt > b[i].sum * b[j].cnt) {
                OccBucket t = b[i];
                b[i] = b[j];
                b[j] = t;
            }
    return nb;
}

static void draw_history(Canvas* canvas, App* app) {
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 8, "Channel occupancy");
    uint8_t n = app->occ_n;
    if(!n) {
        canvas_draw_str(canvas, 2, 30, "no sweeps yet");
        return;
    }
    {
        char w[16];
        snprintf(w, sizeof(w), "n=%u", (unsigned)n);
        canvas_draw_str_aligned(canvas, SCR_W - 1, 8, AlignRight, AlignBottom, w);
    }

    OccBucket b[8];
    uint8_t nb = occ_top(app, b, COUNT_OF(b));

    for(uint8_t k = 0; k < nb && k < 4; k++) {
        uint32_t pct = b[k].sum * 100u / ((uint32_t)b[k].cnt * n);
        const FreqBand* bd = band_lookup((uint32_t)b[k].mhz * 1000000);
        char line[40];
        snprintf(
            line,
            sizeof(line),
            "%3u  %3lu%%  %s",
            (unsigned)b[k].mhz,
            (unsigned long)pct,
            bd ? bd->name : "");
        // first baseline 20, not 16: the header's descenders ("p", "y" of "occupancy") reach
        // y=10 and a row starting at 16 collided with them. 11 px leading, last row ends at 55.
        canvas_draw_str(canvas, 2, 20 + (int)k * 11, line);
    }
}

// ---- help ------------------------------------------------------------------
// The controls are dense enough that they belong on the device, not only in a README nobody
// has with them in the field.
static void help_draw(Canvas* canvas, void* model) {
    UNUSED(model);
    // five lines at 11 px leading: the seven-line version overlapped itself at 7 px spacing
    static const char* const LINES[] = {
        "L/R  page (hold on bars)",
        "U/D  window / tune",
        "OK   zoom / park / capture",
        "hold OK   log on-off",
        "hold U/D  hold / clear",
    };
    // PORT: the encoder key map in spectrum_input(). Same five-line layout.
    static const char* const ENC_LINES[] = {
        "turn  cursor/window/tune",
        "OK+turn   page",
        "OK   zoom / park / capture",
        "hold OK   log on-off",
        "hold side key   freeze",
    };
    const char* const* lines = s_is_encoder ? ENC_LINES : LINES;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 8, "Controls");
    for(size_t i = 0; i < COUNT_OF(LINES); i++)
        canvas_draw_str(canvas, 2, 18 + (int)i * 11, lines[i]);
}

static bool help_input(InputEvent* e, void* ctx) {
    UNUSED(ctx);
    App* app = g_app;
    if(!app) return false;
    if(e->key == InputKeyBack && e->type == InputTypeShort) {
        view_dispatcher_switch_to_view(app->vd, VIEW_CONF2); // Help is opened from page 2
        return true;
    }
    return true;
}

#ifdef RFS_LAB
// ---- carrier test (lab only) ----------------------------------------------
// An unmodulated tone for bench work: PA check, antenna, receiver desense. Three things keep
// it an instrument rather than a nuisance -- the API caps the duration at 60 s, only frequencies
// the CC1101 can tune are keyed (no region gate in the lab build), and OK cancels mid-count.
static const uint8_t CARRIER_SECS[] = {5, 10, 20, 30, 60};

static int32_t carrier_worker(void* ctx) {
    App* app = ctx;
    uint32_t f = app->f_start + (uint32_t)(app->nbins / 2) * app->f_step;
    app->carrier_t0 = furi_get_tick();
    RfTxStatus st = rf_tx_carrier(
        f, subghz_device_cc1101_preset_ook_650khz_async_regs, CARRIER_SECS[app->carrier_secs],
        &app->carrier_abort);
    FURI_LOG_I(TAG, "carrier %lu Hz -> %d", (unsigned long)f, (int)st);
    app->carrier_on = false;
    return 0;
}

static void carrier_stop(App* app) {
    if(!app->carrier_worker) return;
    app->carrier_abort = true;
    furi_thread_join(app->carrier_worker);
    furi_thread_free(app->carrier_worker);
    app->carrier_worker = NULL;
    app->carrier_on = false;
    // hand the radio back to the survey (once: nothing else restarts it while a carrier exists)
    if(!app->worker) {
        app->running = true;
        app->reconfig = true;
        app->worker = furi_thread_alloc_ex("rfsurvey_sweep", 6144, sweep_worker, app);
        furi_thread_start(app->worker);
    }
}

static void carrier_start(App* app) {
    // a carrier that ended by itself leaves a finished thread until lab_tick reaps it; reap it
    // here too, or the pointer below would be overwritten and the thread leaked
    if(app->carrier_worker) {
        furi_thread_join(app->carrier_worker);
        furi_thread_free(app->carrier_worker);
        app->carrier_worker = NULL;
    }
    // the sweep worker is the radio's owner; stop and join it before the transmitter runs
    app->running = false;
    if(app->worker) {
        furi_thread_join(app->worker);
        furi_thread_free(app->worker);
        app->worker = NULL;
    }
    app->carrier_abort = false;
    app->carrier_on = true;
    app->carrier_worker = furi_thread_alloc_ex("rfsurvey_carrier", 6144, carrier_worker, app);
    if(!app->carrier_worker) {
        FURI_LOG_E(TAG, "carrier: out of memory, not starting");
        app->carrier_on = false;
        return;
    }
    furi_thread_start(app->carrier_worker);
}

// GUI-thread tick (lab only). A carrier that runs out its time ends in its own thread, which can
// neither free itself nor safely restart the sweep: app->worker is also changed on THIS thread by
// OK, teardown and the TX rows. So carrier_worker only clears carrier_on, and the single owner
// (this thread) reaps it and hands the radio back. Before this the survey stayed frozen after
// every carrier that was not cancelled with OK.
static void lab_tick(void* ctx) {
    App* app = ctx;
    if(app && app->carrier_worker && !app->carrier_on) carrier_stop(app);
}

static void draw_carrier(Canvas* canvas, App* app) {
    canvas_set_font(canvas, FontSecondary);
    char s[32];
    uint32_t f = app->f_start + (uint32_t)(app->nbins / 2) * app->f_step;
    snprintf(
        s,
        sizeof(s),
        "Carrier %lu.%03lu",
        (unsigned long)(f / 1000000),
        (unsigned long)((f / 1000) % 1000));
    canvas_draw_str(canvas, 2, 9, s);

    if(!app->carrier_on) {
        canvas_draw_str(canvas, 2, 28, "OK: start");
        snprintf(s, sizeof(s), "Up/Down: %u s", (unsigned)CARRIER_SECS[app->carrier_secs]);
        canvas_draw_str(canvas, 2, 40, s);
        canvas_draw_str(canvas, 2, 54, "unmodulated, capped at 60 s");
        return;
    }

    uint32_t total = CARRIER_SECS[app->carrier_secs];
    uint32_t elapsed = (furi_get_tick() - app->carrier_t0) / 1000;
    uint32_t left = (elapsed >= total) ? 0 : total - elapsed;

    canvas_set_font(canvas, FontBigNumbers);
    snprintf(s, sizeof(s), "%lu", (unsigned long)left);
    canvas_draw_str_aligned(canvas, SCR_W / 2, 36, AlignCenter, AlignBottom, s);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, SCR_W / 2, 42, AlignCenter, AlignBottom, "s left");
    canvas_draw_str(canvas, 2, 56, "OK: stop now");
}
#endif

// ---- cadence page (zero-span) ---------------------------------------------

// Allan deviation of the period series (tau = one sample): consecutive differences cancel the
// slow drift that plain variance counts as jitter, so a crystal sits at ~0 even when its period
// walks, while a human's intervals do not. Floor is ~2*PARK_SAMPLE_MS/period from the sampling
// grid alone.
static float park_adev(const App* app) {
    uint8_t n = app->park.per_n;
    if(n < 3) return 0.0f;
    uint8_t idx = (uint8_t)((app->park.per_i + PARK_RING - n) % PARK_RING);
    int prev = app->park.per[idx];
    float sum = (float)prev, diff2 = 0.0f;
    idx = (uint8_t)((idx + 1) % PARK_RING);
    for(uint8_t i = 1; i < n; i++) {
        int cur = app->park.per[idx];
        float d = (float)(cur - prev);
        diff2 += d * d;
        sum += (float)cur;
        prev = cur;
        idx = (uint8_t)((idx + 1) % PARK_RING);
    }
    float mean = sum / (float)n;
    if(mean <= 0.0f) return 0.0f;
    return sqrtf(0.5f * (diff2 / (float)(n - 1))) / mean;
}

static void draw_cadence(Canvas* canvas, App* app) {
    canvas_set_font(canvas, FontSecondary);
    char s[40];
    // idle: show where OK WILL park (park_begin takes selected_freq), not the last parked
    // frequency -- that one is stale and can sit outside the current range entirely
    uint32_t f = app->parked ? app->park_freq : selected_freq(app);
    uint8_t p = app->preset_idx < PRESET_N ? app->preset_idx : 0;

    if(!app->parked) {
        canvas_draw_str(canvas, 2, 9, "Cadence (zero-span)");
        snprintf(
            s,
            sizeof(s),
            "%lu.%03lu MHz  %s",
            (unsigned long)(f / 1000000),
            (unsigned long)((f / 1000) % 1000),
            PRESETS[p].name);
        canvas_draw_str(canvas, 2, 22, s);
        canvas_draw_str(canvas, 2, 38, "OK: park on this freq");
        canvas_draw_str(canvas, 2, 49, "then read period +");
        canvas_draw_str(canvas, 2, 60, "jitter (machine/human)");
        return;
    }

    snprintf(
        s,
        sizeof(s),
        "%lu.%03lu %s",
        (unsigned long)(f / 1000000),
        (unsigned long)((f / 1000) % 1000),
        PRESETS[p].name);
    canvas_draw_str(canvas, 2, 8, s);
    // The modulation contrast lives in the header: the three body lines were 8 px apart in a
    // 9 px font and visibly collided (the simulator caught it; the device would show the same).
    if(app->park_ab_done) {
        snprintf(s, sizeof(s), "AM%d FM%d", app->park_ab[0], app->park_ab[1]);
        canvas_draw_str_aligned(canvas, SCR_W - 1, 8, AlignRight, AlignBottom, s);
    }
    // "~" = the numbers on this page are degraded: an observation hole happened in the last
    // 5 s, so some bursts were missed and their intervals were discarded
    if(app->park.gap_t && (furi_get_tick() - app->park.gap_t) < 5000)
        canvas_draw_str_aligned(canvas, SCR_W - 1, 18, AlignRight, AlignBottom, "~");

    // envelope strip: last PARK_N samples, one pixel column each, filled to the baseline
    int cnt = app->park_count;
    if(cnt > PARK_N) cnt = PARK_N;
    int lo = 127, hi = -128;
    for(int i = 0; i < cnt; i++) {
        int idx = (app->park_head + PARK_N - cnt + i) % PARK_N;
        int v = app->park_env[idx];
        if(v < lo) lo = v;
        if(v > hi) hi = v;
    }
    if(hi < lo) {
        lo = -110;
        hi = -70;
    }
    if(hi - lo < 20) hi = lo + 20;
    const int y0 = 12, y1 = 36;
    for(int i = 0; i < cnt; i++) {
        int idx = (app->park_head + PARK_N - cnt + i) % PARK_N;
        int v = app->park_env[idx];
        if(v < lo) v = lo;
        if(v > hi) v = hi;
        canvas_draw_line(canvas, i, y1 - (v - lo) * (y1 - y0) / (hi - lo), i, y1);
    }
    int ty = app->trigger;
    if(ty < lo) ty = lo;
    if(ty > hi) ty = hi;
    int yt = y1 - (ty - lo) * (y1 - y0) / (hi - lo);
    for(int x = 0; x < SCR_W; x += 3)
        canvas_draw_dot(canvas, x, yt);

    uint32_t mean_ms, sd_ms, cv_milli;
    rfs_park_stats(&app->park, &mean_ms, &sd_ms, &cv_milli);
    uint8_t np = app->park.per_n;
    // absolute spread first: a 100 ms periodic device has CV ~0.02 purely from the 8 ms sample
    // clock, so "stddev within a few sample ticks" is the honest machine test, not CV alone
    // two devices on one frequency make the period series bimodal; averaging them would
    // report a period that belongs to neither, so this check comes first
    uint16_t centers[4];
    uint8_t ncl = rfs_period_clusters(app->park.per, app->park.per_n, centers, COUNT_OF(centers));
    const char* cls = (np < 3) ? "measuring..." :
                      (ncl >= 2) ? "2+ devices" :
                      (sd_ms <= 3u * PARK_SAMPLE_MS) ? "MACHINE (crystal)" :
                      (cv_milli < 150) ? "semi-periodic" : "HUMAN / event";
    canvas_draw_str(canvas, 2, 48, cls);
    unsigned a100 = (unsigned)(park_adev(app) * 100.0f + 0.5f);
    snprintf(s, sizeof(s), "A%u.%02u", a100 / 100, a100 % 100);
    canvas_draw_str_aligned(canvas, SCR_W - 1, 48, AlignRight, AlignBottom, s);

    // one snapshot: the worker can zero the denominator between the test and the divide
    uint32_t nsamp = app->park.n_samples;
    uint32_t duty = nsamp ? (uint32_t)(((uint64_t)app->park.n_above * 100u) / nsamp) : 0;
    unsigned last = app->park.dur_n ?
                        app->park.dur[(app->park.dur_i + PARK_RING - 1) % PARK_RING] :
                        0;
    unsigned rep = app->park.rep_last ? app->park.rep_last : app->park.rep;
    if(mean_ms >= 1000)
        snprintf(
            s,
            sizeof(s),
            "P%u.%us d%lu%% x%u",
            (unsigned)(mean_ms / 1000),
            (unsigned)((mean_ms / 100) % 10),
            (unsigned long)duty,
            rep);
    else
        snprintf(
            s,
            sizeof(s),
            "P%lums d%lu%% x%u %ums",
            (unsigned long)mean_ms,
            (unsigned long)duty,
            rep,
            (unsigned)last);
    canvas_draw_str(canvas, 2, 58, s);
}

// metres -> "385m" / "1.2km", short enough for a 128 px line
static void fmt_dist(char* buf, size_t n, uint32_t m) {
    if(m > 99999) m = 99999; // the model clamps here too; keeps the buffer arithmetic bounded
    if(m < 1000)
        snprintf(buf, n, "%lum", (unsigned long)m);
    else
        snprintf(buf, n, "%lu.%lukm", (unsigned long)(m / 1000), (unsigned long)((m / 100) % 10));
}

// Fox hunt: one frequency, one number, one bar, and the vibro. No statistics -- the question is
// "am I getting warmer", and the answer has to be readable at arm's length while walking.
static void draw_hunt(Canvas* canvas, App* app) {
    canvas_set_font(canvas, FontSecondary);
    char s[48];
    uint32_t f = app->parked ? app->park_freq : selected_freq(app); // see draw_cadence
    snprintf(
        s,
        sizeof(s),
        "%lu.%03lu MHz",
        (unsigned long)(f / 1000000),
        (unsigned long)((f / 1000) % 1000));
    canvas_draw_str(canvas, 2, 9, s);
    canvas_draw_str_aligned(
        canvas, SCR_W - 1, 9, AlignRight, AlignBottom, app->haptic ? "vibro" : "haptic off");

    if(!app->parked) {
        canvas_draw_str(canvas, 2, 30, "OK: start hunting");
        canvas_draw_str(canvas, 2, 42, "Up/Down: tune");
        canvas_draw_str(canvas, 2, 54, "max signal = the source");
        return;
    }

    int cnt = app->park_count;
    int8_t cur = cnt ? app->park_env[(app->park_head + PARK_N - 1) % PARK_N] : -127;

    // big dBm readout, unit BESIDE the number on the same baseline: centred under it, "dBm"
    // (bottom 40, ~7 px tall) landed on the number's last rows (bottom 34). The pair is centred
    // as one, measured each time because "-45" and "-109" differ in width.
    snprintf(s, sizeof(s), "%d", cur);
    canvas_set_font(canvas, FontBigNumbers);
    uint16_t nw = canvas_string_width(canvas, s);
    canvas_set_font(canvas, FontSecondary);
    uint16_t uw = canvas_string_width(canvas, "dBm");
    int x0 = (SCR_W - (int)(nw + 3 + uw)) / 2;
    canvas_set_font(canvas, FontBigNumbers);
    canvas_draw_str_aligned(canvas, x0, 34, AlignLeft, AlignBottom, s);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, x0 + nw + 3, 34, AlignLeft, AlignBottom, "dBm");

    // bar: -105 .. -40 dBm across the full width. Deliberately fixed rather than autoscaled --
    // an autoscaling bar always "looks full" and defeats the whole point of walking.
    const int lo = -105, hi = -40;
    int v = cur;
    if(v < lo) v = lo;
    if(v > hi) v = hi;
    int len = (v - lo) * (SCR_W - 4) / (hi - lo);
    canvas_draw_frame(canvas, 2, 44, SCR_W - 4, 9);
    if(len > 0) canvas_draw_box(canvas, 3, 45, len, 7);

    // Distance as a BAND, never a number: the interval is what +-6 dB of RSSI uncertainty does
    // to the inverse of the path loss model. Anything narrower would be fake precision.
    if(cur > app->trigger) {
        uint16_t gain = 0;
        UNUSED(gain);
        RfsDistance d = rfs_estimate_distance(
            cur, f, 10, 0, PATH_N10[app->path_n < COUNT_OF(PATH_N10) ? app->path_n : 1], 6);
        if(d.valid) {
            char p1[12], p2[12], p3[12];
            fmt_dist(p1, sizeof(p1), d.m);
            fmt_dist(p2, sizeof(p2), d.lo);
            fmt_dist(p3, sizeof(p3), d.hi);
            snprintf(s, sizeof(s), "~%s (%s-%s)", p1, p2, p3);
            canvas_draw_str(canvas, 2, 62, s);
        }
    } else {
        canvas_draw_str(canvas, 2, 62, "below trigger");
    }
}

static void spectrum_draw(Canvas* canvas, void* model) {
    UNUSED(model);
    App* app = g_app;
    if(!app) return; // defensive: never draw after teardown
    canvas_clear(canvas);
    canvas_set_font(canvas, FontSecondary);

    if(app->page == 7) {
#ifdef RFS_LAB
        draw_carrier(canvas, app);
#endif
    } else if(app->page == 6) {
        draw_hunt(canvas, app); // full-height fox-hunt page
    } else if(app->page == 5) {
        draw_cadence(canvas, app); // full-height zero-span page, draws its own layout
    } else if(app->page == 3) {
        draw_numeric(canvas, app); // bars show absolute freqs; the Capture pill is below
    } else if(app->page == 2) {
        draw_waterfall(canvas, app); // full-bleed, carries its own range + span labels
    } else if(app->page == 4) {
        draw_history(canvas, app);
    } else {
        // header: range + likely band name (+ window seconds on the connected page)
        // The band tag is looked up at the CENTRE of the range, which is a lie for a wide
        // sweep: 779-927 MHz is not "LTE800 UL", it just contains it. Only name the band when
        // the whole range actually fits inside one.
        const FreqBand* hb = band_lookup(app->f_start);
        if(hb != band_lookup(app->f_end)) hb = NULL;
        char hdr[48];
        if(app->page == 1) {
            char w[8];
            fmt_win(WIN_SECS[app->win_idx], w, sizeof(w));
            snprintf(
                hdr,
                sizeof(hdr),
                "%lu-%lu %s %s",
                (unsigned long)(app->f_start / 1000000),
                (unsigned long)(app->f_end / 1000000),
                hb ? hb->name : "",
                w);
        } else
            snprintf(
                hdr,
                sizeof(hdr),
                "%lu-%lu %s",
                (unsigned long)(app->f_start / 1000000),
                (unsigned long)(app->f_end / 1000000),
                hb ? hb->name : "");
        // ONE readout line. Two lines of header cost the plot 8 px of its 48 -- and the range
        // is already obvious from the axis, so the band prose was paying rent for nothing.
        if(app->page == 0) {
            int8_t cur, pk;
            grp_max(app, app->cursor, &cur, &pk);
            snprintf(
                hdr,
                sizeof(hdr),
                "%lu.%lu %d",
                (unsigned long)(grp_freq(app, app->cursor) / 1000000),
                (unsigned long)((grp_freq(app, app->cursor) / 100000) % 10),
                cur);
        } else {
            int pmax = -200;
            uint16_t pidx = 0;
            for(uint16_t i = 0; i < app->nbins; i++)
                if(app->rssi[i] > pmax) {
                    pmax = app->rssi[i];
                    pidx = i;
                }
            uint32_t pf = app->f_start + (uint32_t)pidx * app->f_step;
            snprintf(
                hdr,
                sizeof(hdr),
                "%lu.%lu %d",
                (unsigned long)(pf / 1000000),
                (unsigned long)((pf / 100000) % 10),
                pmax);
        }
        canvas_draw_str(canvas, 2, 9, hdr);

        // right slot, by priority: a held trace explains the screen, a low battery explains
        // everything else, an under-settled sweep explains the numbers, otherwise show where
        // you are in the page ring
        char mark[8] = "";
        if(app->frozen)
            snprintf(mark, sizeof(mark), "HOLD");
        else if(app->batt_pct && app->batt_pct < 15)
            snprintf(mark, sizeof(mark), "%u%%", (unsigned)app->batt_pct);
        else if(app->settle_ms < settle_min_ms(app->f_step))
            snprintf(mark, sizeof(mark), "!");
        else if(app->page == 1) { // connected: the averaging window
            char w[8];
            fmt_win(WIN_SECS[app->win_idx], w, sizeof(w));
            snprintf(mark, sizeof(mark), "%s", w);
        } else { // bars: the peak-hold of the band the cursor is on
            int8_t cur, pk;
            grp_max(app, app->cursor, &cur, &pk);
            snprintf(mark, sizeof(mark), "pk%d", pk);
        }
        canvas_draw_str_aligned(canvas, SCR_W - 1, 9, AlignRight, AlignBottom, mark);
        // (the info line that used to sit at y=18 is gone; see the single readout above)
        if(0) {
        }
        int floor, ceil;
        autoscale(app, &floor, &ceil);
        if(app->page == 0)
            draw_bars(canvas, app, floor, ceil);
        else
            draw_connected(canvas, app, floor, ceil);
    }

    // native pill hints (the waterfall and cadence pages draw their own layout instead)
    if(app->page != 0 && app->page != 1 && app->page != 2 && app->page != 5 && app->page != 6 &&
       app->page != 7) {
        // the left pill was the word "Page" on every screen, which told you the key existed but
        // not where you were. A position readout answers the question you actually have.
        if(app->page == 3) elements_button_center(canvas, "Capture"); // OK on the cursor row
    }

    // Recording indicator: a dot in the extreme corner, no text. It used to draw a dot AND a
    // row count at y=8, which collided with every page's right-hand readout (pk-99, n=99,
    // AM10 FM10, haptic off) -- the screenshots made that obvious and nothing else did.
    // The row count still goes to the log; the dot is a status light.
    // XOR on the full-bleed pages (2/3) so it reads over lit/dark cells.
    if(app->recording) {
        bool fb = (app->page == 2 || app->page == 3 || app->page == 5);
        if(fb) canvas_set_color(canvas, ColorXOR);
        canvas_draw_disc(canvas, SCR_W - 3, 3, 2);
        if(fb) canvas_set_color(canvas, ColorBlack);
    }
}

static bool spectrum_input(InputEvent* event, void* context) {
    // EXPERIMENT: shot_flush
    UNUSED(context);
    App* app = g_app;
    if(!app) return false;
    if(!enc_prefilter(event)) return true;
    // PORT: encoder map (T-Embed, ThorChimera). The first port turned the wheel into page flips,
    // which left the Bars cursor (short Left/Right) and Freeze (Up long) unreachable, so OK on Bars
    // always zoomed the same band. Now the wheel drives each page's own axis and the page flip moves
    // to the secondary gesture:
    //   wheel (Up/Down)              Bars: band cursor (-> short Left/Right)
    //                                other pages: upstream Up/Down (time window, Numeric row,
    //                                parked tuning on Cadence/Hunt)
    //   OK held + turn (Left/Right)  page flip on every page (-> Left/Right LONG)
    //   side key held (Back Long)    freeze / hold traces (-> Up LONG; upstream Up-long)
    //   OK held                      CSV logging (unchanged; enc_prefilter drops the stray OK Long
    //                                that can end an OK-held+turn)
    // Clear peaks (upstream Down-long) stays on the config page-2 "Clear peaks" row.
    // D-pad boards skip this entirely.
    if(s_is_encoder) {
        if(event->key == InputKeyUp || event->key == InputKeyDown) {
            if(app->page == 0) {
                event->key = (event->key == InputKeyDown) ? InputKeyRight : InputKeyLeft;
            }
        } else if(event->key == InputKeyLeft || event->key == InputKeyRight) {
            if(event->type == InputTypeShort) event->type = InputTypeLong;
        } else if(event->key == InputKeyBack && event->type == InputTypeLong) {
            event->key = InputKeyUp;
        }
    }
    bool sp = (event->type == InputTypeShort);
    bool lp = (event->type == InputTypeLong);

    if(event->key == InputKeyBack && sp) {
        if(app->zdepth) {
            zoom_out(app);
            spec_redraw(app);
            return true;
        }
        return false; // -> config
    }
    // On the bars page short Left/Right is the cursor and LONG is the page flip. It used to fall
    // through to a page change at the edges, so leaving the page from the middle took 16 presses
    // and the "Page" hint was a lie. Now the mapping is the same shape everywhere: short acts on
    // the page, long changes it.
    if(event->key == InputKeyLeft && (sp || lp)) {
        if(sp && app->page == 0) {
            if(app->cursor > 0) app->cursor--;
        } else {
            app->page = (uint8_t)((app->page + PAGE_N - 1) % PAGE_N);
        }
        app->parked = (app->page == 5 || app->page == 6) ? app->parked : false;
        spec_redraw(app);
        return true;
    }
    if(event->key == InputKeyRight && (sp || lp)) {
        if(sp && app->page == 0) {
            if(app->cursor < NBARS - 1) app->cursor++;
        } else {
            app->page = (uint8_t)((app->page + 1) % PAGE_N);
        }
        app->parked = (app->page == 5 || app->page == 6) ? app->parked : false;
        spec_redraw(app);
        return true;
    }
    // long Up/Down: hold the traces / clear the holds. Kept off the short presses so the time
    // window and page flips keep their existing meaning.
    if(event->key == InputKeyUp && lp && app->page != 5 && app->page != 6) {
        app->frozen = !app->frozen;
        FURI_LOG_I(TAG, "input: hold %s", app->frozen ? "on" : "off");
        spec_redraw(app);
        return true;
    }
    if(event->key == InputKeyDown && lp && app->page != 5 && app->page != 6) {
        app->clear_peaks = true; // max-hold and min-hold both re-seed from the next sweep
        spec_redraw(app);
        return true;
    }
#ifdef RFS_LAB
    // Carrier page: OK starts/stops, Up/Down picks the duration while idle
    if(app->page == 7 && event->key == InputKeyOk && sp) {
        if(app->carrier_on)
            carrier_stop(app);
        else
            carrier_start(app);
        spec_redraw(app);
        return true;
    }
    if(app->page == 7 && !app->carrier_on && sp &&
       (event->key == InputKeyUp || event->key == InputKeyDown)) {
        uint8_t n = COUNT_OF(CARRIER_SECS);
        app->carrier_secs = (uint8_t)((app->carrier_secs + (event->key == InputKeyUp ? 1 : n - 1)) % n);
        spec_redraw(app);
        return true;
    }
#endif
    // Cadence page: OK parks on (or releases) the selected frequency. Nothing is tuned here --
    // the sweep worker sees the flag at the top of its loop and hands the radio to park_loop.
    if(event->key == InputKeyOk && sp && (app->page == 5 || app->page == 6)) {
        if(app->parked) {
            app->parked = false;
        } else {
            park_begin(app); // selection snapshot happens here, on the GUI thread
        }
        FURI_LOG_I(TAG, "input: park %s", app->parked ? "on" : "off");
        spec_redraw(app);
        return true;
    }
    // while parked, Up/Down walks the frequency in 10 kHz steps so you can watch RSSI rise and
    // land exactly on the carrier instead of guessing from a swept bin.
    if((app->page == 5 || app->page == 6) && app->parked && sp &&
       (event->key == InputKeyUp || event->key == InputKeyDown)) {
        uint32_t f = app->park_freq;
        if(event->key == InputKeyUp)
            f = (f <= 928000000 - 10000) ? f + 10000 : 928000000;
        else
            f = (f >= 300000000 + 10000) ? f - 10000 : 300000000;
        if(furi_hal_subghz_is_frequency_valid(f)) app->park_freq = f;
        spec_redraw(app);
        return true;
    }
    // Up/Down: move the capture cursor on the numeric page (only over filled rows, which are
    // contiguous from 0 since the list is sorted), else cycle the time window
    if((event->key == InputKeyUp || event->key == InputKeyDown) && sp && app->page == 3) {
        int tv[4];
        uint16_t tb[4];
        numeric_top4(app, tv, tb);
        int cnt = 0;
        while(cnt < 4 && tv[cnt] > -200)
            cnt++;
        if(cnt < 1) cnt = 1;
        int d = (event->key == InputKeyUp) ? cnt - 1 : 1;
        app->num_sel = (uint8_t)((app->num_sel + d) % cnt);
        spec_redraw(app);
        return true;
    }
    if(event->key == InputKeyUp && sp) {
        if(app->win_idx < (uint8_t)(WIN_N - 1)) app->win_idx++;
        spec_redraw(app);
        return true;
    }
    if(event->key == InputKeyDown && sp) {
        if(app->win_idx > 0) app->win_idx--;
        spec_redraw(app);
        return true;
    }
    if(event->key == InputKeyOk && lp) {
        app->recording = !app->recording; // long-press toggles CSV logging (any page)
        FURI_LOG_I(TAG, "input: rec %s", app->recording ? "start" : "stop");
        spec_redraw(app);
        return true;
    }
    if(event->key == InputKeyOk && sp && app->page == 0) {
        zoom_in(app, app->cursor); // short-press on bars: zoom into the framed band
        spec_redraw(app);
        return true;
    }
    if(event->key == InputKeyOk && sp && app->page == 3) {
        // numeric page: lock the SELECTED top-4 bin (Up/Down cursor) and open .sub capture.
        // Stop the survey worker first (from this input thread, never the draw cb) so the one
        // radio has a single owner.
        int tv[4];
        uint16_t tb[4];
        numeric_top4(app, tv, tb);
        if(tv[app->num_sel] <= -200) return true; // empty slot -> nothing to capture
#ifdef RFS_LAB
        // PORT FIX: while the lab carrier transmits (page 7), carrier_start() has stopped the
        // sweep worker (app->worker == NULL) and the carrier thread owns the radio. Going to this
        // page and pressing OK used to call furi_thread_join(NULL) -> furi_check crash, and would
        // have started a capture on a radio that is transmitting. Stop the carrier first.
        if(app->carrier_worker) return true;
#endif
        app->recording = false; // the survey worker will close any open CSV as it stops
        app->running = false;
        if(app->worker) {
            furi_thread_join(app->worker);
            furi_thread_free(app->worker);
            app->worker = NULL;
        }
        app->cap_freq = app->f_start + (uint32_t)tb[app->num_sel] * app->f_step;
        app->cap_preset = app->preset_idx;
        app->capturing = false;
        app->cap_samples = 0;
        app->cap_overflow = 0;
        FURI_LOG_I(TAG, "capture: lock %lu Hz", (unsigned long)app->cap_freq);
        view_dispatcher_switch_to_view(app->vd, VIEW_CAP);
        return true;
    }
    return true;
}

// PAGE 2: actions. Everything here runs in place or opens another view; nothing navigates to
// the spectrum by accident, because OK on a value row is the only path that does.
static void conf2_enter(void* ctx, uint32_t index) {
    App* app = ctx;
    FURI_LOG_D(TAG, "tools row %lu", (unsigned long)index);
    if(index == CONF2_ROW_HELP) {
        FURI_LOG_I(TAG, "config: open help");
        view_dispatcher_switch_to_view(app->vd, VIEW_HELP);
        return;
    }
    if(index == CONF2_ROW_SAVEPRF) {
        if(!app->profile_slot) {
            FURI_LOG_W(TAG, "save profile: no slot selected");
            return;
        }
        char path[64];
        profile_path(path, sizeof(path), app->profile_slot);
        Storage* st = furi_record_open(RECORD_STORAGE);
        settings_save(app, st, path);
        furi_record_close(RECORD_STORAGE);
        FURI_LOG_I(TAG, "profile %u saved", (unsigned)app->profile_slot);
        return;
    }
    if(index == CONF2_ROW_SAVEBM) {
        OccBucket b[4];
        uint8_t n = occ_top(app, b, COUNT_OF(b));
        if(!n) {
            FURI_LOG_W(TAG, "save busy: nothing measured yet");
            return;
        }
        Storage* st = furi_record_open(RECORD_STORAGE);
        bookmarks_write_auto(app, st, b, n);
        furi_record_close(RECORD_STORAGE);
        FURI_LOG_I(TAG, "save busy: wrote %u channels", (unsigned)n);
        return;
    }
    if(index == CONF2_ROW_CLEARPK) {
        app->clear_peaks = true;
        FURI_LOG_I(TAG, "config: clear peaks queued");
        return;
    }
    if(index == CONF2_ROW_WFSAVE) {
        app->wf_export = true;
        FURI_LOG_I(TAG, "config: waterfall export queued");
        return;
    }
#ifdef RFS_LAB
    if(index == CONF2_ROW_BRUTE) {
        // LAB ONLY. One PAGE per press, then it stops and waits to be asked again. No
        // loop-forever path: 32 codes x 3 repeats is ~3 s of airtime, the cursor advances, and
        // the row shows where the next press starts. No region gate in the lab build (see rf_tx.h).
        if(app->brute_next <= 0xFFFFFFu) {
            uint32_t f = app->f_start + (uint32_t)(app->nbins / 2) * app->f_step;
            app->running = false;
            if(app->worker) {
                furi_thread_join(app->worker);
                furi_thread_free(app->worker);
                app->worker = NULL;
            }
            RfTxStatus st =
                rf_tx_code_page(f, PRESETS[0].regs, app->brute_next, BRUTE_PAGE, 3, 350);
            FURI_LOG_I(TAG, "brute page %06lX -> %d", (unsigned long)app->brute_next, (int)st);
            // advance only over codes that actually went out: a refused page must be retried, not
            // silently skipped (it used to move on by 32 even when nothing was transmitted)
            if(st == RfTxOk) app->brute_next += BRUTE_PAGE;
            if(app->it_brute) {
                char b[12];
                snprintf(b, sizeof(b), "%06lX", (unsigned long)app->brute_next);
                variable_item_set_current_value_text(app->it_brute, b);
            }
            app->running = true;
            app->reconfig = true;
            app->worker = furi_thread_alloc_ex("rfsurvey_sweep", 6144, sweep_worker, app);
            furi_thread_start(app->worker);
        }
        return;
    }
    if(index == CONF2_ROW_TXFRAME) {
        // LAB ONLY. Synthesises a Princeton frame from nothing but arithmetic -- no capture,
        // no decoder. The encoder lives in rf_logic.h and is unit-tested on the PC, so the
        // waveform shape is verified before it is allowed near a transmitter.
        uint32_t f = app->f_start + (uint32_t)(app->nbins / 2) * app->f_step;
        uint16_t n = 0;
        for(uint8_t r = 0; r < 6 && app->rep_buf; r++) {
            uint16_t k = rfs_encode_princeton(
                0x0A5C3Eu, 350, app->rep_buf + n, (uint16_t)(REP_MAX_EDGES - n));
            if(!k) break;
            n = (uint16_t)(n + k);
        }
        if(n) {
            app->running = false;
            if(app->worker) {
                furi_thread_join(app->worker);
                furi_thread_free(app->worker);
                app->worker = NULL;
            }
            RfTxStatus st = rf_tx_edges(f, PRESETS[0].regs, app->rep_buf, n);
            FURI_LOG_I(TAG, "tx frame %lu Hz %u edges -> %d", (unsigned long)f, (unsigned)n,
                       (int)st);
            app->running = true;
            app->reconfig = true;
            app->worker = furi_thread_alloc_ex("rfsurvey_sweep", 6144, sweep_worker, app);
            furi_thread_start(app->worker);
        }
        return;
    }
    if(index == CONF2_ROW_REPLAY) {
        // LAB ONLY. Re-sends the last capture through the same frequency checks as the TX test.
        // The radio is handed over the same way: stop and join the worker first.
        if(!app->rep_buf || app->rep_n < 8) {
            FURI_LOG_W(TAG, "replay: nothing captured yet");
            return;
        }
        app->running = false;
        if(app->worker) {
            furi_thread_join(app->worker);
            furi_thread_free(app->worker);
            app->worker = NULL;
        }
        uint8_t pi = app->rep_preset < PRESET_N ? app->rep_preset : 0;
        RfTxStatus st = rf_tx_edges(app->rep_freq, PRESETS[pi].regs, app->rep_buf, app->rep_n);
        FURI_LOG_I(
            TAG,
            "replay %lu Hz %u edges -> %d",
            (unsigned long)app->rep_freq,
            (unsigned)app->rep_n,
            (int)st);
        app->running = true;
        app->reconfig = true;
        app->worker = furi_thread_alloc_ex("rfsurvey_sweep", 6144, sweep_worker, app);
        furi_thread_start(app->worker);
        return;
    }
    if(index == CONF2_ROW_TXTEST) {
        // LAB ONLY. The radio has one owner and it is the sweep worker, so the worker is
        // stopped and joined before the transmitter touches it -- the same handover the capture
        // screen does. 20 pulses x 500 us is ~20 ms of airtime: a connectivity test, not a
        // carrier.
        uint32_t f = app->f_start + (uint32_t)(app->nbins / 2) * app->f_step;
        app->running = false;
        if(app->worker) {
            furi_thread_join(app->worker);
            furi_thread_free(app->worker);
            app->worker = NULL;
        }
        RfTxStatus st =
            rf_tx_burst(f, subghz_device_cc1101_preset_ook_650khz_async_regs, 20, 500, 500);
        FURI_LOG_I(TAG, "tx test %lu Hz -> %d", (unsigned long)f, (int)st);
        app->running = true;
        app->reconfig = true;
        app->worker = furi_thread_alloc_ex("rfsurvey_sweep", 6144, sweep_worker, app);
        furi_thread_start(app->worker);
        return;
    }
#endif
    if(index == CONF2_ROW_SCAN) { // back to page 1
        FURI_LOG_I(TAG, "config: -> scan");
        view_dispatcher_switch_to_view(app->vd, VIEW_CONF);
        return;
    }
    FURI_LOG_I(TAG, "config: OK -> spectrum");
    view_dispatcher_switch_to_view(app->vd, VIEW_SPEC);
}

static uint32_t view_exit(void* ctx) {
    UNUSED(ctx);
    return VIEW_NONE;
}

static uint32_t to_conf(void* ctx) {
    UNUSED(ctx);
    return VIEW_CONF; // Back from the spectrum returns to config
}

// the haptic cue only runs while the spectrum view is on screen (config/editor/capture quiet)
static void spec_enter(void* ctx) {
    App* app = ctx;
    app->spec_active = true;
    app->cap_redraw = false; // the capture view is gone; never repaint it from here
}
static void spec_exit(void* ctx) {
    App* app = ctx;
    // ONLY publish the flag. The sweep worker is the sole owner of the vibro motor + haptic state;
    // it releases the motor on the next bin once it sees spec_active is false (see the else branch
    // in the sweep loop). Touching hap_on/the motor from this GUI thread races the worker and can
    // latch the motor ON (worker turns it on between our hap_on read and write).
    app->spec_active = false;
    // parking is a property of the cadence/hunt pages: leaving the spectrum entirely (Back to
    // config) must stop it, or a later sweep display looks live while the worker is still parked
    app->parked = false;
}

// ---- custom range editor (VIEW_RANGE) --------------------------------------
// Styled after the stock Frequency Analyzer: the active field (Start or End) is a big
// boxed number; Left/Right pick a digit, Up/Down change it, OK swaps Start<->End, Back
// validates and applies. Values are in 100 kHz units (779.0 MHz = 7790).

static const uint32_t RE_PLACE[4] = {1000, 100, 10, 1}; // hundreds, tens, ones, tenths

static void range_draw(Canvas* canvas, void* model) {
    UNUSED(model);
    App* app = g_app;
    if(!app) return;
    canvas_clear(canvas);
    // the cursor spans both numbers: digits 0..3 edit Start, 4..7 edit End
    uint8_t field = app->re_cur < 4 ? 0 : 1; // 0 = Start, 1 = End
    uint8_t digit = app->re_cur % 4;
    uint32_t a = field ? app->re_end : app->re_start;
    char big[12];
    snprintf(big, sizeof(big), "%lu.%lu", (unsigned long)(a / 10), (unsigned long)(a % 10));

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 9, field ? "End" : "Start");
    canvas_draw_str(canvas, 108, 9, "MHz");

    // inverted box with the big number (like the analyzer's locked readout)
    canvas_draw_box(canvas, 2, 12, 124, 22);
    canvas_set_color(canvas, ColorWhite);
    canvas_set_font(canvas, FontBigNumbers);
    canvas_draw_str(canvas, 6, 30, big);
    // underline the active digit (digit idx -> char idx; the '.' is char 3)
    int ci = digit < 3 ? digit : 4;
    char pre[8];
    memcpy(pre, big, (size_t)ci);
    pre[ci] = 0;
    int x0 = 6 + (int)canvas_string_width(canvas, pre);
    char one[2] = {big[ci], 0};
    int w = (int)canvas_string_width(canvas, one);
    canvas_draw_line(canvas, x0, 32, x0 + (w > 0 ? w - 1 : 2), 32);
    canvas_set_color(canvas, ColorBlack);

    // the other field + the resulting span, below the box
    canvas_set_font(canvas, FontSecondary);
    uint32_t o = field ? app->re_start : app->re_end;
    uint32_t span = (app->re_end > app->re_start) ? app->re_end - app->re_start : 0;
    char info[40];
    snprintf(
        info,
        sizeof(info),
        "%s %lu.%lu  span %lu.%lu",
        field ? "Start" : "End",
        (unsigned long)(o / 10),
        (unsigned long)(o % 10),
        (unsigned long)(span / 10),
        (unsigned long)(span % 10));
    canvas_draw_str(canvas, 2, 46, info);

    elements_button_left(canvas, "Digit"); // Left/Right move the cursor across both numbers
    elements_button_center(canvas, "Set"); // OK applies; Back cancels
}

static bool range_input(InputEvent* e, void* ctx) {
    // EXPERIMENT: shot_flush
    UNUSED(ctx);
    App* app = g_app;
    if(!app) return false;
    // PORT: native encoder axes are already correct here (wheel = digit value via Up/Down,
    // OK-held+turn = digit cursor via Left/Right); only drop the trailing spurious OK Long.
    if(!enc_prefilter(e)) return true;
    bool act = (e->type == InputTypeShort || e->type == InputTypeRepeat);

    if(e->key == InputKeyBack && e->type == InputTypeShort) {
        view_dispatcher_switch_to_view(app->vd, VIEW_CONF); // cancel: leave the range unchanged
        return true;
    }
    if(e->key == InputKeyOk && e->type == InputTypeShort) {
        // Set: validate (clamp to the CC1101 window, enforce a minimum span), apply, return
        if(app->re_start < RE_MIN) app->re_start = RE_MIN;
        if(app->re_end > RE_MAX) app->re_end = RE_MAX;
        if(app->re_end < app->re_start + RE_SPAN) {
            app->re_end = app->re_start + RE_SPAN;
            if(app->re_end > RE_MAX) {
                app->re_end = RE_MAX;
                app->re_start = RE_MAX - RE_SPAN;
            }
        }
        app->f_start = app->re_start * 100000;
        app->f_end = app->re_end * 100000;
        // a hand-typed range can be wide enough that the current step would overflow MAX_BINS
        app->f_step = step_fit(app->f_start, app->f_end, app->f_step);
        conf_sync_rows(app); // range, step AND band: the Band row kept the old name before
        app->parked = false; // new range -> the parked frequency may be outside it
        app->reconfig = true;
        FURI_LOG_I(
            TAG, "range: set %lu-%lu", (unsigned long)app->f_start, (unsigned long)app->f_end);
        view_dispatcher_switch_to_view(app->vd, VIEW_CONF);
        return true;
    }

    // the cursor spans both numbers (0..3 Start, 4..7 End); Up/Down edit the active digit
    uint8_t field = app->re_cur < 4 ? 0 : 1;
    uint8_t digit = app->re_cur % 4;
    uint32_t* v = field ? &app->re_end : &app->re_start;
    if(e->key == InputKeyLeft && act) {
        if(app->re_cur > 0) app->re_cur--;
    } else if(e->key == InputKeyRight && act) {
        if(app->re_cur < 7) app->re_cur++;
    } else if(e->key == InputKeyUp && act) {
        uint32_t nv = *v + RE_PLACE[digit];
        *v = (nv > RE_MAX) ? RE_MAX : nv;
    } else if(e->key == InputKeyDown && act) {
        uint32_t p = RE_PLACE[digit];
        *v = (*v > RE_MIN + p) ? (*v - p) : RE_MIN;
    } else {
        return true;
    }
    with_view_model(app->range_view, void** m, { UNUSED(m); }, true);
    return true;
}

// ---- .sub capture (VIEW_CAP) -----------------------------------------------
// Re-implements the stock RAW recorder: start_async_rx streams (level,duration) edges from an
// ISR. The ISR drops <50 us noise and >=1 s gaps (the latter also keeps the signed int32
// packing from overflowing) and pushes the rest to a stream buffer; the capture thread writes
// RAW_Data rows to a .sub the stock SubGHz app can replay. The survey worker is fully stopped
// first so only one owner touches the radio. (Dropped edges are not merged -- fine for clean
// OOK/FSK bursts; very noisy inputs may not replay perfectly.)

#define CAP_STREAM 4096 // int32 slots ISR -> thread (sized for a burst; overflow is counted)
#define CAP_MAXSPL 60000 // auto-stop after this many durations (bounds the file)
// Pre-trigger history, PortaPack's Signal Hunter idea: when the RSSI gate is on, the edges from
// BEFORE the trigger are the most valuable part (the preamble and the first frame), and they
// used to be thrown away. One ring, written out in order the moment the gate opens.
#define CAP_PRERUN 256

// ISR context: merge the edge into the pending run and push a run when its level ends; never
// block, never allocate, never touch the file.
//
// The old code dropped any edge shorter than 50 us. Dropping an EDGE is not a glitch filter:
// the .sub format is a signed alternation, so one missing edge inverts the polarity of every
// pulse after it and the file stops replaying. Merging is the correct operation -- a short
// opposite pulse between two same-level pulses is folded back into the run that contains it,
// so 400 us | 30 us | 400 us becomes one 830 us run instead of a sign flip.
#define CAP_GLITCH_US 50
static void capture_isr_cb(bool level, uint32_t duration, void* ctx) {
    App* app = ctx;
    FuriStreamBuffer* sb = app->cap_stream;
    if(!sb) return;

    if(duration >= 1000000) { // sub-1 Hz gap: close the run, drop the gap itself
        if(app->cap_run_active) {
            int32_t d =
                app->cap_run_level ? (int32_t)app->cap_run_dur : -(int32_t)app->cap_run_dur;
            if(furi_stream_buffer_send(sb, &d, sizeof(d), 0) != sizeof(d)) app->cap_overflow++;
            app->cap_run_active = false;
        }
        return;
    }
    if(!app->cap_run_active) {
        app->cap_run_active = true;
        app->cap_run_level = level;
        app->cap_run_dur = duration;
        return;
    }
    if(duration < CAP_GLITCH_US || level == app->cap_run_level) { // glitch: extend, keep level
        uint32_t nd = app->cap_run_dur + duration;
        app->cap_run_dur = (nd > 0x3FFFFFFF) ? 0x3FFFFFFF : nd;
        return;
    }
    int32_t d = app->cap_run_level ? (int32_t)app->cap_run_dur : -(int32_t)app->cap_run_dur;
    if(furi_stream_buffer_send(sb, &d, sizeof(d), 0) != sizeof(d)) app->cap_overflow++;
    app->cap_run_level = level;
    app->cap_run_dur = duration;
}

// The firmware's decoder chain, fed the same edge stream we write to the .sub. This is the
// difference between "pulse widths look like the PT2262 family" and "Came 12-bit, ID 0x1A3F":
// subghz_receiver_* are exported to FAPs (verified against api_symbols.csv and a link test).
static void cap_decode_cb(SubGhzReceiver* rx, SubGhzProtocolDecoderBase* base, void* ctx) {
    App* app = ctx;
    FuriString* str = furi_string_alloc();
    if(subghz_protocol_decoder_base_get_string(base, str)) {
        const char* c = furi_string_get_cstr(str);
        size_t n = 0;
        for(; c[n] && n + 1 < sizeof(app->cap_decode); n++)
            app->cap_decode[n] = (c[n] == '\n' || c[n] == '\r') ? ' ' : c[n];
        app->cap_decode[n] = 0;
        app->cap_decoded = true;
        if(app->cap_packets < 0xFFFF) app->cap_packets++;
    }
    furi_string_free(str);
    subghz_receiver_reset(rx); // one frame per burst; the next frame re-syncs
}

// Everything the capture knows, written next to the .sub instead of squeezed onto a 128x64
// line: the screen keeps the one number that matters (base pulse, bits, FIX/ROL) and the file
// keeps the whole analysis for the PC.
static void capture_sidecar(
    App* app, // app->csv_line is the 512-byte scratch buffer; the sweep worker is stopped here
    Storage* storage,
    const char* sub_path,
    uint32_t freq,
    const char* preset,
    uint32_t samples,
    uint32_t base,
    uint32_t frame,
    uint16_t bits,
    uint32_t baud,
    RfsPreamble pre,
    uint8_t code) {
    char path[96];
    snprintf(path, sizeof(path), "%s", sub_path);
    char* dot = strrchr(path, '.');
    if(!dot) return;
    snprintf(dot, 5, ".txt");

    File* f = storage_file_alloc(storage);
    if(!storage_file_open(f, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_close(f);
        storage_file_free(f);
        return;
    }
    const char* kind = (code == RFS_CODE_STATIC) ? "FIX" :
                       (code == RFS_CODE_CHANGING) ? "ROL" : "unknown";
    char* buf = app->csv_line;
    const size_t cap = CSV_BUF;
    int n = snprintf(
        buf,
        cap,
        "# RF Survey capture analysis\n"
        "freq %lu\n"
        "preset %s\n"
        "samples %lu\n"
        "base_us %lu\n"
        "frame_us %lu\n"
        "bits %u\n"
        "baud %lu\n"
        "preamble %u\n"
        "sync_us %lu\n"
        "code %s\n"
        "\n"
        "# base_us  the short pulse T (10th percentile of the edge lengths)\n"
        "# bits     edges per frame / 2 -- one bit of a pulse-width code costs two edges\n"
        "# code     FIX = two frames identical (replayable), ROL = they differ\n",
        (unsigned long)freq,
        preset,
        (unsigned long)samples,
        (unsigned long)base,
        (unsigned long)frame,
        (unsigned)bits,
        (unsigned long)baud,
        (unsigned)pre.count,
        (unsigned long)pre.sync_us,
        kind);
    if(n > 0 && (size_t)n < cap) storage_file_write(f, buf, (size_t)n);
    storage_file_close(f);
    storage_file_free(f);
    FURI_LOG_I(TAG, "cap: sidecar %s", path);
}

static int32_t capture_worker(void* ctx) {
    App* app = ctx;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    const SurveyPreset* ps = &PRESETS[app->cap_preset < PRESET_N ? app->cap_preset : 0];

    storage_common_mkdir(storage, "/ext/subghz");
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);
    char path[96];
    snprintf(
        path,
        sizeof(path),
        "/ext/subghz/rfsurvey_%lu_%02u%02u%02u_%03lu.sub",
        (unsigned long)(app->cap_freq / 1000),
        dt.hour,
        dt.minute,
        dt.second,
        (unsigned long)(furi_get_tick() % 1000)); // tick suffix: never overwrite a prior .sub

    // Listen mode decodes without touching the card: some sensors (weather, TPMS) transmit once
    // a minute, so the useful interaction is "leave it listening", not "catch a burst by hand".
    const bool to_file = !app->cap_listen;
    FlipperFormat* ff = to_file ? flipper_format_file_alloc(storage) : NULL;
    int32_t* buf = malloc(sizeof(int32_t) * 512);
    app->cap_stream = furi_stream_buffer_alloc(sizeof(int32_t) * CAP_STREAM, sizeof(int32_t));
    bool ok = buf && app->cap_stream &&
              (!to_file || flipper_format_file_open_always(ff, path));
    if(ok && to_file) {
        uint32_t ver = 1, freq = app->cap_freq;
        ok = flipper_format_write_header_cstr(ff, "Flipper SubGhz RAW File", ver) &&
             flipper_format_write_uint32(ff, "Frequency", &freq, 1) &&
             flipper_format_write_string_cstr(ff, "Preset", ps->sub) &&
             flipper_format_write_string_cstr(ff, "Protocol", "RAW");
    }
    if(ok && !furi_hal_subghz_is_frequency_valid(app->cap_freq)) {
        // belt-and-suspenders: the input handler already blocks tuning into a band gap, but never
        // hand the HAL an unsupported frequency -- set_frequency can furi_crash on a gap.
        FURI_LOG_E(TAG, "cap: invalid frequency %lu", (unsigned long)app->cap_freq);
        ok = false;
    }
    if(!ok) {
        FURI_LOG_E(TAG, "cap: open/header failed %s", path);
        app->capturing = false;
    } else {
        FURI_LOG_I(
            TAG,
            "cap: %s %lu Hz %s -> %s",
            to_file ? "recording" : "listening",
            (unsigned long)app->cap_freq,
            ps->name,
            to_file ? path : "-");
        app->cap_run_active = false; // never carry a run over from a previous capture
        app->cap_decode[0] = 0;
        app->cap_decoded = false;
        app->cap_packets = 0;
        app->cap_edge_n = 0;
#ifdef RFS_LAB
        app->rep_n = 0;
        app->rep_freq = app->cap_freq;
        app->rep_preset = app->cap_preset;
#endif
        // decoder chain: optional. If the environment or the receiver cannot be allocated the
        // capture still runs -- decoding is an enrichment, never a prerequisite.
        SubGhzEnvironment* env = subghz_environment_alloc();
        SubGhzReceiver* dec = NULL;
        if(env) {
            subghz_environment_set_protocol_registry(env, &subghz_protocol_registry);
            dec = subghz_receiver_alloc_init(env);
            if(dec) {
                subghz_receiver_set_filter(dec, SubGhzProtocolFlag_Decodable);
                subghz_receiver_set_rx_callback(dec, cap_decode_cb, app);
            }
        }
        radio_setup(app); // internal reset, or external cc1101_ext up (5V/begin); may fall back
        radio_load_preset(ps->regs);
        radio_set_freq(app->cap_freq);
        radio_async_start(capture_isr_cb, app);

        int ind = 0;
        bool werr = false;
        uint32_t last_poll = 0;
        int32_t* pre = app->cap_gate ? malloc(sizeof(int32_t) * CAP_PRERUN) : NULL;
        uint16_t pre_n = 0, pre_i = 0;
        bool was_paused = true, flush_pre = false;
        app->cap_paused = app->cap_gate; // if gating, wait for signal before the first write
        while(app->capturing && !werr && app->cap_samples < CAP_MAXSPL) {
            // poll the level ~50 Hz for the on-screen readout; only PAUSE writing when the gate
            // is on and we're below the Trigger (off by default -> record everything).
            uint32_t now = furi_get_tick();
            if(now - last_poll >= 20) {
                float r = radio_rssi();
                app->cap_rssi = (r < -127.0f) ? (int8_t)-127 : (int8_t)r;
                app->cap_paused = app->cap_gate && (app->cap_rssi < app->trigger);
                if(app->cap_gate) {
                    if(was_paused && !app->cap_paused) flush_pre = true; // gate just opened
                    was_paused = app->cap_paused;
                }
                last_poll = now;
                // same strength-tracked locate cue as the spectrum (non-blocking: pump only toggles
                // when its own timer elapses, so the stream drain is never stalled).
                if(app->haptic && app->notif) {
                    haptic_sample(app, app->cap_rssi);
                    haptic_pump(app, now);
                } else if(app->hap_on) {
                    haptic_off(app); // symmetry with the sweep loop: never leave a pulse latched
                }
            }
            int32_t d;
            if(furi_stream_buffer_receive(app->cap_stream, &d, sizeof(d), 20) == sizeof(d)) {
                if(pre) { // always remember the recent past, even while gated off
                    pre[pre_i] = d;
                    pre_i = (uint16_t)((pre_i + 1) % CAP_PRERUN);
                    if(pre_n < CAP_PRERUN) pre_n++;
                }
                if(app->cap_paused) continue; // below trigger -> drain but don't record
                if(flush_pre && pre && pre_n) {
                    flush_pre = false;
                    for(uint16_t k = 0; k < pre_n; k++) {
                        buf[ind++] = pre[(uint16_t)((pre_i + CAP_PRERUN - pre_n + k) % CAP_PRERUN)];
                        app->cap_samples++;
                        if(ind == 512) {
                            // PORT FIX: the original wrote `!(werr = !write())` and `break`-ed on a
                            // SUCCESSFUL write, skipping `ind = 0`. ind stayed 512, so the next
                            // `buf[ind++] = d` wrote one past the malloc(512*int32) buffer and then
                            // grew unbounded (heap corruption). Match the live-edge path below: record
                            // any error in `werr` (the while() at the top of this loop exits on werr)
                            // and ALWAYS reset ind so it can never exceed the buffer.
                            if(to_file) werr = !flipper_format_write_int32(ff, "RAW_Data", buf, 512);
                            ind = 0;
                        }
                    }
                    pre_n = 0; // consumed; the live edges follow
                }
                buf[ind++] = d;
                app->cap_samples++;
                uint32_t m = (d < 0) ? (uint32_t)(-d) : (uint32_t)d;
                uint8_t b = pw_bucket(m);
                if(app->cap_pw[b] < 0xFFFF) app->cap_pw[b]++;
                app->cap_pw_n++;
                if(app->cap_edge_n < RFS_FRAME_EDGES) app->cap_edge[app->cap_edge_n++] = d;
#ifdef RFS_LAB
                if(app->rep_buf && app->rep_n < REP_MAX_EDGES) app->rep_buf[app->rep_n++] = d;
#endif
                if(dec) subghz_receiver_decode(dec, d > 0, m);
                if(ind == 512) {
                    if(to_file) werr = !flipper_format_write_int32(ff, "RAW_Data", buf, 512);
                    ind = 0;
                }
            }
        }
        // teardown order is non-negotiable: stop the ISR FIRST so no callback can fire into
        // the stream after we free it; then drain the tail, flush, close, free.
        radio_async_stop();
        // the merge holds the last run back until the next level change -> flush it by hand,
        // otherwise the tail of every capture is silently lost
        if(app->cap_run_active) {
            int32_t t =
                app->cap_run_level ? (int32_t)app->cap_run_dur : -(int32_t)app->cap_run_dur;
            if(furi_stream_buffer_send(app->cap_stream, &t, sizeof(t), 0) != sizeof(t))
                app->cap_overflow++;
            app->cap_run_active = false;
        }
        int32_t d;
        while(furi_stream_buffer_receive(app->cap_stream, &d, sizeof(d), 0) == sizeof(d)) {
            buf[ind++] = d;
            app->cap_samples++;
            uint32_t m = (d < 0) ? (uint32_t)(-d) : (uint32_t)d;
            uint8_t b = pw_bucket(m);
            if(app->cap_pw[b] < 0xFFFF) app->cap_pw[b]++;
            app->cap_pw_n++;
            if(ind == 512) { // keep draining the tail instead of dropping it at 512
                if(to_file) flipper_format_write_int32(ff, "RAW_Data", buf, 512);
                ind = 0;
            }
        }
        if(ind > 0 && to_file) flipper_format_write_int32(ff, "RAW_Data", buf, (uint16_t)ind);
        if(pre) free(pre);
        radio_teardown();
        // analysis sidecar: computed from the same edge buffer the displays use
        if(to_file) {
            uint32_t abase, aframe;
            uint16_t abits;
            rfs_frame_estimate(app->cap_edge, app->cap_edge_n, &abase, &aframe, &abits);
            RfsPreamble pre = rfs_preamble(app->cap_edge, app->cap_edge_n, abase);
            uint8_t ck = rfs_code_kind(app->cap_edge, app->cap_edge_n, abase);
            uint32_t baud = aframe ? ((uint32_t)abits * 1000000u) / aframe : 0;
            capture_sidecar(
                app,
                storage,
                path,
                app->cap_freq,
                ps->name,
                app->cap_samples,
                abase,
                aframe,
                abits,
                baud,
                pre,
                ck);
        }
        if(dec) subghz_receiver_free(dec);
        if(env) subghz_environment_free(env);
        haptic_off(app); // release the locate motor when the capture stops
        FURI_LOG_I(
            TAG,
            "cap: stop, %lu samples, %lu overflow",
            (unsigned long)app->cap_samples,
            (unsigned long)app->cap_overflow);
    }

    // stop_async_rx already ran in the success path (before any free); if the file never
    // opened, nothing was started, so there is nothing to stop here. Free in the safe order.
    if(ff) flipper_format_free(ff);
    if(buf) free(buf);
    if(app->cap_stream) {
        furi_stream_buffer_free(app->cap_stream);
        app->cap_stream = NULL;
    }
    furi_record_close(RECORD_STORAGE);
    app->capturing = false;
    app->cap_redraw = true; // auto-stop (CAP_MAXSPL) never gets an input event -> paint once
    return 0;
}

// One-line reading of the captured envelope's duration histogram: base pulse, ratio to the
// second cluster, and the encoding family that ratio implies. Heuristic by design -- it names
// the FAMILY, never a specific decoder or a device. "b348 x3.0 PT2262/EV" = 348 us base with a
// 1:3 pulse-width scheme, which is the PT2262/EV1527/CAME class of fixed-code remotes.
static void pw_signature(const App* app, char* out, size_t n) {
    int b1 = -1, b2 = -1;
    for(int i = 1; i < PW_N; i++) { // skip bucket 0: sub-50 us is ISR-filtered noise
        if(app->cap_pw[i] == 0) continue;
        if(b1 < 0 || app->cap_pw[i] > app->cap_pw[b1]) {
            b2 = b1;
            b1 = i;
        } else if(b2 < 0 || app->cap_pw[i] > app->cap_pw[b2]) {
            b2 = i;
        }
    }
    if(b1 < 0) {
        snprintf(out, n, "no edges");
        return;
    }
    uint32_t base = pw_center((uint8_t)b1);
    if(b2 < 0) {
        snprintf(out, n, "b%uus single", (unsigned)base);
        return;
    }
    uint32_t sec = pw_center((uint8_t)b2);
    unsigned r10 = base ? (unsigned)((sec * 10u + base / 2) / base) : 0;
    const char* fam = "?";
    if(base >= 250 && base <= 460) {
        if(r10 >= 27 && r10 <= 34)
            fam = "PT2262/EV";
        else if(r10 >= 18 && r10 <= 22)
            fam = "biphase";
        else if(r10 <= 12)
            fam = "steady";
    } else if(base > 460 && base <= 700) {
        fam = "Linear?";
    } else if(base > 700) {
        fam = "slow/sync";
    }
    // one bit of a pulse-width encoding costs two edges, so once the long silence delimits a
    // frame the bit count falls out -- the number the family name could only imply
    uint32_t fbase, fframe;
    uint16_t fbits;
    rfs_frame_estimate(app->cap_edge, app->cap_edge_n, &fbase, &fframe, &fbits);
    // the one observation that decides whether replay is even possible
    uint8_t ck = rfs_code_kind(app->cap_edge, app->cap_edge_n, base);
    const char* code = (ck == RFS_CODE_STATIC) ? "FIX" :
                       (ck == RFS_CODE_CHANGING) ? "ROL" : NULL;
    if(fbits >= 8) {
        if(code)
            snprintf(out, n, "b%u %ub %s", (unsigned)base, (unsigned)fbits, code);
        else
            snprintf(out, n, "b%u %ub", (unsigned)base, (unsigned)fbits);
    } else if(code) {
        snprintf(out, n, "b%u x%u.%u %s", (unsigned)base, r10 / 10, r10 % 10, code);
    } else {
        snprintf(out, n, "b%u x%u.%u %s", (unsigned)base, r10 / 10, r10 % 10, fam);
    }
}

static void capture_draw(Canvas* canvas, void* model) {
    UNUSED(model);
    App* app = g_app;
    if(!app) return;
    canvas_clear(canvas);
    const SurveyPreset* ps = &PRESETS[app->cap_preset < PRESET_N ? app->cap_preset : 0];

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(
        canvas, 2, 9, (app->capturing && app->cap_listen) ? "Listening" : "Capture .sub");
    canvas_draw_str_aligned(canvas, 126, 9, AlignRight, AlignBottom, ps->name); // preset, right

    canvas_draw_box(canvas, 2, 12, 124, 22);
    canvas_set_color(canvas, ColorWhite);
    canvas_set_font(canvas, FontBigNumbers);
    char big[12];
    snprintf(
        big,
        sizeof(big),
        "%lu.%03lu",
        (unsigned long)(app->cap_freq / 1000000),
        (unsigned long)((app->cap_freq / 1000) % 1000));
    canvas_draw_str(canvas, 6, 30, big);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 104, 30, "MHz"); // small, inside the box next to the number
    canvas_set_color(canvas, ColorBlack);

    // one status line above the pills (never over them)
    canvas_set_font(canvas, FontSecondary);
    char line[40];
    if(app->capturing) {
        if(app->cap_listen) {
            if(app->cap_decoded)
                snprintf(line, sizeof(line), "%s", app->cap_decode);
            else
                snprintf(line, sizeof(line), "listening... %u pkts", (unsigned)app->cap_packets);
        } else if(app->cap_gate && app->cap_paused)
            snprintf(line, sizeof(line), "signal below range (%d)", app->cap_rssi);
        else
            snprintf(
                line,
                sizeof(line),
                "REC  %lu spl%s  %d dBm",
                (unsigned long)app->cap_samples,
                app->cap_overflow ? "!" : "",
                app->cap_rssi);
    } else if(app->cap_decoded) {
        // the firmware recognised a protocol: that beats any guess from pulse widths
        snprintf(line, sizeof(line), "%s", app->cap_decode);
    } else if(app->cap_pw_n > 0) {
        // idle after a capture: the envelope's duration signature (family, not a decoder)
        pw_signature(app, line, sizeof(line));
    } else {
        // idle: Up/Dn tunes the frequency, long-OK switches file/listen, long-L/R the filter
        snprintf(
            line,
            sizeof(line),
            "%s  filt:%s",
            app->cap_listen ? "LISTEN" : "FILE",
            app->cap_gate ? "on" : "off");
    }
    canvas_draw_str(canvas, 2, 45, line);

    if(app->capturing) {
        elements_button_center(canvas, "Stop");
    } else {
        elements_button_left(canvas, "Mod");
        elements_button_center(canvas, "Rec");
        elements_button_right(canvas, "Mod");
    }
}

// Join + free the capture thread handle. Safe whether the thread is still running (sets
// capturing=false to ask it to stop, then waits) or already self-finished (auto-stop / open
// or write failure left the handle unreaped). Called before every start so there is never a
// second worker racing the old one's stream free.
static void capture_stop(App* app) {
    if(!app->cap_worker) return;
    app->capturing = false;
    furi_thread_join(app->cap_worker); // returns at once if the thread already exited
    furi_thread_free(app->cap_worker);
    app->cap_worker = NULL;
}

static void capture_start(App* app) {
    capture_stop(app); // reap any prior worker (running or self-finished) before a new one
    app->capturing = true;
    app->cap_samples = 0;
    app->cap_overflow = 0;
    memset(app->cap_pw, 0, sizeof(app->cap_pw)); // signature is per-capture
    app->cap_pw_n = 0;
    app->cap_worker = furi_thread_alloc_ex("rfsurvey_cap", 6144, capture_worker, app);
    if(!app->cap_worker) { // no thread -> nothing to join later; report and stay idle
        FURI_LOG_E(TAG, "capture: out of memory, not starting");
        app->capturing = false;
        return;
    }
    furi_thread_start(app->cap_worker);
}

static bool capture_input(InputEvent* e, void* ctx) {
    // EXPERIMENT: shot_flush
    UNUSED(ctx);
    App* app = g_app;
    if(!app) return false;
    // PORT: native encoder axes are already correct here (wheel = tune via Up/Down, OK-held+turn =
    // preset via Left/Right); drop the trailing spurious OK Long. The RSSI gate toggle needs
    // Left/Right LONG, which the encoder never emits, so the side key held (Back Long) stands in.
    if(!enc_prefilter(e)) return true;
    if(s_is_encoder && e->key == InputKeyBack && e->type == InputTypeLong) e->key = InputKeyRight;
    bool sp = (e->type == InputTypeShort || e->type == InputTypeRepeat);
    bool lp = (e->type == InputTypeLong);

    if(e->key == InputKeyBack && e->type == InputTypeShort) {
        capture_stop(app);
        // restart a fresh survey worker (re-inits the radio exactly like first boot).
        // Leaving capture returns to a SWEEP, not to the parked view you came via: the worker
        // would otherwise re-enter park_loop and the spectrum would look dead.
        app->parked = false;
        app->running = true;
        app->reconfig = true;
        app->worker = furi_thread_alloc_ex("rfsurvey_sweep", 6144, sweep_worker, app);
        furi_thread_start(app->worker);
        view_dispatcher_switch_to_view(app->vd, VIEW_SPEC);
        return true;
    }
    if(e->key == InputKeyOk && e->type == InputTypeShort) {
        if(app->capturing)
            capture_stop(app);
        else
            capture_start(app);
    } else if(e->key == InputKeyOk && lp && !app->capturing) {
        app->cap_listen = !app->cap_listen; // long-OK: write a .sub, or just listen and decode
    } else if((e->key == InputKeyLeft || e->key == InputKeyRight) &&
              e->type == InputTypeLong && !app->capturing) {
        app->cap_gate = !app->cap_gate; // long-Left/Right: RSSI filter on/off
    } else if(!app->capturing && sp && (e->key == InputKeyLeft || e->key == InputKeyRight)) {
        // Left/Right: capture modulation (OOK vs 2FSK) -- the user must match the signal
        if(e->key == InputKeyRight)
            app->cap_preset = (uint8_t)((app->cap_preset + 1) % PRESET_N);
        else
            app->cap_preset = (uint8_t)((app->cap_preset + PRESET_N - 1) % PRESET_N);
    } else if(!app->capturing && sp && (e->key == InputKeyUp || e->key == InputKeyDown)) {
        // Up/Down: fine-tune the capture frequency by 10 kHz, clamped to the CC1101 range. Only
        // accept a step that lands on a HAL-valid frequency, so you can't tune into a band gap
        // (348-387 / 464-779 MHz) -- capture would otherwise pick the wrong path and feed the HAL
        // an unsupported frequency.
        uint32_t f = app->cap_freq;
        if(e->key == InputKeyUp)
            f = (f <= 928000000 - 10000) ? f + 10000 : 928000000;
        else
            f = (f >= 300000000 + 10000) ? f - 10000 : 300000000;
        if(furi_hal_subghz_is_frequency_valid(f)) app->cap_freq = f;
    } else {
        return true;
    }
    with_view_model(app->cap_view, void** m, { UNUSED(m); }, true);
    return true;
}

// ---- config screen ---------------------------------------------------------

// Finest STEP_HZ entry that still fits MAX_BINS across [lo,hi], never finer than `want`.
// Every path that can change the range or the step funnels through this: a step that would
// overflow the bin array used to make the worker silently rewrite f_end, so a "Full spectrum"
// sweep quietly covered only 300-351 MHz. want = STEP_HZ[0] means "the finest that fits"
// (band presets); want = the current step means "keep the user's choice, coarsen if forced".
static uint32_t step_fit(uint32_t lo, uint32_t hi, uint32_t want) {
    return rfs_step_fit(lo, hi, want, MAX_BINS); // rf_logic.h: same code the host test runs
}

// which STEP_HZ entry best represents a step loaded from settings (largest one <= step)
static uint8_t step_index(uint32_t step) {
    return rfs_step_index(step); // rf_logic.h
}

// which band preset exactly matches a loaded range, else the 779-928 default slot with
// *exact = false. A restored zoomed/custom range has no preset, and showing a band name the
// range does not match would be a lie on the config screen.
static uint16_t band_index(uint32_t lo, uint32_t hi, bool* exact) {
    for(uint16_t i = 0; i < PRESET_COUNT; i++) {
        const FreqBand* b = preset_at(i);
        if(b->lo == lo && b->hi == hi) {
            *exact = true;
            return i;
        }
    }
    *exact = false;
    return 3;
}

static void band_cb(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    const FreqBand* b = preset_at(idx);
    variable_item_set_current_value_text(item, b->name);
    app->f_start = b->lo;
    app->f_end = b->hi;
    // A step finer than the band can hold overflows MAX_BINS, and the worker used to truncate
    // the range silently (f_end rewritten to start + (MAX_BINS-1)*step) while this row still
    // said e.g. "Full spectrum" -- you'd survey 300-351 MHz and believe it was 300-928. Pick
    // the finest step that actually covers the band and push it back into the Step row.
    app->f_step = step_fit(b->lo, b->hi, STEP_HZ[0]); // a band preset wants its finest fit
    uint8_t si = step_index(app->f_step);
    app->parked = false; // a new band invalidates the parked observation
    if(app->it_step) {
        variable_item_set_current_value_index(app->it_step, si);
        variable_item_set_current_value_text(app->it_step, STEP_TXT[si]);
    }
    app->reconfig = true;
    FURI_LOG_I(
        TAG,
        "config: band[%u]=%s %lu-%lu",
        idx,
        b->name,
        (unsigned long)b->lo,
        (unsigned long)b->hi);
    // reflect the chosen band into the range readout
    char r[24];
    snprintf(
        r,
        sizeof(r),
        "%lu-%lu",
        (unsigned long)(b->lo / 1000000),
        (unsigned long)(b->hi / 1000000));
    variable_item_set_current_value_text(app->it_range, r);
    // suggest a matching preset from the band's modulation (AM bands -> AM650, FM -> FM238).
    // Only a default: the user can still override the Modulation item afterwards.
    uint8_t def = (b->mod == ModFM) ? 2 : 0;
    app->preset_idx = def;
    variable_item_set_current_value_index(app->it_preset, def);
    variable_item_set_current_value_text(app->it_preset, PRESETS[def].name);
}

static void preset_cb(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    if(idx >= PRESET_N) idx = PRESET_N - 1;
    app->preset_idx = idx; // worker reloads the CC1101 next loop
    variable_item_set_current_value_text(item, PRESETS[idx].name);
    app->parked = false; // park loaded the old modulation -> must re-enter
    app->reconfig = true; // changed measurement -> clear stale readings
}

static void step_cb(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    if(idx >= COUNT_OF(STEP_HZ)) idx = COUNT_OF(STEP_HZ) - 1;
    uint8_t si;
    // the user's choice is kept unless it cannot fit the current range
    app->f_step = step_fit(app->f_start, app->f_end, STEP_HZ[idx]);
    si = step_index(app->f_step);
    variable_item_set_current_value_index(item, si);
    variable_item_set_current_value_text(item, STEP_TXT[si]);
    app->parked = false;
    app->reconfig = true;
}

// Trigger floor: -100..-45 dBm in 5 dB steps (12 values). Separates weak from strong for
// the waterfall, haptic, history and (later) capture gating.
static void trigger_cb(VariableItem* item) {
    App* app = variable_item_get_context(item);
    app->trigger = (int8_t)(TRIG_MIN + (int)variable_item_get_current_value_index(item) * 5);
    char t[8];
    snprintf(t, sizeof(t), "%d", app->trigger);
    variable_item_set_current_value_text(item, t);
}

static void settle_cb(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    if(idx >= SETTLE_N) idx = SETTLE_N - 1;
    app->settle_ms = SETTLE_CHOICES[idx]; // applies from the next bin, no buffer reset needed
    char t[8];
    snprintf(t, sizeof(t), "%ums", app->settle_ms);
    variable_item_set_current_value_text(item, t);
}

static const char* const PATH_TXT[] = {"Open", "Urban", "Indoor", "Dense"};
static void path_cb(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    if(idx >= COUNT_OF(PATH_TXT)) idx = 1;
    app->path_n = idx;
    variable_item_set_current_value_text(item, PATH_TXT[idx]);
}

static const char* const TRACE_TXT[] = {"Live", "Max hold", "Min hold"};
static void trace_cb(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    if(idx >= COUNT_OF(TRACE_TXT)) idx = 0;
    app->trace = idx; // draw-time only: no reconfig, the buffers are shared by all traces
    variable_item_set_current_value_text(item, TRACE_TXT[idx]);
}

static void bm_cb(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    if(idx == 0 || idx > app->bm_n) {
        variable_item_set_current_value_text(item, "-");
        return;
    }
    const uint32_t lo = app->bm[idx - 1].lo, hi = app->bm[idx - 1].hi;
    app->f_start = lo;
    app->f_end = hi;
    app->f_step = step_fit(lo, hi, STEP_HZ[0]); // same rule as a band preset
    app->parked = false;
    app->reconfig = true;
    variable_item_set_current_value_text(item, app->bm[idx - 1].name);
    if(app->it_step) {
        uint8_t si = step_index(app->f_step);
        variable_item_set_current_value_index(app->it_step, si);
        variable_item_set_current_value_text(app->it_step, STEP_TXT[si]);
    }
    if(app->it_range) {
        char r[24];
        snprintf(
            r,
            sizeof(r),
            "%lu-%lu",
            (unsigned long)(lo / 1000000),
            (unsigned long)(hi / 1000000));
        variable_item_set_current_value_text(app->it_range, r);
    }
    FURI_LOG_I(TAG, "bookmark: %s %lu-%lu", app->bm[idx - 1].name, (unsigned long)lo,
               (unsigned long)hi);
}

static void profile_cb(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    if(idx > PROFILE_N) idx = 0;
    app->profile_slot = idx;
    char txt[8];
    if(!idx) {
        variable_item_set_current_value_text(item, "off");
        return;
    }
    snprintf(txt, sizeof(txt), "%u", (unsigned)idx);
    variable_item_set_current_value_text(item, txt);
    char path[64];
    profile_path(path, sizeof(path), idx);
    Storage* st = furi_record_open(RECORD_STORAGE);
    settings_load(app, st, path); // a missing slot leaves the current values alone
    furi_record_close(RECORD_STORAGE);
    app->f_step = step_fit(app->f_start, app->f_end, app->f_step);
    conf_sync_rows(app);
    app->parked = false;
    app->reconfig = true;
    FURI_LOG_I(TAG, "profile %u loaded", (unsigned)idx);
}

static void haptic_cb(VariableItem* item) {
    App* app = variable_item_get_context(item);
    app->haptic = variable_item_get_current_value_index(item) ? true : false;
    variable_item_set_current_value_text(item, app->haptic ? "On" : "Off");
}

static void gain_cb(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    if(idx >= COUNT_OF(GAIN_TXT)) idx = 0;
    app->gain = idx; // worker reloads the preset with patched AGC next loop
    variable_item_set_current_value_text(item, GAIN_TXT[idx]);
    app->parked = false; // park loaded the old AGC setting -> must re-enter
}

static void radio_cb(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t idx = (RFS_HAS_EXT_RADIO && variable_item_get_current_value_index(item)) ? 1 : 0;
    variable_item_set_current_value_text(item, RADIO_TXT[idx]);
    if(idx == app->radio || !app->worker) {
        app->radio = idx;
        return;
    }
    // restart the sweep worker so the new radio is brought up cleanly (external OTG/begin, or the
    // internal reset). Safe here: in config view the sweep worker is the radio's sole owner.
    app->running = false;
    furi_thread_join(app->worker);
    furi_thread_free(app->worker);
    app->radio = idx;
    app->running = true;
    app->worker = furi_thread_alloc_ex("rfsurvey_sweep", 6144, sweep_worker, app);
    furi_thread_start(app->worker);
    FURI_LOG_I(TAG, "radio: switched to %s (worker restarted)", RADIO_TXT[idx]);
}

// The two config pages share the action bodies but not their indices, so the page number is
// part of the call. index 10 on page 1 and index 8 on page 2 are the switching rows.
static void conf_enter(void* ctx, uint32_t index) {
    App* app = ctx;
    FURI_LOG_D(TAG, "scan row %lu", (unsigned long)index);
    // action rows act in place and stay on the config screen. Both are one-shots consumed by
    // the sweep worker: the radio has exactly one owner, and that owner is not this thread.
    if(index == CONF_ROW_TRIGGER) {
        app->trig_cal = true;
        FURI_LOG_I(TAG, "config: trigger auto-cal queued");
        return;
    }
    if(index == CONF_ROW_TOOLS) { // switch to page 2
        FURI_LOG_I(TAG, "config: -> tools");
        view_dispatcher_switch_to_view(app->vd, VIEW_CONF2);
        return;
    }
    if(index == 1) { // "Range MHz" row -> open the custom range editor
        app->re_start = app->f_start / 100000;
        app->re_end = app->f_end / 100000;
        if(app->re_start < RE_MIN) app->re_start = RE_MIN;
        if(app->re_end > RE_MAX) app->re_end = RE_MAX;
        app->re_cur = 0;
        FURI_LOG_I(TAG, "config: open range editor");
        view_dispatcher_switch_to_view(app->vd, VIEW_RANGE);
        return;
    }
    FURI_LOG_I(TAG, "config: OK -> spectrum");
    view_dispatcher_switch_to_view(app->vd, VIEW_SPEC); // OK on any other row starts the scan
}

// The worker can move the Trigger behind the config screen's back (Auto trig), so the row text
// is re-read whenever the screen is entered instead of only when the value is edited here.
static void conf_view_enter(void* ctx) {
    // NOT `App* app = ctx`: the firmware calls an enter callback with the VIEW's context, and a
    // VariableItemList's view context is the list itself (variable_item_list.c sets it). Reading
    // it_trigger through that pointer read a "pointer" from ~12 KB past a 20-byte heap block and
    // wrote through it on every switch to the config view: launch, Back from the spectrum,
    // return from help/range. That was the v1.3 crash with a different fault every time.
    UNUSED(ctx);
    App* app = g_app;
    if(!app) return;
    if(app->it_trigger) {
        // index AND text: the list picks the next value from the index, so a stale index would
        // make the first Left/Right after an auto-cal jump back to the old floor
        variable_item_set_current_value_index(
            app->it_trigger, (uint8_t)((app->trigger - TRIG_MIN) / 5));
        char t[8];
        snprintf(t, sizeof(t), "%d", app->trigger);
        variable_item_set_current_value_text(app->it_trigger, t);
    }
}

static void redraw_cb(void* ctx) {
    static bool traced;
    App* app = ctx;
    if(!traced) {
        traced = true;
        boot_trace_to("t_timer.txt", "timer fired, seq=%lu", (unsigned long)app->shot_seq);
    }
    // EXPERIMENT: shot_flush(app, false);
    // commit only the view that is actually live + visible, so the timer does no work in the
    // config / range editor (they redraw on input) and never commits the hidden view. The two
    // live views are mutually exclusive: spec_active on the spectrum, capturing on the .sub screen.
    if(app->spec_active) {
        with_view_model(app->view, void** m, { UNUSED(m); }, true);
    } else if(app->capturing && app->cap_view) {
        with_view_model(app->cap_view, void** m, { UNUSED(m); }, true);
    } else if(app->cap_redraw && app->cap_view) {
        app->cap_redraw = false; // one-shot: the frozen "REC" frame otherwise never updates
        with_view_model(app->cap_view, void** m, { UNUSED(m); }, true);
    }
}

// ---- entry -----------------------------------------------------------------

// PORT: ported external apps run only on the ESP32-S3 boards that opt in (T-Embed CC1101,
// Paradoxt, ThorChimera, Thormini). Other boards' pins / power / radio wiring were never checked
// against this app, so refuse to run with a clear message instead of misbehaving.
static bool rfsurvey_board_supported(void) {
    if(furi_hal_board_supports_ported_apps()) return true;
    DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
    DialogMessage* message = dialog_message_alloc();
    dialog_message_set_header(message, "Board not supported", 64, 2, AlignCenter, AlignTop);
    dialog_message_set_text(
        message,
        "Runs on T-Embed, Paradoxt,\nThorChimera and Thormini\nonly.",
        64,
        34,
        AlignCenter,
        AlignCenter);
    dialog_message_set_buttons(message, NULL, "OK", NULL);
    dialog_message_show(dialogs, message);
    dialog_message_free(message);
    furi_record_close(RECORD_DIALOGS);
    return false;
}

int32_t rf_survey_app(void* p) {
    UNUSED(p);
    if(!rfsurvey_board_supported()) return 0;
    // PORT: resolve the input layout once. Encoder boards need the custom-view remap in the input
    // handlers above; D-pad / button boards leave those handlers at their original behaviour.
    s_is_encoder = (furi_hal_board_get_input_layout() == FuriHalBoardInputLayoutEncoder);
    s_enc_mod_seen = false;
    FURI_LOG_I(TAG, "app: start (sizeof App=%u)", (unsigned)sizeof(App));
    App* app = malloc(sizeof(App));
    if(!app) {
        FURI_LOG_E(TAG, "app: malloc failed");
        return -1;
    }
    memset(app, 0, sizeof(App));
    g_app = app;

    // default scan: the whole 779-928 MHz window
    app->f_start = 779000000;
    app->f_end = 928000000;
    app->f_step = 650000;
    app->settle_ms = 3;
    app->preset_idx = 0; // AM650: widest OOK filter, best broadband survey pickup
    app->cursor = NBARS / 2;
    app->win_idx = WIN_DEFAULT; // 60 s
    app->trigger = -85; // default floor: above the CC1101 noise floor, below real signals
    app->haptic = false;
    app->hap_level = -127; // seed the locate filter at the noise floor (0 would false-trigger)
    app->radio =
        radio_external_present() ? 1 : 0; // auto-select External when the module is plugged in
    FURI_LOG_I(TAG, "radio: default %s", app->radio ? "External (detected)" : "Internal");
    {
        Storage* st = furi_record_open(RECORD_STORAGE);
        settings_load(app, st, SETTINGS_PATH); // overrides every default when valid
        bookmarks_load(app, st);
        furi_record_close(RECORD_STORAGE);
    }
    // a saved step may be a zoom-derived value that has no row in STEP_HZ -> snap it so the
    // Step row and the actual dwell width never disagree
    if(app->f_step != 0) app->f_step = STEP_HZ[step_index(app->f_step)];
    // defensive against bad config (future editable ranges): never div-by-zero,
    // never an inverted range, always 1..MAX_BINS bins, and keep f_end consistent
    // with the bins actually scanned so the axis labels can't lie.
    if(app->f_step == 0) app->f_step = 650000;
    if(app->f_end < app->f_start) app->f_end = app->f_start;
    uint32_t n = (app->f_end - app->f_start) / app->f_step + 1;
    if(n > MAX_BINS) n = MAX_BINS;
    if(n < 1) n = 1;
    app->nbins = (uint16_t)n;
    app->f_end = app->f_start + (uint32_t)(app->nbins - 1) * app->f_step;
    // cadence defaults to the middle of the range so the page shows a real frequency before the
    // first park (the user re-targets it from the bars cursor / numeric selection anyway)
    app->park_freq = app->f_start + (uint32_t)(app->nbins / 2) * app->f_step;
    // wf[] is a dBm ring and -128 is "no data". The app struct is zeroed, so without this the
    // waterfall treats every untouched cell as 0 dBm -- i.e. lit. Invisible on a device that
    // fills the ring within seconds; the simulator caught it immediately.
    memset(app->wf, -128, sizeof(app->wf));
    for(uint16_t i = 0; i < app->nbins; i++) {
        app->rssi[i] = -128;
        app->peak[i] = -128;
    }

    app->notif = furi_record_open(RECORD_NOTIFICATION);
    app->gui = furi_record_open(RECORD_GUI);
    app->vd = view_dispatcher_alloc();
#ifdef RFS_LAB
    view_dispatcher_set_event_callback_context(app->vd, app);
    view_dispatcher_set_tick_event_callback(app->vd, lab_tick, 250);
#endif
    view_dispatcher_attach_to_gui(app->vd, app->gui, ViewDispatcherTypeFullscreen);

    // Every allocation below is furi_check'd by the firmware, so NULL means an assert. Fail
    // loudly and leave instead of dereferencing it. (The leak on this path is deliberate: the
    // alternative is a crash on a device with no debugger.)
#define ALLOC_OR_BAIL(p)                                     \
    do {                                                     \
        if(!(p)) {                                           \
            FURI_LOG_E(TAG, "out of memory: " #p);           \
            return -1;                                       \
        }                                                    \
    } while(0)

    boot_trace_to("t_heap0.txt", "free=%u App=%u", (unsigned)memmgr_get_free_heap(), (unsigned)sizeof(App));

    // spectrum view (custom): Back returns to config
    app->view = view_alloc();
    ALLOC_OR_BAIL(app->view);
    view_allocate_model(app->view, ViewModelTypeLockFree, sizeof(void*));
    view_set_context(app->view, app);
    view_set_draw_callback(app->view, spectrum_draw);
    view_set_input_callback(app->view, spectrum_input);
    view_set_previous_callback(app->view, to_conf);
    view_set_enter_callback(app->view, spec_enter);
    view_set_exit_callback(app->view, spec_exit);
    view_dispatcher_add_view(app->vd, VIEW_SPEC, app->view);

    // config view (VariableItemList): pick a band preset (full spectrum / full band /
    // known allocation) -> fills range; also Range readout, Modulation preset and Step. OK scans.
    app->conf = variable_item_list_alloc();
    ALLOC_OR_BAIL(app->conf);
    // PAGE 1 -- what the sweep measures, in the order you would change it while sweeping.
    bool band_exact = false;
    uint16_t bi = band_index(app->f_start, app->f_end, &band_exact);
    VariableItem* vi = variable_item_list_add(app->conf, "Band", PRESET_COUNT, band_cb, app);
    app->it_band = vi;
    variable_item_set_current_value_index(vi, bi);
    variable_item_set_current_value_text(vi, band_exact ? preset_at(bi)->name : "custom");
    app->it_range = variable_item_list_add(app->conf, "Range MHz", 1, NULL, app);
    {
        char r[24];
        snprintf(
            r,
            sizeof(r),
            "%lu-%lu",
            (unsigned long)(app->f_start / 1000000),
            (unsigned long)(app->f_end / 1000000));
        variable_item_set_current_value_text(app->it_range, r);
    }
    app->it_preset = variable_item_list_add(app->conf, "Modulation", PRESET_N, preset_cb, app);
    variable_item_set_current_value_index(app->it_preset, app->preset_idx);
    variable_item_set_current_value_text(app->it_preset, PRESETS[app->preset_idx].name);
    uint8_t si0 = step_index(app->f_step);
    vi = variable_item_list_add(app->conf, "Step", COUNT_OF(STEP_HZ), step_cb, app);
    variable_item_set_current_value_index(vi, si0);
    variable_item_set_current_value_text(vi, STEP_TXT[si0]);
    app->it_step = vi;
    app->it_trigger = variable_item_list_add(app->conf, "Trigger", TRIG_N, trigger_cb, app);
    variable_item_set_current_value_index(
        app->it_trigger, (uint8_t)((app->trigger - TRIG_MIN) / 5));
    {
        char t[8];
        snprintf(t, sizeof(t), "%d", app->trigger);
        variable_item_set_current_value_text(app->it_trigger, t);
    }
    uint8_t se = 0;
    while((uint32_t)se + 1 < SETTLE_N && SETTLE_CHOICES[se + 1] <= app->settle_ms)
        se++;
    app->it_settle = variable_item_list_add(app->conf, "Settle", SETTLE_N, settle_cb, app);
    variable_item_set_current_value_index(app->it_settle, se);
    {
        char t[8];
        snprintf(t, sizeof(t), "%ums", app->settle_ms);
        variable_item_set_current_value_text(app->it_settle, t);
    }
    vi = variable_item_list_add(app->conf, "Gain", COUNT_OF(GAIN_TXT), gain_cb, app);
    variable_item_set_current_value_index(vi, app->gain);
    variable_item_set_current_value_text(vi, GAIN_TXT[app->gain]);
    vi = variable_item_list_add(app->conf, "Radio", RADIO_CHOICES, radio_cb, app);
    variable_item_set_current_value_index(vi, app->radio);
    variable_item_set_current_value_text(vi, RADIO_TXT[app->radio]);
    vi = variable_item_list_add(app->conf, "Path loss", COUNT_OF(PATH_TXT), path_cb, app);
    variable_item_set_current_value_index(vi, app->path_n);
    variable_item_set_current_value_text(vi, PATH_TXT[app->path_n]);
    vi = variable_item_list_add(app->conf, "Trace", COUNT_OF(TRACE_TXT), trace_cb, app);
    variable_item_set_current_value_index(vi, app->trace);
    variable_item_set_current_value_text(vi, TRACE_TXT[app->trace]);
    vi = variable_item_list_add(app->conf, "Tools", 1, NULL, app);
    variable_item_set_current_value_text(vi, ">");
    variable_item_list_set_enter_callback(app->conf, conf_enter, app);
    view_set_enter_callback(variable_item_list_get_view(app->conf), conf_view_enter);
    view_set_previous_callback(variable_item_list_get_view(app->conf), view_exit);
    view_dispatcher_add_view(app->vd, VIEW_CONF, variable_item_list_get_view(app->conf));
    boot_trace_to("t_heap1.txt", "after cfg1 free=%u", (unsigned)memmgr_get_free_heap());

    // PAGE 2 -- actions and exports. Nothing here changes what the sweep measures, so it is a
    // separate page rather than rows 11..18 of the same list.
    app->conf2 = variable_item_list_alloc();
    ALLOC_OR_BAIL(app->conf2);
    vi = variable_item_list_add(app->conf2, "Haptic", 2, haptic_cb, app);
    variable_item_set_current_value_index(vi, app->haptic ? 1 : 0);
    variable_item_set_current_value_text(vi, app->haptic ? "On" : "Off");
    vi = variable_item_list_add(app->conf2, "Clear peaks", 1, NULL, app);
    variable_item_set_current_value_text(vi, "OK");
    vi = variable_item_list_add(app->conf2, "Save waterfall", 1, NULL, app);
    variable_item_set_current_value_text(vi, "OK");
    vi = variable_item_list_add(app->conf2, "Bookmark", app->bm_n + 1, bm_cb, app);
    variable_item_set_current_value_index(vi, 0);
    variable_item_set_current_value_text(vi, "-");
    vi = variable_item_list_add(app->conf2, "Save busy", 1, NULL, app);
    variable_item_set_current_value_text(vi, "OK");
    vi = variable_item_list_add(app->conf2, "Profile", PROFILE_N + 1, profile_cb, app);
    variable_item_set_current_value_index(vi, app->profile_slot);
    variable_item_set_current_value_text(vi, app->profile_slot ? "1" : "off");
    vi = variable_item_list_add(app->conf2, "Save prof.", 1, NULL, app);
    variable_item_set_current_value_text(vi, "OK");
    vi = variable_item_list_add(app->conf2, "Help", 1, NULL, app);
    variable_item_set_current_value_text(vi, "OK");
    vi = variable_item_list_add(app->conf2, "Scan", 1, NULL, app);
    variable_item_set_current_value_text(vi, "<");
#ifdef RFS_LAB
    vi = variable_item_list_add(app->conf2, "TX test", 1, NULL, app); // lab build only
    variable_item_set_current_value_text(vi, "OK");
    vi = variable_item_list_add(app->conf2, "Replay last", 1, NULL, app);
    variable_item_set_current_value_text(vi, "OK");
    vi = variable_item_list_add(app->conf2, "TX frame", 1, NULL, app);
    variable_item_set_current_value_text(vi, "OK");
    app->it_brute = variable_item_list_add(app->conf2, "Code page", 1, NULL, app);
    variable_item_set_current_value_text(app->it_brute, "000000");
#endif
    variable_item_list_set_enter_callback(app->conf2, conf2_enter, app);

    boot_trace_to("t_heap2.txt", "after cfg2 free=%u", (unsigned)memmgr_get_free_heap());
    view_set_previous_callback(variable_item_list_get_view(app->conf2), to_conf);
    view_dispatcher_add_view(app->vd, VIEW_CONF2, variable_item_list_get_view(app->conf2));

    // custom range editor view (Back returns to config)
    app->range_view = view_alloc();
    ALLOC_OR_BAIL(app->range_view);
    view_allocate_model(app->range_view, ViewModelTypeLockFree, sizeof(void*));
    view_set_context(app->range_view, app);
    view_set_draw_callback(app->range_view, range_draw);
    view_set_input_callback(app->range_view, range_input);
    view_set_previous_callback(app->range_view, to_conf);
    view_dispatcher_add_view(app->vd, VIEW_RANGE, app->range_view);

    // .sub capture view (Back is handled in its input callback: stop + restart the survey)
    app->cap_view = view_alloc();
    ALLOC_OR_BAIL(app->cap_view);
    view_allocate_model(app->cap_view, ViewModelTypeLockFree, sizeof(void*));
    view_set_context(app->cap_view, app);
    view_set_draw_callback(app->cap_view, capture_draw);
    view_set_input_callback(app->cap_view, capture_input);
    view_dispatcher_add_view(app->vd, VIEW_CAP, app->cap_view);

    app->help_view = view_alloc();
    ALLOC_OR_BAIL(app->help_view);
    view_allocate_model(app->help_view, ViewModelTypeLockFree, sizeof(void*));
    view_set_context(app->help_view, app);
    view_set_draw_callback(app->help_view, help_draw);
    view_set_input_callback(app->help_view, help_input);
    view_set_previous_callback(app->help_view, to_conf);
    view_dispatcher_add_view(app->vd, VIEW_HELP, app->help_view);

    // install the permissive region for the scan (RX only); restored on exit
    const FuriHalRegion* orig_region = furi_hal_region_get();
    furi_hal_region_set((FuriHalRegion*)&s_region);
    FURI_LOG_I(TAG, "app: region set permissive (was %s)", furi_hal_region_get_name());

    app->running = true;
    // 6 KB stack: the sweep worker calls into the subghz HAL (SPI) and peaks at ~1.6 KB on the
    // device (measured with `top`); the rest is headroom, not a fix for the v1.3 crash.
    app->worker = furi_thread_alloc_ex("rfsurvey_sweep", 6144, sweep_worker, app);
    furi_thread_start(app->worker);

#ifdef RFS_LAB
    app->rep_buf = malloc(sizeof(int32_t) * REP_MAX_EDGES);
#endif
    app->shot_auto = false; // EXPERIMENT: capture path disabled entirely
    boot_trace_to("t_boot.txt", "registered-noshot");
    // EXPERIMENT: registration disabled to see whether it is the call that kills the app
    // gui_add_framebuffer_callback(app->gui, fb_callback, app);
    // app->fb_registered = true; // re-enable together with the line above

    app->redraw = furi_timer_alloc(redraw_cb, FuriTimerTypePeriodic, app);
    furi_timer_start(app->redraw, 150);

    FURI_LOG_I(TAG, "app: views ready, worker+timer up, entering dispatcher");
    view_dispatcher_switch_to_view(app->vd, VIEW_CONF); // land on config
    view_dispatcher_run(app->vd);
    FURI_LOG_I(TAG, "app: dispatcher returned, tearing down");

    // teardown order: stop the redraw timer (no more draw callbacks), then stop and
    // join the worker (no more radio/buffer writes), only then free views and data.
    furi_timer_stop(app->redraw);
    furi_timer_free(app->redraw);
    capture_stop(app); // join a capture thread if one is somehow still running
#ifdef RFS_LAB
    carrier_stop(app);
#endif
    app->running = false;
    if(app->worker) { // NULL only if we somehow exit mid-capture; normally restarted on Back
        furi_thread_join(app->worker);
        furi_thread_free(app->worker);
    }
    furi_hal_region_set((FuriHalRegion*)orig_region); // restore the user's region
    { // persist this session's config for the next launch
        Storage* st = furi_record_open(RECORD_STORAGE);
        settings_save(app, st, SETTINGS_PATH);
        furi_record_close(RECORD_STORAGE);
    }

    view_dispatcher_remove_view(app->vd, VIEW_SPEC);
    view_dispatcher_remove_view(app->vd, VIEW_CONF);
    view_dispatcher_remove_view(app->vd, VIEW_RANGE);
    view_dispatcher_remove_view(app->vd, VIEW_CAP);
    view_dispatcher_remove_view(app->vd, VIEW_HELP);
    view_dispatcher_remove_view(app->vd, VIEW_CONF2);
    view_free(app->view);
    view_free(app->range_view);
    view_free(app->cap_view);
    view_free(app->help_view);
    variable_item_list_free(app->conf);
    variable_item_list_free(app->conf2);
    // canvas_remove_framebuffer_callback() furi_checks that the pair is registered exactly once:
    // removing one that the experiment above never added crashed EVERY exit (furi_check failed)
    if(app->fb_registered) gui_remove_framebuffer_callback(app->gui, fb_callback, app);
    view_dispatcher_free(app->vd);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    g_app = NULL; // defensive: a stray draw after this sees NULL, not freed memory
#ifdef RFS_LAB
    if(app->rep_buf) free(app->rep_buf);
#endif
    free(app);
    FURI_LOG_I(TAG, "app: end");
    return 0;
}
