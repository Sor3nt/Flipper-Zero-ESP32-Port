// rf_logic.h — the parts of RF Survey that are pure state machines and pure arithmetic.
//
// Why this file exists: the three bugs that actually hurt in this project (F13 ring off-by-one,
// F15 synthetic rising edge after an observation hole, F17 percentile rank) all lived in logic
// that needs no radio, no display and no SD card. Keeping them here means the SAME code that
// ships can be compiled on a PC and hammered with adversarial input (tests/logic_test.c)
// instead of being validated by reading it carefully and hoping.
//
// Rules for this file: no furi, no gui, no storage. Float is allowed only where the physics
// needs a logarithm (the distance model); everything else is integer so results are identical
// on the device and on the host.

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h> // log10f/powf: only the distance model needs them

// ------------------------------------------------------------------ sweep geometry

#define RFS_MAX_BINS 1024

static const uint32_t RFS_STEP_HZ[] = {50000, 100000, 200000, 325000, 650000};
#define RFS_STEP_N ((uint32_t)(sizeof(RFS_STEP_HZ) / sizeof(RFS_STEP_HZ[0])))

// Finest step that still fits `max_bins` across [lo,hi], never finer than `want`.
// want = RFS_STEP_HZ[0] means "the finest that fits" (band presets); want = the current step
// means "keep the user's choice, coarsen only if forced".
static inline uint32_t rfs_step_fit(uint32_t lo, uint32_t hi, uint32_t want, uint32_t max_bins) {
    uint32_t span = (hi > lo) ? hi - lo : 0;
    uint32_t i = 0;
    while(i + 1 < RFS_STEP_N && RFS_STEP_HZ[i] < want)
        i++;
    while(i + 1 < RFS_STEP_N && span / RFS_STEP_HZ[i] + 1 > max_bins)
        i++;
    return RFS_STEP_HZ[i];
}

// largest STEP_HZ entry <= step
static inline uint8_t rfs_step_index(uint32_t step) {
    uint8_t i = 0;
    while((uint32_t)i + 1 < RFS_STEP_N && RFS_STEP_HZ[i + 1] <= step)
        i++;
    return i;
}

// Ring index of the k-th waterfall column, oldest first. `head` is the NEWEST written column,
// so the oldest of `cnt` is head-cnt+1. Getting this wrong by one exports an unwritten slot
// when cnt == 1 and rotates the chronology when the ring is full.
static inline uint16_t rfs_wf_col_at(uint16_t head, uint16_t cols, uint16_t cnt, uint16_t k) {
    return (uint16_t)((head + cols + 1u - cnt + k) % cols);
}

// |duration| without the signed overflow of -INT32_MIN. Durations come from a merged capture
// (clamped to 0x3FFFFFFF) so the magnitude is representable, but the helper is what makes that
// a property of the code rather than of the caller's discipline.
static inline uint32_t rfs_edge_mag(int32_t d) {
    return (d < 0) ? (0u - (uint32_t)d) : (uint32_t)d;
}

// ------------------------------------------------------------------ frame estimation

#define RFS_FRAME_EDGES 192

// Estimate the repeating frame from a captured edge train -- URH-style, but without a bit
// synchroniser, because that needs the frame boundary we do not have yet. The trick is that
// in ANY pulse-width encoding ONE BIT costs exactly two edges (a rise and a fall), so bits per
// frame is edges/2 once the frame is delimited. The delimiter is the long silence: `base` is
// the 10th-percentile edge (the short pulse T) and anything above 6T ends a frame.
static inline void rfs_frame_estimate(
    const int32_t* e,
    uint16_t n,
    uint32_t* base_us,
    uint32_t* frame_us,
    uint16_t* bits) {
    *base_us = 0;
    *frame_us = 0;
    *bits = 0;
    if(n < 8 || !e) return;
    if(n > RFS_FRAME_EDGES) n = RFS_FRAME_EDGES;

    uint32_t sorted[RFS_FRAME_EDGES];
    for(uint16_t i = 0; i < n; i++) {
        int32_t d = e[i];
        sorted[i] = rfs_edge_mag(d);
    }
    for(uint16_t i = 1; i < n; i++) { // insertion sort, <= 96 entries
        uint32_t k = sorted[i];
        int j = (int)i - 1;
        while(j >= 0 && sorted[j] > k) {
            sorted[j + 1] = sorted[j];
            j--;
        }
        sorted[j + 1] = k;
    }
    uint32_t base = sorted[n / 10];
    if(base < 50) base = 50; // the ISR drops anything shorter
    *base_us = base;

    const uint64_t gap_thr = (uint64_t)base * 6u;
    uint64_t sum = 0;
    uint16_t edges = 0;
    bool started = false;
    for(uint16_t i = 0; i < n; i++) {
        int32_t d = e[i];
        uint32_t a = rfs_edge_mag(d);
        if((uint64_t)a > gap_thr) {
            if(started) break; // the second silence closes the frame
            continue; // still inside a partial first frame
        }
        started = true;
        sum += a;
        edges++;
    }
    if(!started || edges < 4) return;
    // a frame longer than the output type cannot be reported honestly, so it is clamped rather
    // than wrapped into a small number that looks like a measurement
    *frame_us = (sum > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)sum;
    *bits = (uint16_t)(edges / 2);
}

// ------------------------------------------------------------------ distance estimate

typedef struct {
    uint32_t m; // point estimate, metres
    uint32_t lo; // with the RSSI uncertainty applied
    uint32_t hi;
    bool valid;
} RfsDistance;

// Log-distance path loss, inverted. Honest about its own error: the interval comes from the
// RSSI uncertainty (CC1101 is roughly +-6 dB, and that alone is a factor of 1.7 at n=2.7),
// so the answer is a band, never a number. It also cannot see antenna orientation, body
// shadowing, or the difference between line of sight and two walls -- treat it as "near" or
// "far", not as a measurement.
//
//   PL(d) = 20*log10(d) + 20*log10(f) - 147.55     (d in m, f in Hz)
//   P_rx  = p_tx + gain - PL(d) - 10*(n/10)*log10(d)
static inline RfsDistance rfs_estimate_distance(
    int8_t rssi_dbm,
    uint32_t freq_hz,
    int8_t p_tx_dbm,
    int8_t gain_db,
    uint8_t n10, // path loss exponent x10: 20 free space, 27 urban, 35 indoor, 45 dense
    uint8_t margin_db) {
    RfsDistance r = {0, 0, 0, false};
    if(freq_hz < 100000000u || freq_hz > 1000000000u || n10 < 15 || n10 > 60) return r;

    float pl1m = 20.0f * log10f((float)freq_hz) - 147.55f; // path loss at 1 m
    float budget = (float)p_tx_dbm + (float)gain_db - (float)rssi_dbm - pl1m;
    float n = (float)n10 / 10.0f;
    if(budget <= 0.0f) { // closer than 1 m: the model has nothing useful to say
        r.valid = true;
        r.m = 1;
        r.lo = 1;
        r.hi = 1;
        return r;
    }
    // clamp in FLOAT: converting an out-of-range (or infinite) float to uint32 is undefined,
    // and the exponent can produce one from perfectly valid-looking inputs
#define RFS_DIST_CLAMP(x)                                       \
    (((x) != (x) || (x) < 1.0f) ? 1u : ((x) > 99999.0f ? 99999u : (uint32_t)(x)))
    r.m = RFS_DIST_CLAMP(powf(10.0f, budget / (10.0f * n)));
    r.lo = RFS_DIST_CLAMP(powf(10.0f, (budget - (float)margin_db) / (10.0f * n)));
    r.hi = RFS_DIST_CLAMP(powf(10.0f, (budget + (float)margin_db) / (10.0f * n)));
#undef RFS_DIST_CLAMP
    r.valid = true;
    return r;
}

// ------------------------------------------------------------------ period clustering

// How many distinct periodic sources are on this frequency? One sensor is the common case, but
// two devices a few kHz apart (or the same model with different jumper settings) show up as a
// BIMODAL period series, and averaging them produces a period that belongs to neither.
// Single-linkage on the sorted periods: a new cluster starts when the gap exceeds a quarter of
// the running mean. Clusters with fewer than 2 members are noise and are dropped.
static inline uint8_t
    rfs_period_clusters(const uint16_t* per, uint8_t n, uint16_t* center, uint8_t max) {
    if(!per || n < 4 || max == 0) return 0;
    uint16_t v[32];
    if(n > 32) n = 32;
    for(uint8_t i = 0; i < n; i++)
        v[i] = per[i];
    for(uint8_t i = 1; i < n; i++) { // insertion sort
        uint16_t k = v[i];
        int j = (int)i - 1;
        while(j >= 0 && v[j] > k) {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = k;
    }

    uint8_t nc = 0;
    uint32_t sum = v[0];
    uint16_t cnt = 1;
    uint16_t start = 0;
    for(uint8_t i = 1; i <= n; i++) {
        bool split = (i == n);
        if(!split) {
            uint32_t mean = sum / cnt;
            uint32_t gap_min = (mean / 4) ? (mean / 4) : 1u;
            split = (uint32_t)(v[i] - v[i - 1]) > gap_min;
        }
        if(split) {
            if(cnt >= 2 && nc < max) center[nc++] = (uint16_t)(sum / cnt);
            if(i == n) break;
            sum = 0;
            cnt = 0;
            start = i;
        }
        sum += v[i];
        cnt++;
        (void)start;
    }
    return nc;
}

// ------------------------------------------------------------------ Princeton frame encoder

#define RFS_PRINCETON_BITS 24
#define RFS_PRINCETON_EDGES ((RFS_PRINCETON_BITS * 2) + 2)

// Encode a PT2262-style frame: every bit is 4T, a "0" is 1T high + 3T low and a "1" is 3T high
// + 1T low, then a 1T/31T sync closes the frame. Pure arithmetic, so the shape can be checked
// on a PC before any of it is allowed near a transmitter.
static inline uint16_t
    rfs_encode_princeton(uint32_t code, uint32_t t_us, int32_t* out, uint16_t max) {
    if(!out || max < RFS_PRINCETON_EDGES || t_us < 50 || t_us > 2000) return 0;
    uint16_t n = 0;
    for(int i = RFS_PRINCETON_BITS - 1; i >= 0; i--) {
        bool one = ((code >> i) & 1u) != 0;
        out[n++] = (int32_t)(one ? 3u * t_us : t_us);
        out[n++] = (int32_t)-(one ? t_us : 3u * t_us);
    }
    out[n++] = (int32_t)t_us; // sync high
    out[n++] = -(int32_t)(31u * t_us); // sync low
    return n;
}

// ------------------------------------------------------------------ preamble / sync

typedef struct {
    uint16_t count; // longest run of base-width pulses (the preamble)
    uint32_t sync_us; // the long silence that closes that run (0 = the run had no gap after it)
} RfsPreamble;

// A preamble is a run of identical short pulses; the sync is the silence after it. Its LENGTH is
// a device fingerprint (KeeLoq 23, many OEM remotes 8 or 12) and it is also where a frame
// really begins -- "any gap longer than 6T" only approximates that.
static inline RfsPreamble rfs_preamble(const int32_t* e, uint16_t n, uint32_t base_us) {
    RfsPreamble r = {0, 0};
    if(!e || n < 8 || base_us < 50) return r;
    const uint32_t tol = base_us / 3; // +-33%: cheap encoders drift
    const uint32_t lo = (base_us > tol) ? base_us - tol : 1;
    const uint32_t hi = base_us + tol;
    const uint32_t gap = base_us * 6;

    uint16_t run = 0, best = 0;
    uint32_t sync = 0;
    for(uint16_t i = 0; i < n; i++) {
        int32_t d = e[i];
        uint32_t a = rfs_edge_mag(d);
        if(a >= lo && a <= hi) {
            run++;
            continue;
        }
        if(run > best) { // a run ends here: remember it and whether a silence closed it
            best = run;
            sync = (a > gap) ? a : 0;
        }
        run = 0;
    }
    if(run > best) {
        best = run;
        sync = 0;
    }
    r.count = best;
    r.sync_us = sync;
    return r;
}

// ------------------------------------------------------------------ fixed vs rolling code

#define RFS_CODE_UNKNOWN  0
#define RFS_CODE_STATIC   1 // two frames are identical -> a fixed code, replayable
#define RFS_CODE_CHANGING 2 // they differ -> a counter or a cipher, replay will not work

// Compare two consecutive frames of a capture. This is not an attack and not cryptanalysis: it
// is the one observation that decides whether replay is even possible. A fixed-code remote puts
// the same bits on the air every press; a rolling-code one does not. Quantising the durations by
// base/4 keeps the comparison robust to the ±5% timing jitter of cheap encoders.
//
// The first run in a capture is a partial frame (we joined mid-frame), so it is discarded and
// runs 2 and 3 are compared.
static inline uint8_t rfs_code_kind(const int32_t* e, uint16_t n, uint32_t base_us) {
    if(!e || n < 24 || base_us < 50) return RFS_CODE_UNKNOWN;
    const uint32_t gap = base_us * 6;
    const uint32_t q = (base_us / 4) ? (base_us / 4) : 1;

    // Collect up to three complete runs. Two frames are compared at a time; keeping three means
    // a capture that started mid-frame still has one clean pair (the partial run differs in edge
    // count, so the pair is skipped instead of being read as "rolling").
    uint32_t h[3];
    uint16_t c[3];
    uint8_t nr = 0;
    uint32_t acc = 2166136261u; // FNV-1a offset basis, restarted per run
    uint16_t edges = 0;

    for(uint16_t i = 0; i <= n && nr < 3; i++) {
        bool end = (i == n);
        uint32_t a = 0;
        if(!end) {
            int32_t d = e[i];
            a = rfs_edge_mag(d);
            if(a > gap) end = true;
        }
        if(end) {
            if(edges >= 8) { // shorter runs are a leading gap or a stray pair, not a frame
                h[nr] = acc;
                c[nr] = edges;
                nr++;
            }
            acc = 2166136261u;
            edges = 0;
            continue;
        }
        acc ^= (a / q); // quantised edge length, robust to the +-5% jitter of cheap encoders
        acc *= 16777619u;
        edges++;
    }

    if(nr < 2) return RFS_CODE_UNKNOWN;
    bool comparable = false;
    for(uint8_t i = 0; i + 1 < nr; i++) {
        if(c[i] != c[i + 1]) continue; // different lengths cannot be the same code
        comparable = true;
        if(h[i] == h[i + 1]) return RFS_CODE_STATIC;
    }
    return comparable ? RFS_CODE_CHANGING : RFS_CODE_UNKNOWN;
}

// ------------------------------------------------------------------ parked burst machine

#define RFS_PARK_RING      16
// The cadence loop is "sample, maybe write SD, sleep 8 ms": 8 ms is a FLOOR, not a clock.
// Anything above this is an instrumentation hole -- bursts can be missed inside it and any
// interval or duration that spans it is not a measurement.
#define RFS_PARK_GAP_MS    40u
#define RFS_PARK_REP_GAP_MS 250u // rising edges closer than this belong to the same press
#define RFS_PARK_MAX_PERIOD_MS 600000u // 10 min: longer than any plausible sensor, shorter than a wrap

typedef struct {
    // burst segmentation
    bool in_burst;
    bool gap_pending; // the current inter-edge interval contains a hole -> no period
    bool suppress; // after a hole: ignore bursts until the carrier is seen low again
    bool have_last, have_prev, have_rise; // explicit "seen one" flags: tick 0 is a valid tick
    uint32_t last_t; // previous sample tick (gap detection)
    uint32_t gap_t; // tick of the most recent hole (0 = none)
    uint32_t prev_start; // tick of the previous rising edge (0 = none yet)
    uint32_t burst_start;
    uint32_t last_rise; // repeat-group continuity
    uint16_t bursts;
    uint16_t gaps;
    uint16_t rep; // current group size
    uint16_t rep_last; // last completed group size
    uint32_t n_samples;
    uint32_t n_above;
    uint16_t per[RFS_PARK_RING]; // last N repetition periods, ms
    uint8_t per_n, per_i;
    uint16_t dur[RFS_PARK_RING]; // last N burst durations, ms
    uint8_t dur_n, dur_i;
} RfsPark;

static inline void rfs_park_clear(RfsPark* s) {
    memset(s, 0, sizeof(*s));
}

static inline void rfs_park_push(uint16_t* ring, uint8_t* n, uint8_t* i, uint32_t v) {
    ring[*i] = (uint16_t)(v > 0xFFFFu ? 0xFFFFu : v);
    *i = (uint8_t)((*i + 1u) % RFS_PARK_RING);
    if(*n < RFS_PARK_RING) (*n)++;
}

// Feed one RSSI sample. Pure: no clock, no radio, no globals -- `now` is the caller's tick.
static inline void rfs_park_feed(RfsPark* s, uint32_t now, int8_t rssi, int8_t trigger) {
    if(s->have_last) {
        uint32_t dt = now - s->last_t;
        if(dt > RFS_PARK_GAP_MS) {
            s->gaps++;
            s->gap_t = now;
            s->gap_pending = true; // the interval that contains this hole is void
            // A hole can swallow the end of a burst AND the start of the next, so a still-high
            // sample right after it is not a rise -- it is the burst we were already inside.
            // Ignore everything until the carrier is observed low, and break the repeat group.
            s->suppress = true;
            s->in_burst = false;
            s->have_rise = false;
        }
    }
    s->last_t = now;
    s->have_last = true;
    s->n_samples++;

    if(rssi > trigger) s->n_above++;

    if(s->suppress) {
        if(rssi <= trigger) { // back to a known state: the next rise is a real one
            s->suppress = false;
            s->have_prev = false; // no baseline -> that rise yields no period
            s->rep = 0;
        }
        return;
    }

    if(rssi > trigger) {
        if(!s->in_burst) { // rising edge
            s->in_burst = true;
            s->burst_start = now;
            s->bursts++;
            if(s->have_rise && now - s->last_rise < RFS_PARK_REP_GAP_MS)
                s->rep++;
            else
                s->rep = 1;
            s->last_rise = now;
            s->have_rise = true;
            if(s->gap_pending) {
                s->gap_pending = false; // this interval crossed an unobserved hole
            } else if(s->have_prev) {
                // unsigned subtraction, not an ordering compare: the tick wraps every 49 days
                // and `now > prev_start` silently dropped the one interval that spanned it
                uint32_t elapsed = now - s->prev_start;
                if(elapsed > 0 && elapsed <= RFS_PARK_MAX_PERIOD_MS)
                    rfs_park_push(s->per, &s->per_n, &s->per_i, elapsed);
            }
            s->prev_start = now;
            s->have_prev = true;
        }
    } else if(s->in_burst) { // falling edge
        s->in_burst = false;
        s->rep_last = s->rep;
        rfs_park_push(s->dur, &s->dur_n, &s->dur_i, now - s->burst_start);
    }
}

// Stopping while a burst is open: we never saw the falling edge, so the elapsed time is a
// lower bound, not a measurement. Drop it rather than bias the length statistics.
static inline void rfs_park_stop(RfsPark* s) {
    s->in_burst = false;
}

// mean period, standard deviation and coefficient of variation over the ring
static inline void rfs_park_stats(
    const RfsPark* s,
    uint32_t* mean_ms,
    uint32_t* sd_ms,
    uint32_t* cv_milli) {
    *mean_ms = 0;
    *sd_ms = 0;
    *cv_milli = 0;
    uint8_t n = s->per_n;
    if(n < 2) return;
    uint64_t sum = 0, sq = 0;
    for(uint8_t k = 0; k < n; k++) {
        uint32_t v = s->per[k];
        sum += v;
        sq += (uint64_t)v * v;
    }
    uint32_t mean = (uint32_t)(sum / n);
    // var = E[x^2] - mean^2, clamped; integer sqrt via Newton so no libm in the firmware
    int64_t var = (int64_t)(sq / n) - (int64_t)mean * mean;
    if(var < 0) var = 0;
    uint64_t x = (uint64_t)var, r = x;
    if(x) {
        for(int i = 0; i < 24; i++) r = (r + x / r) / 2; // converges in a few rounds
    } else {
        r = 0;
    }
    *mean_ms = mean;
    *sd_ms = (uint32_t)r;
    *cv_milli = mean ? (uint32_t)((r * 1000u) / mean) : 0;
}
