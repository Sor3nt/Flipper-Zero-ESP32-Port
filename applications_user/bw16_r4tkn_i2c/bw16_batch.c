#include "bw16_batch.h"
#include <string.h>

uint32_t bw16_batch_counter(const Bw16Batch* b, const Bw16Control* c) {
    if(!b || !c || !b->count) return 0;
    return b->accounted ? b->total_counter : c->counter;
}

static void finish(Bw16Batch* b, const Bw16Control* c, bool acknowledged) {
    if(!b->accounted) {
        b->total_counter = c->counter;
        b->accounted = true;
        if(acknowledged && b->active_seen) b->completed = 1;
    }
    b->running = false;
}

bool bw16_batch_start(
    Bw16Batch* b, Bw16Control* c, const unsigned* indices,
    unsigned count, uint32_t now) {
    if(!b || !c || !indices || b->running || !count || count > BW16_MAX_APS ||
       !c->aps_valid || !c->indices_valid || !c->ap_count || c->ap_count > BW16_MAX_APS ||
       c->restart_required || c->remote_unknown ||
       (c->state != Bw16Idle && c->state != Bw16Stopped) ||
       (count > 1 && !c->native_selected)) return false;
    /* A bad final index must not dispatch any operation. */
    for(unsigned i = 0; i < count; ++i) {
        if(indices[i] >= c->ap_count || !c->aps[indices[i]].mac[0]) return false;
        for(unsigned j = 0; j < i; ++j) if(indices[j] == indices[i]) return false;
    }
    unsigned selected[BW16_MAX_APS];
    memcpy(selected, indices, count * sizeof(*indices));
    memset(b, 0, sizeof(*b));
    memcpy(b->targets, selected, count * sizeof(*indices));
    b->count = count;
    b->started = now;
    b->dwell_ms = BW16_OPERATION_MS;
    b->native = count > 1;
    b->running = b->native ? bw16_control_start_selected(c, selected, count, now) :
        bw16_control_start(c, Bw16TargetStation, selected[0], 0, now);
    if(!b->running) {
        b->cancel_requested = true;
        finish(b, c, false);
    }
    return b->running;
}

void bw16_batch_stop(Bw16Batch* b, Bw16Control* c, uint32_t now) {
    if(!b || !c || !b->running) return;
    b->cancel_requested = true;
    if(c->state == Bw16Starting || c->state == Bw16Active) {
        if(c->state == Bw16Active) b->active_seen = true;
        b->stop_requested = true;
        bw16_control_stop(c, now);
    }
    if(c->state == Bw16Stopping) b->stop_requested = true;
    else finish(b, c, c->state == Bw16Stopped && !c->remote_unknown);
}

void bw16_batch_tick(Bw16Batch* b, Bw16Control* c, uint32_t now, bool uart_quiet) {
    (void)uart_quiet;
    if(!b || !c || !b->running) return;
    if(c->state == Bw16Fault) {
        b->cancel_requested = true;
        finish(b, c, false);
        return;
    }
    if(c->start_confirmed) b->active_seen = true;
    if(c->state == Bw16Stopped && !c->remote_unknown) {
        if(!b->active_seen && !b->cancel_requested) {
            bw16_control_error(c, "Unconfirmed batch target start", now);
            b->cancel_requested = true;
            finish(b, c, false);
        } else finish(b, c, true);
        return;
    }
    if(c->restart_required || (uint32_t)(now - b->started) >= BW16_OPERATION_MS) {
        bw16_batch_stop(b, c, now);
        return;
    }
    if(c->state == Bw16Starting) return;
    if(c->state == Bw16Active) {
        if(!b->active_seen) {
            b->active_seen = true;
            b->active_started = now;
        }
        if(b->native && c->selected_ap_valid) {
            for(unsigned i = 0; i < b->count; ++i)
                if(b->targets[i] == c->selected_ap) { b->position = i; break; }
        }
        if(b->cancel_requested) bw16_batch_stop(b, c, now);
        return;
    }
    if(c->state == Bw16Stopping) {
        b->stop_requested = true;
        return;
    }
    bw16_control_error(c, "Batch state changed; reset BW16", now);
    b->cancel_requested = true;
    finish(b, c, false);
}
