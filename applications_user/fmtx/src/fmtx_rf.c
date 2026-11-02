#include "fmtx_rf.h"
#include "fmtx_compat.h"

#include <stdlib.h>
#include <string.h>
#include <esp_heap_caps.h>
#include <furi_hal.h>
#include <lib/drivers/cc1101_regs.h>

// ESP32 port: the radio is reached through the Sub-GHz device API, which routes to the
// board's CC1101 ("cc1101_int") or to the external module that the user picked in
// Sub-GHz > Radio Settings ("cc1101_ext", shared SPI + chosen CS/GDO0 header pins). Upstream
// talked to furi_hal_subghz directly for the internal radio and switched the 5 V OTG rail on
// for the external one: the external module here is header-powered, and the radio HAL
// refuses TX outside the allowed bands by itself.
#define FMTX_MAX_RUN         2048U
#define FMTX_DEVICE_INTERNAL "cc1101_int"
#define FMTX_DEVICE_EXTERNAL "cc1101_ext"

static const uint8_t fmtx_preset[] = {
    CC1101_IOCFG0,
    0x0D,
    CC1101_FSCTRL1,
    0x06,
    CC1101_PKTCTRL0,
    0x32,
    CC1101_PKTCTRL1,
    0x04,
    CC1101_MDMCFG0,
    0x00,
    CC1101_MDMCFG1,
    0x02,
    CC1101_MDMCFG2,
    0x04,
    CC1101_MDMCFG3,
    0xE4,
    CC1101_MDMCFG4,
    0x6A,
    CC1101_DEVIATN,
    0x04,
    CC1101_MCSM0,
    0x18,
    CC1101_FOCCFG,
    0x16,
    CC1101_AGCCTRL0,
    0x91,
    CC1101_AGCCTRL1,
    0x00,
    CC1101_AGCCTRL2,
    0x07,
    CC1101_WORCTRL,
    0xFB,
    CC1101_FREND0,
    0x10,
    CC1101_FREND1,
    0x56,
    0x00,
    0x00,
    0xC0,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
};

Rf* rfalloc(void) {
    // The refill callback runs in the RMT interrupt: keep what it touches in internal RAM
    // (PSRAM is unreachable while the flash cache is off). Fail cleanly when internal RAM is
    // exhausted instead of returning a ring buffer that the interrupt cannot read.
    Rf* rf = heap_caps_malloc(sizeof(Rf), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return rf;
}

void rffree(Rf* rf) {
    free(rf);
}

void rfinit(Rf* rf, uint32_t hz) {
    memset(rf, 0, sizeof(*rf));
    rf->hz = hz;
    rf->regs = fmtx_preset;
    rf->sample_decisions = 3U;
    rf->gain = 2;
    rf->selected = FmtxRadioAuto;
}

const uint8_t* rfregs(void) {
    return fmtx_preset;
}

uint16_t rfused(const Rf* rf) {
    return (rf->head - rf->tail) & (RINGSZ - 1U);
}

void rfhold(Rf* rf, uint8_t decisions) {
    if(rf->on || decisions == 0) return;
    rf->sample_decisions = decisions;
    rf->sphase = 0;
}

static int16_t rfpop(Rf* rf) {
    uint16_t t;
    if(!rf->prime) {
        uint16_t threshold = rf->drain ? 1U : 512U;
        if(rfused(rf) < threshold) return 0;
        rf->prime = true;
    }
    t = rf->tail;
    if(t == rf->head) {
        rf->prime = false;
        return 0;
    }
    int32_t x = rf->ring[t] * rf->gain / 2;
    if(x > 32767) x = 32767;
    if(x < -32768) x = -32768;
    __DMB();
    rf->tail = (t + 1U) & (RINGSZ - 1U);
    if(rf->played_samples != UINT32_MAX) rf->played_samples++;
    return x;
}

void rfgain(Rf* rf, uint8_t gain) {
    if(!rf) return;
    rf->gain = gain;
    __DMB();
}

static LevelDuration rfbit(void* ctx) {
    Rf* rf = ctx;
    uint32_t us;
    if(rf->lock_decisions) {
        rf->lock_decisions--;
        rf->s = 0;
    } else if(rf->sphase == 0) {
        if(rf->drain && rf->tail == rf->head) return level_duration_reset();
        rf->s = rfpop(rf);
    }
    rf->sphase++;
    if(rf->sphase == rf->sample_decisions) rf->sphase = 0;
    // One PDM bit selects one of the two FSK deviations.
    rf->err += rf->s;
    bool bit = rf->err >= 0;
    // ESP32 port: the radio HAL merges equal consecutive bits into one RMT duration inside the
    // refill interrupt, so a saturated input (full-scale DC) would keep it spinning for ever.
    // Normal audio never repeats a bit this long (2048 decisions = 43 ms); the forced flip
    // is one 21 us glitch that the error feedback below absorbs.
    if(bit == rf->bit) {
        if(++rf->run >= FMTX_MAX_RUN) {
            bit = !bit;
            rf->run = 0;
        }
    } else {
        rf->run = 0;
    }
    rf->bit = bit;
    rf->err += bit ? -32767 : 32768;
    // Five decisions use 21 us, then one uses 20 us.
    rf->slot++;
    if(rf->slot == 6U) {
        rf->slot = 0;
        us = 20U;
    } else {
        us = 21U;
    }
    return level_duration_make(rf->bit, us);
}

static void txled(bool on) {
    if(on) {
        furi_hal_light_set(LightBlue, 0);
        furi_hal_light_set(LightGreen, 96);
        furi_hal_light_set(LightRed, 255);
    } else {
        furi_hal_light_set(LightRed | LightGreen, 0);
        furi_hal_light_set(LightBlue, 255);
    }
}

void rfledidle(void) {
    txled(false);
}

void rfledoff(void) {
    furi_hal_light_set(LightRed | LightGreen | LightBlue, 0);
}

void rfselect(Rf* rf, FmtxRadio radio) {
    if(!rf || rf->on || rf->awake) return;
    rf->selected = radio <= FmtxRadioInternal ? radio : FmtxRadioAuto;
}

// ---- device handling -------------------------------------------------------------------------

// Open the registry and pick the radio. The external module is started (SPI handle re-pointed,
// CS/GDO0 pads reserved) only here and released again by rfclose().
static bool rfopen(Rf* rf, bool external) {
    if(!rf->devices_started) {
        subghz_devices_init();
        rf->devices_started = true;
    }
    if(external) {
        const SubGhzDevice* ext = subghz_devices_get_by_name(FMTX_DEVICE_EXTERNAL);
        // is_connect(): chosen in Sub-GHz settings AND a CC1101 answers on the chosen pins.
        if(!ext || !subghz_devices_is_connect(ext) || !subghz_devices_begin(ext)) return false;
        rf->device = ext;
        rf->external = true;
        return true;
    }
    rf->device = subghz_devices_get_by_name(FMTX_DEVICE_INTERNAL);
    rf->external = false;
    return rf->device != NULL;
}

static void rfclose(Rf* rf) {
    if(rf->device) {
        subghz_devices_sleep(rf->device);
        if(rf->external) subghz_devices_end(rf->device); // releases the module and its pads
    }
    rf->device = NULL;
    rf->external = false;
    if(rf->devices_started) {
        subghz_devices_deinit();
        rf->devices_started = false;
    }
}

bool rfexternal(Rf* rf) {
    bool ok = false;
    if(!rf || rf->on || rf->awake || rf->devices_started) return false;
    subghz_devices_init();
    const SubGhzDevice* ext = subghz_devices_get_by_name(FMTX_DEVICE_EXTERNAL);
    ok = ext && subghz_devices_is_connect(ext);
    subghz_devices_deinit();
    return ok;
}

static bool rfstartdev(Rf* rf, bool external) {
    if(!rfopen(rf, external)) {
        rfclose(rf);
        return false;
    }
    subghz_devices_reset(rf->device);
    subghz_devices_idle(rf->device);
    subghz_devices_load_preset(rf->device, FuriHalSubGhzPresetCustom, (uint8_t*)rf->regs);
    (void)subghz_devices_set_frequency(rf->device, rf->hz);
    subghz_devices_flush_tx(rf->device);
    // start_async_tx() enters TX by itself and hands GDO0 to the RMT; it fails when the radio
    // is absent or the frequency is not allowed for transmission.
    rf->on = subghz_devices_start_async_tx(rf->device, rfbit, rf);
    if(!rf->on) rfclose(rf);
    return rf->on;
}

bool rfstart(Rf* rf) {
    if(rf->on) return true;
    if(!furi_hal_subghz_is_frequency_valid(rf->hz) || !furi_hal_subghz_is_tx_allowed(rf->hz))
        return false;
    furi_hal_power_insomnia_enter();
    rf->awake = true;
    rf->drain = false;
    // Start with 100 ms of zero samples while the carrier settles.
    rf->lock_decisions = 4800U;
    if(rf->selected != FmtxRadioInternal && rfstartdev(rf, true)) {
        txled(true);
        return true;
    }
    if(rf->selected != FmtxRadioExternal && rfstartdev(rf, false)) {
        txled(true);
        return true;
    }
    rfstop(rf);
    return rf->on;
}

bool rfresume(Rf* rf) {
    if(rf->on) return true;
    if(!rf->awake || !rf->device) return false;
    rf->drain = false;
    subghz_devices_idle(rf->device);
    (void)subghz_devices_set_frequency(rf->device, rf->hz);
    subghz_devices_flush_tx(rf->device);
    rf->on = subghz_devices_start_async_tx(rf->device, rfbit, rf);
    txled(rf->on);

    return rf->on;
}

void rfpause(Rf* rf) {
    if(rf->device) {
        if(rf->on) subghz_devices_stop_async_tx(rf->device);
    }
    rf->on = false;
    if(rf->device) subghz_devices_idle(rf->device);
    txled(false);
}

void rfstop(Rf* rf) {
    if(rf->device && rf->on) subghz_devices_stop_async_tx(rf->device);
    rf->on = false;
    rfclose(rf);
    txled(false);
    if(rf->awake) furi_hal_power_insomnia_exit();
    rf->awake = false;
}

bool rfput(Rf* rf, int16_t s) {
    // The async TX callback is the only reader.
    while(rf && rf->on) {
        uint16_t h = rf->head;
        uint16_t n = (h + 1U) & (RINGSZ - 1U);
        if(n != rf->tail) {
            rf->ring[h] = s;
            __DMB();
            rf->head = n;
            return true;
        }
        furi_delay_tick(1U);
    }
    return false;
}

void rfend(Rf* rf) {
    __DMB();
    rf->drain = true;
}

bool rfdone(const Rf* rf) {
    return rf->drain && rf->device && subghz_devices_is_async_complete_tx(rf->device);
}

bool rfdrain(Rf* rf, uint32_t timeout_ms) {
    uint32_t at;
    if(!rf || !rf->on) return false;
    if(rfused(rf)) rf->prime = true;
    rfend(rf);
    at = furi_get_tick();
    while(!rfdone(rf) && furi_get_tick() - at < furi_ms_to_ticks(timeout_ms))
        furi_delay_tick(1U);
    return rfdone(rf);
}

uint32_t rfplayed(const Rf* rf) {
    if(!rf) return 0;
    __DMB();
    return rf->played_samples;
}

void rfrst(Rf* rf) {
    if(rf->on) return;
    rf->head = 0;
    rf->tail = 0;
    rf->prime = false;
    rf->drain = false;
    rf->s = 0;
    rf->lock_decisions = 0;
    rf->played_samples = 0;
    rf->sphase = 0;
    rf->err = 0;
    rf->slot = 0;
    rf->bit = false;
    rf->run = 0;
}
