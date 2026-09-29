#pragma once
#include "bw16_control.h"

/* The selected operation has one 30-second budget. The caller must initialize this to zero. */
typedef struct {
    bool running, cancel_requested, accounted, native;
    unsigned targets[BW16_MAX_APS], count, position, completed;
    uint32_t started, dwell_ms, total_counter;
    bool active_seen, stop_requested;
    uint32_t active_started;
} Bw16Batch;

/* Validate the complete selection before sending any command. */
bool bw16_batch_start(
    Bw16Batch* batch, Bw16Control* control, const unsigned* indices,
    unsigned count, uint32_t now);
/* Call after bw16_control_tick. uart_quiet is retained for source compatibility. */
void bw16_batch_tick(Bw16Batch* batch, Bw16Control* control, uint32_t now, bool uart_quiet);
/* Cancel the entire batch; the existing controller still verifies STOP acknowledgement. */
void bw16_batch_stop(Bw16Batch* batch, Bw16Control* control, uint32_t now);
/* Device-reported total attempts; never multiplied by the selected target count. */
uint32_t bw16_batch_counter(const Bw16Batch* batch, const Bw16Control* control);
