#include "beacon.h"

#include <furi.h>

#include "idlog.h"

#define TAG            "Beacon"
#define BEACON_TICK_MS 20

/* Wrap-safe deadline test. furi_get_tick() rolls over about every 50 days;
 * comparing the signed difference keeps working across the seam. */
static inline bool tick_reached(uint32_t now, uint32_t target) {
    return (int32_t)(now - target) >= 0;
}

struct Beacon {
    CwRadio* radio;
    FuriThread* thread;
    volatile bool running;
    volatile bool manual_id;
    MorseConfig config;
    BeaconStatus status;
    FuriMutex* mutex;
};

static void beacon_params(const MorseConfig* config, CwRadioParams* params) {
    params->frequency = config->frequency;
    params->mode = config->mode;
    params->deviation = config->deviation;
    params->rx_bw = config->rx_bw;
    params->tone_hz = config->tone_hz;
}

static void beacon_send_id(Beacon* beacon) {
    MorseStream* stream = morse_stream_alloc(
        beacon->config.id_text,
        beacon->config.wpm,
        beacon->config.farnsworth,
        (beacon->config.mode == CwModeMcw) ? beacon->config.preamble_ms : 0,
        (beacon->config.mode == CwModeMcw) ? beacon->config.tail_ms : 0);
    if(!stream) {
        morse_log("id: nothing sendable in \"%s\"", beacon->config.id_text);
        return;
    }
    morse_log("id: keying \"%s\"", beacon->config.id_text);

    CwRadioParams params;
    beacon_params(&beacon->config, &params);

    furi_mutex_acquire(beacon->mutex, FuriWaitForever);
    beacon->status.state = BeaconStateSending;
    furi_mutex_release(beacon->mutex);

    bool ok = cw_radio_send(beacon->radio, stream, &params);
    morse_stream_free(stream);

    furi_mutex_acquire(beacon->mutex, FuriWaitForever);
    if(ok) {
        beacon->status.id_count++;
        beacon->status.since_id_ms = 0;
    } else {
        FURI_LOG_E(TAG, "transmit failed");
    }
    beacon->status.quiet_ms = 0;
    beacon->status.progress = 0;
    furi_mutex_release(beacon->mutex);

    cw_radio_listen(beacon->radio, &params);
}

static int32_t beacon_worker(void* context) {
    Beacon* beacon = context;
    CwRadioParams params;
    beacon_params(&beacon->config, &params);

    const uint32_t quiet_arm_ms = (uint32_t)beacon->config.quiet_time_s * 1000;
    const uint32_t max_interval_ms = (uint32_t)beacon->config.max_interval_s * 1000;
    const uint32_t period_ms = (uint32_t)beacon->config.period_s * 1000;
    const bool use_interval = beacon->config.beacon_trigger == BeaconTriggerInterval ||
                              beacon->config.beacon_trigger == BeaconTriggerBoth;
    const bool use_activity = beacon->config.beacon_trigger == BeaconTriggerActivity ||
                              beacon->config.beacon_trigger == BeaconTriggerBoth;

    const bool anchor_start = beacon->config.interval_anchor == IntervalAnchorStart;

    morse_log(
        "beacon: worker start, %lu Hz, trigger=%s%s",
        beacon->config.frequency,
        use_interval ? (use_activity ? "both" : "interval") : "activity",
        use_interval ? (anchor_start ? ", start-to-start" : ", end-to-start") : "");
    if(!cw_radio_listen(beacon->radio, &params)) {
        furi_mutex_acquire(beacon->mutex, FuriWaitForever);
        beacon->status.state = BeaconStateError;
        furi_mutex_release(beacon->mutex);
        beacon->running = false;
        return 0;
    }


    uint32_t carrier_ms = 0;
    uint32_t clear_ms = 0;
    uint32_t pending_ms = 0;
    uint32_t quiet_before = 0;

    furi_mutex_acquire(beacon->mutex, FuriWaitForever);
    beacon->status.state = BeaconStateListening;
    // A cold start counts as dead air, so the first traffic we hear gets an ID.
    beacon->status.quiet_ms = quiet_arm_ms;
    beacon->status.since_id_ms = 0;
    furi_mutex_release(beacon->mutex);

    /* Interval mode runs off an absolute deadline rather than a countdown, so
     * neither the length of a transmission nor the drift of this loop can walk
     * the schedule. Every timer below likewise measures real elapsed time:
     * furi_delay_ms(20) sleeps *at least* 20 ms and the loop body costs more on
     * top, so a tick accumulator runs slow. */
    const uint32_t tick_hz = furi_kernel_get_tick_frequency();
    const uint32_t period_ticks = (uint32_t)((uint64_t)period_ms * tick_hz / 1000);
    uint32_t next_id_tick = furi_get_tick() + period_ticks;

    if(beacon->config.id_on_start) {
        uint32_t tx_start = furi_get_tick();
        beacon_send_id(beacon);
        next_id_tick = (anchor_start ? tx_start : furi_get_tick()) + period_ticks;
        furi_mutex_acquire(beacon->mutex, FuriWaitForever);
        beacon->status.state = BeaconStateListening;
        furi_mutex_release(beacon->mutex);
    }

    uint32_t last_tick = furi_get_tick();

    while(beacon->running) {
        furi_delay_ms(BEACON_TICK_MS);

        uint32_t now_tick = furi_get_tick();
        uint32_t dt = (uint32_t)((uint64_t)(now_tick - last_tick) * 1000 / tick_hz);
        last_tick = now_tick;

        float rssi = cw_radio_rssi(beacon->radio);
        bool carrier = rssi > (float)beacon->config.squelch_dbm;

        furi_mutex_acquire(beacon->mutex, FuriWaitForever);
        beacon->status.rssi = rssi;
        beacon->status.carrier = carrier;
        beacon->status.since_id_ms += dt;
        BeaconState state = beacon->status.state;
        furi_mutex_release(beacon->mutex);

        if(beacon->manual_id) {
            beacon->manual_id = false;
            beacon_send_id(beacon);
            // A manual ID restarts a gap, but never shifts the grid - the whole
            // point of start-to-start is that the slots do not move.
            if(!anchor_start) next_id_tick = furi_get_tick() + period_ticks;
            last_tick = furi_get_tick();
            carrier_ms = clear_ms = pending_ms = 0;
            furi_mutex_acquire(beacon->mutex, FuriWaitForever);
            beacon->status.state = BeaconStateListening;
            furi_mutex_release(beacon->mutex);
            continue;
        }

        if(use_interval && tick_reached(now_tick, next_id_tick)) {
            // Interval mode keys on schedule whether or not the channel is busy;
            // a fox has to be predictable to be huntable.
            beacon_send_id(beacon);

            if(anchor_start) {
                /* Advance the grid, not the clock. Slots stay at fixed multiples
                 * of the period from the first one, so the cadence holds even
                 * though each transmission takes seconds. */
                next_id_tick += period_ticks;
                uint32_t skipped = 0;
                while(tick_reached(furi_get_tick(), next_id_tick)) {
                    next_id_tick += period_ticks;
                    skipped++;
                }
                if(skipped) {
                    // Honouring start-to-start here would mean keying without a
                    // break. Drop whole slots instead and say so.
                    morse_log(
                        "beacon: ID longer than the interval, skipped %lu slot(s)",
                        skipped);
                }
            } else {
                next_id_tick = furi_get_tick() + period_ticks;
            }

            last_tick = furi_get_tick();
            carrier_ms = clear_ms = pending_ms = 0;
            furi_mutex_acquire(beacon->mutex, FuriWaitForever);
            beacon->status.state = BeaconStateListening;
            furi_mutex_release(beacon->mutex);
            continue;
        }

        switch(state) {
        case BeaconStateListening:
            if(carrier) {
                carrier_ms += dt;
                if(carrier_ms >= beacon->config.min_carrier_ms) {
                    furi_mutex_acquire(beacon->mutex, FuriWaitForever);
                    quiet_before = beacon->status.quiet_ms;
                    beacon->status.traffic_count++;
                    beacon->status.state = BeaconStateBusy;
                    furi_mutex_release(beacon->mutex);
                    clear_ms = 0;
                }
            } else {
                carrier_ms = 0;
                furi_mutex_acquire(beacon->mutex, FuriWaitForever);
                beacon->status.quiet_ms += dt;
                furi_mutex_release(beacon->mutex);
            }
            break;

        case BeaconStateBusy:
            if(carrier) {
                clear_ms = 0;
            } else {
                clear_ms += dt;
                if(clear_ms >= beacon->config.hang_ms) {
                    furi_mutex_acquire(beacon->mutex, FuriWaitForever);
                    bool trigger = use_activity &&
                                   ((quiet_before >= quiet_arm_ms) ||
                                    (max_interval_ms &&
                                     beacon->status.since_id_ms >= max_interval_ms));
                    if(trigger) {
                        beacon->status.state = BeaconStatePending;
                        pending_ms = clear_ms;
                    } else {
                        beacon->status.state = BeaconStateListening;
                        beacon->status.quiet_ms = clear_ms;
                    }
                    furi_mutex_release(beacon->mutex);
                    carrier_ms = 0;
                }
            }
            break;

        case BeaconStatePending:
            if(carrier) {
                carrier_ms += dt;
                if(carrier_ms >= beacon->config.min_carrier_ms) {
                    // traffic came back before we could ID - wait it out and retry
                    furi_mutex_acquire(beacon->mutex, FuriWaitForever);
                    beacon->status.state = BeaconStateBusy;
                    furi_mutex_release(beacon->mutex);
                    clear_ms = 0;
                }
            } else {
                carrier_ms = 0;
                pending_ms += dt;
                if(pending_ms >= beacon->config.courtesy_delay_ms) {
                    beacon_send_id(beacon);
                    if(!anchor_start) next_id_tick = furi_get_tick() + period_ticks;
                    last_tick = furi_get_tick();
                    furi_mutex_acquire(beacon->mutex, FuriWaitForever);
                    beacon->status.state = BeaconStateListening;
                    furi_mutex_release(beacon->mutex);
                    pending_ms = 0;
                }
            }
            break;

        default:
            break;
        }

        furi_mutex_acquire(beacon->mutex, FuriWaitForever);
        beacon->status.armed =
            use_activity && ((beacon->status.quiet_ms >= quiet_arm_ms) ||
                             (max_interval_ms &&
                              beacon->status.since_id_ms >= max_interval_ms));
        int32_t remain_ticks = (int32_t)(next_id_tick - now_tick);
        beacon->status.next_id_ms =
            (use_interval && remain_ticks > 0) ?
                (uint32_t)((uint64_t)remain_ticks * 1000 / tick_hz) :
                0;
        furi_mutex_release(beacon->mutex);
    }

    cw_radio_idle(beacon->radio);

    furi_mutex_acquire(beacon->mutex, FuriWaitForever);
    beacon->status.state = BeaconStateStopped;
    furi_mutex_release(beacon->mutex);
    return 0;
}

Beacon* beacon_alloc(CwRadio* radio) {
    Beacon* beacon = malloc(sizeof(Beacon));
    memset(beacon, 0, sizeof(Beacon));
    beacon->radio = radio;
    beacon->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    beacon->status.state = BeaconStateStopped;
    return beacon;
}

void beacon_free(Beacon* beacon) {
    furi_check(beacon);
    beacon_stop(beacon);
    furi_mutex_free(beacon->mutex);
    free(beacon);
}

void beacon_start(Beacon* beacon, const MorseConfig* config) {
    furi_check(beacon);
    if(beacon->running) return;

    beacon->config = *config;
    memset(&beacon->status, 0, sizeof(BeaconStatus));
    beacon->status.state = BeaconStateListening;
    beacon->manual_id = false;
    beacon->running = true;

    beacon->thread = furi_thread_alloc_ex("BeaconWorker", 4096, beacon_worker, beacon);
    furi_thread_start(beacon->thread);
}

void beacon_stop(Beacon* beacon) {
    furi_check(beacon);
    if(!beacon->running) {
        if(beacon->thread) {
            furi_thread_join(beacon->thread);
            furi_thread_free(beacon->thread);
            beacon->thread = NULL;
        }
        return;
    }
    beacon->running = false;
    cw_radio_abort(beacon->radio);
    furi_thread_join(beacon->thread);
    furi_thread_free(beacon->thread);
    beacon->thread = NULL;
}

bool beacon_is_running(Beacon* beacon) {
    return beacon && beacon->running;
}

void beacon_request_id(Beacon* beacon) {
    if(beacon && beacon->running) beacon->manual_id = true;
}

void beacon_get_status(Beacon* beacon, BeaconStatus* out) {
    furi_check(beacon);
    furi_mutex_acquire(beacon->mutex, FuriWaitForever);
    *out = beacon->status;
    furi_mutex_release(beacon->mutex);
    out->progress = cw_radio_progress(beacon->radio);
}
