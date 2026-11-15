#include "radio.h"

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_region.h> // PORT: furi_hal.h here does not pull in the region API
#include <lib/subghz/devices/cc1101_configs.h>
#include <lib/subghz/devices/cc1101_int/cc1101_int_interconnect.h>
#include "idlog.h"

#define TAG "CwRadio"

#define CC1101_REG_MDMCFG4 0x10
#define CC1101_REG_DEVIATN 0x15

// DEVIATN = (E << 4) | M, f_dev = 26MHz / 2^17 * (8 + M) * 2^E
#define CC1101_DEVIATN_2K4 0x04 // 2.38 kHz
#define CC1101_DEVIATN_4K8 0x14 // 4.76 kHz

#define CC1101_REG_FSCTRL1 0x0B

// MDMCFG4 high nibble = (CHANBW_E << 2) | CHANBW_M
// BW = 26MHz / (8 * (4 + M) * 2^E)
static const uint8_t cc1101_chanbw[CwRxBwCount] = {
    0xF0, // 58 kHz
    0xC0, // 101 kHz
    0xA0, // 135 kHz
    0x60, // 270 kHz (stock)
};

// TI pairs a 152 kHz IF with filters this narrow; the stock 381 kHz IF is only
// right for the widest setting.
#define CC1101_FSCTRL1_152K 0x06

#define CW_PRESET_MAX_BYTES 96
#define CW_CHUNK_MAX_US     100000UL

/* The async TX engine never clocks out the first and last run of same-level
 * samples, so a 300 ms unkeyed preamble simply never reached the air. Bracket
 * the real stream with a short run of the OPPOSITE level: those throwaway runs
 * absorb the loss and everything the caller asked for is transmitted. */
#define CW_GUARD_US 1000UL

typedef enum {
    CwPhaseLeadGuard,
    CwPhaseStream,
    CwPhaseTailGuard,
    CwPhaseDone,
} CwPhase;

struct CwRadio {
    const SubGhzDevice* device;
    bool external;

    uint8_t preset[CW_PRESET_MAX_BYTES];

    // --- async TX state, touched from the timer ISR ---
    const MorseSegment* seg;
    size_t count;
    size_t idx;
    uint32_t remaining_us;
    bool cur_on;
    bool mcw;
    uint32_t half_us;
    bool tone_level;
    volatile uint32_t elapsed_us;
    uint32_t total_us;
    CwPhase phase;
    volatile bool abort;
    volatile bool sending;
};

/* ------------------------------------------------------------------ preset */

static void cw_preset_put(uint8_t* buf, size_t* len, uint8_t reg, uint8_t value) {
    for(size_t i = 0; i + 1 < *len; i += 2) {
        if(buf[i] == reg) {
            buf[i + 1] = value;
            return;
        }
    }
    buf[(*len)++] = reg;
    buf[(*len)++] = value;
}

/* Build a custom CC1101 preset from the stock one: narrow the receive filter so
 * the squelch only hears our own channel, and set the FM deviation we want.
 * Layout is what furi_hal_subghz_load_custom_preset() expects:
 * reg/value pairs, 0x00 0x00, then the 8-byte PA table. */
static void cw_radio_build_preset(CwRadio* radio, const CwRadioParams* params) {
    // Every mode except MCW keys a bare carrier, so they all share the OOK base.
    const uint8_t* base = (params->mode == CwModeMcw) ?
                              subghz_device_cc1101_preset_2fsk_dev2_38khz_async_regs :
                              subghz_device_cc1101_preset_ook_270khz_async_regs;

    size_t len = 0;
    for(size_t i = 0; base[i] != 0x00 && len + 12 < CW_PRESET_MAX_BYTES; i += 2) {
        radio->preset[len++] = base[i];
        radio->preset[len++] = base[i + 1];
    }

    uint8_t mdmcfg4 = 0x67; // stock: 270 kHz filter, DRATE_E 7
    for(size_t i = 0; i + 1 < len; i += 2) {
        if(radio->preset[i] == CC1101_REG_MDMCFG4) mdmcfg4 = radio->preset[i + 1];
    }
    CwRxBw bw = (params->rx_bw < CwRxBwCount) ? params->rx_bw : CwRxBw270;
    cw_preset_put(
        radio->preset, &len, CC1101_REG_MDMCFG4, cc1101_chanbw[bw] | (mdmcfg4 & 0x0F));
    if(bw != CwRxBw270) {
        cw_preset_put(radio->preset, &len, CC1101_REG_FSCTRL1, CC1101_FSCTRL1_152K);
    }

    if(params->mode == CwModeMcw) {
        cw_preset_put(
            radio->preset,
            &len,
            CC1101_REG_DEVIATN,
            (params->deviation == CwDeviationWide) ? CC1101_DEVIATN_4K8 : CC1101_DEVIATN_2K4);
    }

    radio->preset[len++] = 0x00;
    radio->preset[len++] = 0x00;

    // OOK keys between PA table entry 0 (off) and 1 (max); FSK holds entry 0.
    memset(&radio->preset[len], 0, 8);
    if(params->mode == CwModeMcw) {
        radio->preset[len + 0] = 0xC0;
    } else {
        radio->preset[len + 1] = 0xC0;
    }
    len += 8;
    furi_assert(len <= CW_PRESET_MAX_BYTES);
}

/* The SSB modes are how a real rig sends CW into an SSB passband: key the bare
 * carrier tone_hz away from the dial frequency, and a receiver sitting on the
 * dial in the matching sideband hears the beat note at exactly tone_hz. The
 * CC1101 synthesizer steps in ~397 Hz increments (26 MHz / 2^16), so the pitch
 * lands on the nearest step - close enough for an ID. */
static uint32_t cw_radio_tx_frequency(const CwRadioParams* params) {
    uint32_t tone = params->tone_hz ? params->tone_hz : 800;
    if(params->mode == CwModeSsbUsb) return params->frequency + tone;
    if(params->mode == CwModeSsbLsb) return params->frequency - tone;
    return params->frequency;
}

static const char* const cw_mode_log_names[CwModeCount] = {"MCW", "OOK", "SSB-USB", "SSB-LSB"};

/* --------------------------------------------------------------- async TX */

static LevelDuration cw_radio_async_callback(void* context) {
    CwRadio* radio = context;

    if(radio->phase == CwPhaseLeadGuard) {
        radio->phase = CwPhaseStream;
        return level_duration_make(true, CW_GUARD_US);
    }

    while(true) {
        if(radio->abort) return level_duration_reset();

        if(radio->remaining_us == 0) {
            if(radio->idx >= radio->count) {
                if(radio->phase == CwPhaseStream) {
                    radio->phase = CwPhaseTailGuard;
                    return level_duration_make(true, CW_GUARD_US);
                }
                return level_duration_reset();
            }
            const MorseSegment* segment = &radio->seg[radio->idx++];
            radio->remaining_us = segment->duration_us;
            radio->cur_on = segment->on;
            if(!radio->cur_on) radio->tone_level = false; // every tone burst starts in phase
            continue;
        }

        uint32_t duration;
        bool level;

        if(radio->mcw && radio->cur_on) {
            // Square-wave the two FSK tones at audio rate: an FM discriminator
            // turns that back into a tone.
            duration = radio->half_us;
            if(duration > radio->remaining_us) duration = radio->remaining_us;
            level = radio->tone_level;
            radio->tone_level = !radio->tone_level;
        } else {
            duration = radio->remaining_us;
            if(duration > CW_CHUNK_MAX_US) duration = CW_CHUNK_MAX_US;
            // MCW spaces hold one tone: carrier stays up, audio goes quiet.
            level = radio->mcw ? false : radio->cur_on;
        }

        radio->remaining_us -= duration;
        radio->elapsed_us += duration;
        return level_duration_make(level, duration);
    }
}

/* ------------------------------------------------------------------- radio */

CwRadio* cw_radio_alloc(void) {
    CwRadio* radio = malloc(sizeof(CwRadio));
    memset(radio, 0, sizeof(CwRadio));
    subghz_devices_init();
    return radio;
}

void cw_radio_free(CwRadio* radio) {
    furi_check(radio);
    cw_radio_close(radio);
    subghz_devices_deinit();
    free(radio);
}

bool cw_radio_open(CwRadio* radio, bool external) {
    furi_check(radio);
    if(radio->device) cw_radio_close(radio);

    const SubGhzDevice* device =
        subghz_devices_get_by_name(external ? "cc1101_ext" : SUBGHZ_DEVICE_CC1101_INT_NAME);
    if(!device) {
        morse_log("radio: no device named %s", external ? "cc1101_ext" : "cc1101_int");
        return false;
    }
    // The internal CC1101 has no begin() at all, so subghz_devices_begin()
    // returns false for it as a matter of course - only an external module
    // reports anything meaningful here.
    bool begun = subghz_devices_begin(device);
    if(external && (!begun || !subghz_devices_is_connect(device))) {
        subghz_devices_end(device);
        morse_log("radio: external module did not come up");
        return false;
    }

    subghz_devices_reset(device);
    subghz_devices_idle(device);
    radio->device = device;
    radio->external = external;
    return true;
}

void cw_radio_close(CwRadio* radio) {
    furi_check(radio);
    if(!radio->device) return;
    subghz_devices_idle(radio->device);
    subghz_devices_sleep(radio->device);
    subghz_devices_end(radio->device);
    radio->device = NULL;
}

bool cw_radio_is_open(const CwRadio* radio) {
    return radio && radio->device != NULL;
}

bool cw_radio_is_external(const CwRadio* radio) {
    return radio && radio->external;
}

bool cw_radio_frequency_supported(uint32_t frequency) {
    /* Ask the running firmware rather than hardcoding a range: the tunable
     * span differs between firmware builds. This is the furi_hal function,
     * NOT subghz_devices_is_frequency_valid(), whose internal-radio
     * implementation calls furi_crash() instead of returning false. */
    return furi_hal_subghz_is_frequency_valid(frequency);
}

bool cw_radio_tx_allowed(uint32_t frequency) {
    /* furi_hal_region_is_frequency_allowed() exists in both the official and
     * the Unleashed SDK; furi_hal_subghz_is_tx_allowed() is Unleashed-only. */
    return furi_hal_region_is_frequency_allowed(frequency);
}

bool cw_radio_frequency_valid(CwRadio* radio, uint32_t frequency) {
    UNUSED(radio);
    return cw_radio_frequency_supported(frequency);
}

bool cw_radio_listen(CwRadio* radio, const CwRadioParams* params) {
    furi_check(radio);
    if(!radio->device) return false;
    if(!cw_radio_frequency_supported(params->frequency)) {
        morse_log("listen: %lu Hz is outside the radio's tuning range", params->frequency);
        return false;
    }

    subghz_devices_idle(radio->device);
    cw_radio_build_preset(radio, params);
    subghz_devices_load_preset(radio->device, FuriHalSubGhzPresetCustom, radio->preset);
    uint32_t tuned = subghz_devices_set_frequency(radio->device, params->frequency);
    morse_log("listen: asked %lu Hz, tuned %lu Hz", params->frequency, tuned);
    subghz_devices_flush_rx(radio->device);
    subghz_devices_set_rx(radio->device);
    return true;
}

float cw_radio_rssi(CwRadio* radio) {
    if(!radio || !radio->device) return -127.0f;
    return subghz_devices_get_rssi(radio->device);
}

void cw_radio_idle(CwRadio* radio) {
    if(radio && radio->device) subghz_devices_idle(radio->device);
}

bool cw_radio_send(CwRadio* radio, const MorseStream* stream, const CwRadioParams* params) {
    furi_check(radio);
    if(!radio->device || !stream || stream->count == 0) {
        morse_log("send: no device or empty stream");
        return false;
    }
    // SSB modes key the carrier offset from the dial by the tone pitch.
    uint32_t tx_frequency = cw_radio_tx_frequency(params);
    if(!cw_radio_frequency_supported(tx_frequency)) {
        morse_log("send: %lu Hz is outside the radio's tuning range", tx_frequency);
        return false;
    }

    radio->seg = stream->seg;
    radio->count = stream->count;
    radio->idx = 0;
    radio->remaining_us = 0;
    radio->cur_on = false;
    radio->mcw = (params->mode == CwModeMcw);
    radio->half_us = 500000UL / (params->tone_hz ? params->tone_hz : 800);
    radio->tone_level = false;
    radio->elapsed_us = 0;
    radio->total_us = stream->total_us ? stream->total_us : 1;
    radio->phase = CwPhaseLeadGuard;
    radio->abort = false;

    subghz_devices_idle(radio->device);
    cw_radio_build_preset(radio, params);
    subghz_devices_load_preset(radio->device, FuriHalSubGhzPresetCustom, radio->preset);
    subghz_devices_set_frequency(radio->device, tx_frequency);

    if(!cw_radio_tx_allowed(tx_frequency)) {
        morse_log(
            "send: firmware does not permit TX at %lu Hz (region %s)",
            tx_frequency,
            furi_hal_region_get_name());
        return false;
    }

    furi_hal_power_suppress_charge_enter();
    radio->sending = true;

    uint32_t started = furi_get_tick();
    bool ok = subghz_devices_set_tx(radio->device);
    if(ok) {
        if(subghz_devices_start_async_tx(radio->device, cw_radio_async_callback, radio)) {
            while(!subghz_devices_is_async_complete_tx(radio->device)) {
                furi_delay_ms(5);
            }
            subghz_devices_stop_async_tx(radio->device);
            morse_log(
                "send: %lu Hz %s %u seg, %lu ms planned, %lu ms actual",
                tx_frequency,
                cw_mode_log_names[(params->mode < CwModeCount) ? params->mode : CwModeMcw],
                (unsigned)stream->count,
                stream->total_us / 1000,
                furi_get_tick() - started);
        } else {
            ok = false;
            morse_log("send: start_async_tx() refused");
        }
    } else {
        morse_log(
            "send: firmware refused TX at %lu Hz (outside its default TX range; "
            "the extended-range setting widens it)",
            tx_frequency);
        FURI_LOG_E(TAG, "TX refused at %lu Hz", tx_frequency);
    }

    radio->sending = false;
    subghz_devices_idle(radio->device);
    furi_hal_power_suppress_charge_exit();
    return ok;
}

void cw_radio_abort(CwRadio* radio) {
    if(radio) radio->abort = true;
}

uint8_t cw_radio_progress(const CwRadio* radio) {
    if(!radio || radio->total_us == 0) return 0;
    uint32_t pct = (radio->elapsed_us / 1000) * 100 / (radio->total_us / 1000 + 1);
    return pct > 100 ? 100 : (uint8_t)pct;
}

bool cw_radio_is_sending(const CwRadio* radio) {
    return radio && radio->sending;
}
