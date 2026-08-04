/*
 * IR transmitter — ESP32 port.
 *
 * Original used TIM1 CH3N for carrier PWM and DWT->CYCCNT for symbol timing.
 * ESP32 port uses furi_hal_infrared_async_tx (RMT-backed) with a callback
 * producing level/duration pairs for PP4 symbols.
 *
 * NOTE: carrier is capped at INFRARED_MAX_FREQUENCY (1 MHz on ESP32-S3).
 */

#include "tagtinker_ir.h"

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_infrared.h>

#define TAGTINKER_IR_TAG "TagTinkerIR"

#define CARRIER_FREQ_HZ 1000000UL /* original ~1.23 MHz, clamped to HAL max */
#define CARRIER_DUTY    0.5f

#define PP4_BURST_US 40UL
static const uint32_t pp4_gap_us[4] = {
    60,  /* symbol 0 */
    242, /* symbol 3 */
    121, /* symbol 2 */
    181, /* symbol 1 */
};

typedef struct {
    const uint8_t* data;
    size_t len;
    size_t byte_idx;
    uint8_t sym_idx; /* 0..3 symbols per byte */
    uint8_t phase;   /* 0=burst, 1=gap */
    uint8_t last_symbol;
    bool closing_burst_sent;
    bool last_done;
} PP4Tx;

static PP4Tx pp4_tx_state;
static bool ir_initialized = false;
static volatile bool ir_stop_requested = false;

static FuriHalInfraredTxGetDataState pp4_tx_cb(void* ctx, uint32_t* duration, bool* level) {
    PP4Tx* s = ctx;

    if(ir_stop_requested) {
        *duration = PP4_BURST_US;
        *level = false;
        return FuriHalInfraredTxGetDataStateLastDone;
    }

    if(s->byte_idx >= s->len) {
        if(!s->closing_burst_sent) {
            s->closing_burst_sent = true;
            *duration = PP4_BURST_US;
            *level = true;
            return FuriHalInfraredTxGetDataStateLastDone;
        }
        *duration = PP4_BURST_US;
        *level = false;
        return FuriHalInfraredTxGetDataStateLastDone;
    }

    uint8_t byte = s->data[s->byte_idx];
    uint8_t symbol = (byte >> (s->sym_idx * 2)) & 0x03;

    if(s->phase == 0) {
        *duration = PP4_BURST_US;
        *level = true;
        s->last_symbol = symbol;
        s->phase = 1;
    } else {
        *duration = pp4_gap_us[s->last_symbol];
        *level = false;
        s->phase = 0;
        s->sym_idx++;
        if(s->sym_idx >= 4) {
            s->sym_idx = 0;
            s->byte_idx++;
        }
    }
    return FuriHalInfraredTxGetDataStateOk;
}

void tagtinker_ir_init(void) {
    if(ir_initialized) return;
    ir_stop_requested = false;
    ir_initialized = true;
}

void tagtinker_ir_deinit(void) {
    if(!ir_initialized) return;
    tagtinker_ir_stop();
    ir_initialized = false;
}

bool tagtinker_ir_transmit(const uint8_t* data, size_t len, uint16_t repeats_raw, uint8_t delay) {
    if(!ir_initialized) return false;
    if(!data || len == 0 || len > 255) return false;

    ir_stop_requested = false;
    uint32_t repeats = repeats_raw & 0x7FFF;

    for(uint32_t rep = 0; rep <= repeats; rep++) {
        if(ir_stop_requested) return false;

        pp4_tx_state = (PP4Tx){
            .data = data,
            .len = len,
            .byte_idx = 0,
            .sym_idx = 0,
            .phase = 0,
            .last_symbol = 0,
            .closing_burst_sent = false,
            .last_done = false,
        };

        furi_hal_infrared_async_tx_set_data_isr_callback(pp4_tx_cb, &pp4_tx_state);
        furi_hal_infrared_async_tx_start(CARRIER_FREQ_HZ, CARRIER_DUTY);
        furi_hal_infrared_async_tx_wait_termination();

        if(rep < repeats) {
            if(delay > 0) {
                uint32_t delay_us = (uint32_t)delay * 500U;
                uint32_t delay_ms_yield = delay_us / 1000U;
                if(delay_ms_yield > 0) furi_delay_ms(delay_ms_yield);
            } else {
                if((rep % 10U) == 9U) furi_delay_ms(1);
            }
        }
    }

    return true;
}

void tagtinker_ir_stop(void) {
    ir_stop_requested = true;
}
