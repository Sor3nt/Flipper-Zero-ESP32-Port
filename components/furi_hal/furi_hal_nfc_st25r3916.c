/**
 * @file furi_hal_nfc_st25r3916.c
 * @brief NFC HAL implementation for the ST25R3916 front-end (SPI).
 *
 * Ported from the Flipper Zero firmware (targets/f7/furi_hal/furi_hal_nfc.c).
 * The only ESP-IDF specific changes are:
 *  - the SPI bus handle (`furi_hal_spi_bus_handle_nfc`) lives in furi_hal_spi.c,
 *  - chip select is handled by the ESP-IDF SPI driver (see st25r3916_reg.c),
 *  - a few PN532-only entry points used by this firmware are provided as
 *    compatibility stubs at the bottom of the file.
 */

#include "furi_hal_nfc_i.h"
#include "furi_hal_nfc_tech_i.h"

#include "st25r3916.h"

#include <furi.h>
#include <furi_hal_spi.h>

#include <driver/gpio.h>
#include <esp_rom_sys.h>
#include "boards/board.h"
#include <furi_hal_resources.h>

#define TAG "FuriHalNfc"

const FuriHalNfcTechBase* const furi_hal_nfc_tech[FuriHalNfcTechNum] = {
    [FuriHalNfcTechIso14443a] = &furi_hal_nfc_iso14443a,
    [FuriHalNfcTechIso14443b] = &furi_hal_nfc_iso14443b,
    [FuriHalNfcTechIso15693] = &furi_hal_nfc_iso15693,
    [FuriHalNfcTechFelica] = &furi_hal_nfc_felica,
};

FuriHalNfc furi_hal_nfc;

static FuriHalNfcError furi_hal_nfc_turn_on_osc(const FuriHalSpiBusHandle* handle) {
    FuriHalNfcError error = FuriHalNfcErrorNone;
    furi_hal_nfc_event_start();

    if(!st25r3916_check_reg(
           handle,
           ST25R3916_REG_OP_CONTROL,
           ST25R3916_REG_OP_CONTROL_en,
           ST25R3916_REG_OP_CONTROL_en)) {
        st25r3916_mask_irq(handle, ~ST25R3916_IRQ_MASK_OSC);
        st25r3916_set_reg_bits(handle, ST25R3916_REG_OP_CONTROL, ST25R3916_REG_OP_CONTROL_en);
        furi_hal_nfc_event_wait_for_specific_irq(handle, ST25R3916_IRQ_MASK_OSC, 10);
    }
    // Disable IRQs
    st25r3916_mask_irq(handle, ST25R3916_IRQ_MASK_ALL);

    bool osc_on = st25r3916_check_reg(
        handle,
        ST25R3916_REG_AUX_DISPLAY,
        ST25R3916_REG_AUX_DISPLAY_osc_ok,
        ST25R3916_REG_AUX_DISPLAY_osc_ok);
    if(!osc_on) {
        error = FuriHalNfcErrorOscillator;
    }

    return error;
}

/* Chip identification for the ST25R3916.
 *
 * This front-end has no reset line on the fatalcore board and keeps its
 * register state across an MCU reset, so it can be found in any state a
 * previous firmware run left behind. In particular, when OP_CONTROL.en is
 * clear the 27.12 MHz oscillator is stopped and IC_IDENTITY reads back as
 * 0x00 (or a stale byte) even though the chip is perfectly alive. The old
 * init read the id first and gave up, which is why NFC reported
 * "NFC Chip Failed" while a Bruce build on the same board read 0x2A.
 *
 * Evidence from the failing board: the first read returned 0x10 with
 * OP_CONTROL = 0x00; after forcing the oscillator on, the same read returned
 * 0x2A and IO_CONF2 read back as 0x04.
 *
 * So the sequence is: SET_DEFAULT, restore the IO driver strength, start the
 * oscillator and wait for osc_ok, then read the type. Each retry also
 * re-creates the ESP-IDF SPI device, because the display driver re-initialises
 * the shared host after the early probe and can leave our device handle in a
 * state where transactions go nowhere. */
#define FURI_HAL_NFC_CHIP_ID_RETRIES 5
#define FURI_HAL_NFC_OSC_SETTLE_MS   40

/* Start the oscillator without relying on a readable MISO or on the GPIO IRQ
 * handler (which is not installed yet at this point). The write goes straight
 * to OP_CONTROL instead of a read-modify-write, because readback is exactly
 * what is not trustworthy on a chip that has not answered yet. */
static bool furi_hal_nfc_osc_wake(const FuriHalSpiBusHandle* handle, uint8_t* aux_out) {
    st25r3916_write_reg(handle, ST25R3916_REG_OP_CONTROL, ST25R3916_REG_OP_CONTROL_en);

    uint8_t aux = 0;
    for(uint32_t waited = 0; waited <= FURI_HAL_NFC_OSC_SETTLE_MS; waited += 5) {
        furi_delay_ms(5);
        aux = 0;
        st25r3916_read_reg(handle, ST25R3916_REG_AUX_DISPLAY, &aux);
        if(aux & ST25R3916_REG_AUX_DISPLAY_osc_ok) break;
    }

    if(aux_out) *aux_out = aux;
    return (aux & ST25R3916_REG_AUX_DISPLAY_osc_ok) != 0;
}

static bool furi_hal_nfc_chip_id_matches(uint8_t chip_id) {
    const uint8_t type = chip_id & ST25R3916_REG_IC_IDENTITY_ic_type_mask;
    return type == ST25R3916_REG_IC_IDENTITY_ic_type_st25r3916 ||
           type == ST25R3916_REG_IC_IDENTITY_ic_type_st25r3916b;
}

/* Non-destructive variant used by furi_hal_nfc_is_hal_ready(): read the type,
 * and if the chip looks asleep just start its oscillator. No SET_DEFAULT here,
 * because that would throw away the register configuration that
 * furi_hal_nfc_init() already applied. */
static bool furi_hal_nfc_read_chip_id(const FuriHalSpiBusHandle* handle, uint8_t* chip_id_out) {
    uint8_t chip_id = 0;
    st25r3916_read_reg(handle, ST25R3916_REG_IC_IDENTITY, &chip_id);

    if(!furi_hal_nfc_chip_id_matches(chip_id)) {
        uint8_t aux = 0;
        if(furi_hal_nfc_osc_wake(handle, &aux)) {
            st25r3916_read_reg(handle, ST25R3916_REG_IC_IDENTITY, &chip_id);
        }
    }

    *chip_id_out = chip_id;
    return furi_hal_nfc_chip_id_matches(chip_id);
}

static bool furi_hal_nfc_identify_chip(
    const FuriHalSpiBusHandle* handle,
    uint8_t* chip_id_out) {
    FuriHalSpiBusHandle* mutable_handle = (FuriHalSpiBusHandle*)handle;
    uint8_t last_id = 0;

    for(int attempt = 0; attempt < FURI_HAL_NFC_CHIP_ID_RETRIES; attempt++) {
        if(attempt == 0) {
            /* Record the state the chip was left in by whatever ran before
             * (previous firmware boot, Bruce, or nothing): it keeps registers
             * across MCU resets and has no reset line here, so this is the
             * only way to tell "was asleep" from "was woken up". */
            uint8_t raw_id = 0;
            uint8_t op = 0;
            st25r3916_read_reg(handle, ST25R3916_REG_IC_IDENTITY, &raw_id);
            st25r3916_read_reg(handle, ST25R3916_REG_OP_CONTROL, &op);
            FURI_LOG_I(
                TAG,
                "init: before reset: OP_CONTROL=0x%02X IC_IDENTITY=0x%02X",
                op,
                raw_id);
        }

        if(attempt > 0) {
            /* Start the retry from a fresh SPI device: remove_device() also
             * parks CS high instead of leaving it floating for the other
             * devices on the shared bus. */
            furi_hal_spi_bus_handle_deinit(mutable_handle);
            furi_hal_spi_bus_handle_init(mutable_handle);
            if(!mutable_handle->initialized) {
                FURI_LOG_W(TAG, "init: attempt %d: SPI device not initialised", attempt);
                continue;
            }
        }

        st25r3916_direct_cmd(handle, ST25R3916_CMD_SET_DEFAULT);
        furi_delay_ms(5);
        /* Increase IO driver strength of MISO and IRQ. */
        st25r3916_write_reg(handle, ST25R3916_REG_IO_CONF2, ST25R3916_REG_IO_CONF2_io_drv_lvl);

        uint8_t aux = 0;
        const bool osc_ok = furi_hal_nfc_osc_wake(handle, &aux);

        uint8_t chip_id = 0;
        st25r3916_read_reg(handle, ST25R3916_REG_IC_IDENTITY, &chip_id);
        last_id = chip_id;

        uint8_t io2 = 0;
        st25r3916_read_reg(handle, ST25R3916_REG_IO_CONF2, &io2);

        FURI_LOG_I(
            TAG,
            "init: attempt %d: io2=0x%02X osc_ok=%d aux=0x%02X IC_IDENTITY=0x%02X",
            attempt,
            io2,
            (int)osc_ok,
            aux,
            chip_id);

        if(furi_hal_nfc_chip_id_matches(chip_id)) {
            *chip_id_out = chip_id;
            return true;
        }
    }

    *chip_id_out = last_id;
    return false;
}

/* Tear down the NFC runtime after a failed bring-up, but leave the front-end
 * oscillator running. furi_hal_nfc_low_power_mode_start() clears OP_CONTROL.en,
 * and on this board (no reset line, state survives MCU resets) that turns one
 * bad boot into a permanent state: the next boot finds the chip asleep again
 * and fails identically, reflash after reflash. */
static void furi_hal_nfc_deinit_keep_osc(const FuriHalSpiBusHandle* handle) {
    st25r3916_write_reg(handle, ST25R3916_REG_OP_CONTROL, ST25R3916_REG_OP_CONTROL_en);
    furi_hal_nfc_deinit_gpio_isr();
    furi_hal_nfc_timers_deinit();
    furi_hal_nfc_event_stop();
}

#ifdef BOARD_NFC_CHIP_ID_SWEEP
/* Bring-up helper: try to wake the front-end at several SPI clocks.
 *
 * The chip answers at 4 MHz once its oscillator is running, but a chip in
 * power-down may be clocked from its internal RC oscillator and then tolerate
 * far less on SCK. Every step writes OP_CONTROL.en (a write does not need a
 * working MISO), waits for the oscillator, and then reads the oscillator
 * status, the chip id and one written-back register. That separates
 * "transaction is garbled at this clock" from "the chip does not answer at
 * any clock". */
static void furi_hal_nfc_chip_id_sweep(void) {
    static const uint32_t freqs[] = {4000000, 2000000, 1000000, 500000, 200000, 100000};
    static const char* const names[] = {"4MHz", "2MHz", "1MHz", "500kHz", "200kHz", "100kHz"};
    FuriHalSpiBusHandle* handle = (FuriHalSpiBusHandle*)&furi_hal_spi_bus_handle_nfc;
    const uint32_t saved_hz = handle->frequency_hz;

    for(size_t f = 0; f < sizeof(freqs) / sizeof(freqs[0]); f++) {
        furi_hal_spi_bus_handle_deinit(handle);
        handle->frequency_hz = freqs[f];
        furi_hal_spi_bus_handle_init(handle);

        uint8_t op = 0;
        uint8_t aux = 0;
        uint8_t id = 0;
        uint8_t io2 = 0;

        st25r3916_write_reg(handle, ST25R3916_REG_OP_CONTROL, ST25R3916_REG_OP_CONTROL_en);
        furi_delay_ms(50);
        st25r3916_read_reg(handle, ST25R3916_REG_OP_CONTROL, &op);
        st25r3916_read_reg(handle, ST25R3916_REG_AUX_DISPLAY, &aux);
        st25r3916_read_reg(handle, ST25R3916_REG_IC_IDENTITY, &id);

        /* Independent write/read-back, so a dead read path cannot be mistaken
         * for a rejected write. */
        st25r3916_write_reg(handle, ST25R3916_REG_IO_CONF2, ST25R3916_REG_IO_CONF2_io_drv_lvl);
        st25r3916_read_reg(handle, ST25R3916_REG_IO_CONF2, &io2);

        FURI_LOG_I(
            TAG,
            "wake sweep %s: op=0x%02X aux=0x%02X io2=0x%02X id=0x%02X",
            names[f],
            op,
            aux,
            io2,
            id);
    }

    furi_hal_spi_bus_handle_deinit(handle);
    handle->frequency_hz = saved_hz;
    furi_hal_spi_bus_handle_init(handle);
}
#endif

#ifdef BOARD_NFC_IRQ_ADC_PROBE
/* Bring-up helper: is the IRQ line actively driven, or just held low by a
 * resistor? The digital pull-up/pull-down test cannot tell the two apart, the
 * ADC can: with the internal ~45k pull-up enabled, a chip that drives IRQ low
 * reads ~0 V, a plain 10k pull-down resistor reads ~0.6 V and an open pin
 * reads ~3.3 V. The IRQ pin is ADC2_CH1 on the ESP32-S3.
 *
 * A resistor that wins over the pull-up also means the chip is not driving
 * the line -- i.e. either no supply on the front-end or a broken trace. */
#include <esp_adc/adc_oneshot.h>

static int nfc_irq_read_mv(adc_oneshot_unit_handle_t unit, adc_channel_t ch) {
    int raw = 0;
    int acc = 0;
    for(int i = 0; i < 16; i++) {
        if(adc_oneshot_read(unit, ch, &raw) != ESP_OK) return -1;
        acc += raw;
    }
    /* 12 dB attenuation, 12 bit: roughly 3.1 V full scale. */
    return (acc / 16) * 3100 / 4095;
}

static void furi_hal_nfc_irq_adc_probe(void) {
    const gpio_num_t irq = (gpio_num_t)gpio_nfc_irq.pin;
    adc_unit_t unit = ADC_UNIT_1;
    adc_channel_t ch = ADC_CHANNEL_0;
    if(adc_oneshot_io_to_channel((int)irq, &unit, &ch) != ESP_OK) {
        FURI_LOG_E(TAG, "irq adc: GPIO%u is not an ADC pin", (unsigned)irq);
        return;
    }

    adc_oneshot_unit_handle_t adc = NULL;
    const adc_oneshot_unit_init_cfg_t unit_cfg = {.unit_id = unit};
    if(adc_oneshot_new_unit(&unit_cfg, &adc) != ESP_OK) {
        FURI_LOG_E(TAG, "irq adc: unit init failed");
        return;
    }
    const adc_oneshot_chan_cfg_t ch_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    adc_oneshot_config_channel(adc, ch, &ch_cfg);

    gpio_set_direction(irq, GPIO_MODE_INPUT);
    const struct {
        gpio_pull_mode_t mode;
        const char* name;
    } steps[] = {
        {GPIO_FLOATING, "float"},
        {GPIO_PULLUP_ONLY, "pullup"},
        {GPIO_PULLDOWN_ONLY, "pulldown"},
    };
    int mv[3];
    for(size_t i = 0; i < 3; i++) {
        gpio_set_pull_mode(irq, steps[i].mode);
        furi_delay_ms(2);
        mv[i] = nfc_irq_read_mv(adc, ch);
        FURI_LOG_I(
            TAG, "irq adc: %s=%dmV", steps[i].name, mv[i]);
    }
    gpio_set_pull_mode(irq, GPIO_PULLUP_ONLY);

    if(mv[1] < 100) {
        FURI_LOG_I(TAG, "irq adc: line is ACTIVELY DRIVEN low -> front-end is powered");
    } else if(mv[1] < 2500) {
        FURI_LOG_E(
            TAG,
            "irq adc: line is held by a resistor (%dmV under a ~45k pull-up) "
            "-> front-end does NOT drive it (no supply or broken trace)",
            mv[1]);
    } else {
        FURI_LOG_E(TAG, "irq adc: line floats -> IRQ pin not connected to anything");
    }

    adc_oneshot_del_unit(adc);

    /* Sanity check for the measurement itself: the IR receiver output is an
     * external push-pull driver that idles high. If this pin does not read a
     * few volts, the ADC path is broken and the IRQ numbers above mean
     * nothing. */
    adc_unit_t s_unit = ADC_UNIT_1;
    adc_channel_t s_ch = ADC_CHANNEL_0;
    const gpio_num_t sanity = (gpio_num_t)BOARD_PIN_IR_RX;
    if(adc_oneshot_io_to_channel((int)sanity, &s_unit, &s_ch) == ESP_OK) {
        adc_oneshot_unit_handle_t s_adc = NULL;
        const adc_oneshot_unit_init_cfg_t s_unit_cfg = {.unit_id = s_unit};
        if(adc_oneshot_new_unit(&s_unit_cfg, &s_adc) == ESP_OK) {
            adc_oneshot_config_channel(s_adc, s_ch, &ch_cfg);
            FURI_LOG_I(
                TAG,
                "irq adc: sanity GPIO%u (IR RX, idles high) = %dmV",
                (unsigned)sanity,
                nfc_irq_read_mv(s_adc, s_ch));
            adc_oneshot_del_unit(s_adc);
        }
    }
}
#endif /* BOARD_NFC_IRQ_ADC_PROBE */

#ifdef BOARD_NFC_EXEC_PROBE
/* Bring-up helper: answer "does the chip execute what we send it?" without
 * needing a readable MISO.
 *
 * The ST25R3916 drives its IRQ line push-pull (we already measured that: the
 * pin is actively held low when idle) and latches interrupt events that we can
 * select through the IRQ mask registers. Two events need no RF field:
 *
 *  - DCT ("direct command termination"): raised when a direct command with a
 *    defined duration finishes. Uses the chip's internal RC oscillator, so it
 *    works even with a dead 27.12 MHz crystal.
 *  - OSC (oscillator stable): raised when the 27.12 MHz crystal oscillator has
 *    started after the `en` bit is set.
 *
 * If DCT fires, chip select, clock, data-in, supply and the chip core all
 * work, and only the data-out (MISO) path is broken. If DCT does not fire,
 * the commands never reach the chip (CS/SCK/MOSI open, no supply) or the
 * core is dead. OSC then tells a working crystal apart from a dead one.
 *
 * IRQ is active high and driven low when idle. The whole probe is polled, so
 * it works before the GPIO interrupt handler is installed.
 */
static bool nfc_exec_wait_irq(gpio_num_t irq, uint32_t timeout_ms, uint32_t* waited_us) {
    uint32_t waited = 0;
    const uint32_t timeout = timeout_ms * 1000U;
    while(waited < timeout && gpio_get_level(irq) == 0) {
        esp_rom_delay_us(50);
        waited += 50;
    }
    *waited_us = waited;
    return gpio_get_level(irq) != 0;
}

static void furi_hal_nfc_exec_probe(const FuriHalSpiBusHandle* handle) {
    const gpio_num_t irq = (gpio_num_t)gpio_nfc_irq.pin;
    const gpio_config_t irq_cfg = {
        .pin_bit_mask = 1ULL << irq,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&irq_cfg);

    const int idle = gpio_get_level(irq);

    /* Known state, then mask everything. Register value 1 = masked. */
    st25r3916_direct_cmd(handle, ST25R3916_CMD_SET_DEFAULT);
    furi_delay_ms(5);
    st25r3916_mask_irq(handle, ST25R3916_IRQ_MASK_ALL);
    const int after_default = gpio_get_level(irq);

    /* --- Test 1: direct command termination, no crystal required. --- */
    st25r3916_mask_irq(handle, ~ST25R3916_IRQ_MASK_DCT);
    st25r3916_direct_cmd(handle, ST25R3916_CMD_ADJUST_REGULATORS);
    uint32_t dct_us = 0;
    const bool dct_irq = nfc_exec_wait_irq(irq, 20, &dct_us);
    st25r3916_mask_irq(handle, ST25R3916_IRQ_MASK_ALL);
    furi_delay_ms(1);
    const int after_dct_clear = gpio_get_level(irq);
    st25r3916_direct_cmd(handle, ST25R3916_CMD_CLEAR_FIFO);
    furi_delay_ms(1);

    /* --- Test 2: crystal oscillator start. --- */
    st25r3916_mask_irq(handle, ~ST25R3916_IRQ_MASK_OSC);
    /* Direct write instead of read-modify-write: MISO may be dead. */
    st25r3916_write_reg(handle, ST25R3916_REG_OP_CONTROL, ST25R3916_REG_OP_CONTROL_en);
    uint32_t osc_us = 0;
    const bool osc_irq = nfc_exec_wait_irq(irq, 20, &osc_us);

    uint8_t aux = 0;
    st25r3916_read_reg(handle, ST25R3916_REG_AUX_DISPLAY, &aux);
    st25r3916_mask_irq(handle, ST25R3916_IRQ_MASK_ALL);

    FURI_LOG_I(
        TAG,
        "exec probe: irq idle=%d after_default=%d after_dct=%d dct_irq=%d dct_us=%lu "
        "osc_irq=%d osc_us=%lu aux=0x%02X",
        idle,
        after_default,
        after_dct_clear,
        (int)dct_irq,
        (unsigned long)dct_us,
        (int)osc_irq,
        (unsigned long)osc_us,
        aux);

    if(dct_irq) {
        FURI_LOG_W(
            TAG,
            "exec probe: chip EXECUTES commands -> supply/core/CS/SCK/MOSI are OK, "
            "fault is data-out (MISO) %s",
            osc_irq ? "or a chip register state; crystal runs" : "and/or the crystal is dead");
    } else {
        FURI_LOG_E(
            TAG,
            "exec probe: no DCT -> chip does not execute commands "
            "(no supply, CS/SCK/MOSI open, or core dead)");
    }

    /* Leave the oscillator running. Writing 0 here puts the chip to sleep, and
     * because it has no reset line and keeps state across MCU resets the next
     * boot then looks exactly like a dead chip. */
    st25r3916_write_reg(handle, ST25R3916_REG_OP_CONTROL, ST25R3916_REG_OP_CONTROL_en);

    /* Can the IRQ output drive the line high at all? With the internal pull-up
     * enabled a pin that still reads 0 is either actively driven low by the
     * chip or shorted to ground; a floating or released line follows the
     * pull-up. */
    const gpio_config_t irq_pullup_cfg = {
        .pin_bit_mask = 1ULL << irq,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&irq_pullup_cfg);
    furi_delay_ms(1);
    FURI_LOG_I(TAG, "exec probe: irq with internal pull-up=%d", gpio_get_level(irq));
    gpio_set_direction(irq, GPIO_MODE_INPUT);
}
#endif /* BOARD_NFC_EXEC_PROBE */

#ifdef BOARD_NFC_EARLY_PROBE
#ifdef BOARD_NFC_BITBANG_PROBE
/* Last-resort bring-up probe: read IC_IDENTITY with plain GPIO toggling, no
 * ESP-IDF SPI driver at all. If the chip answers here but not through
 * furi_hal_spi_bus_trx(), the problem is in our SPI setup (mode/timing/CS);
 * if it stays 0x00 the chip or its SPI wiring is not there, no matter what the
 * driver does. Hand-clocked at ~5 kHz, so it also covers a marginal/failing
 * solder joint that the hardware SPI at 4 MHz cannot. */
static uint8_t nfc_bb_read_reg(
    gpio_num_t cs,
    gpio_num_t sck,
    gpio_num_t mosi,
    gpio_num_t miso,
    uint8_t cmd,
    bool cpol) {
    gpio_set_level(cs, 1);
    gpio_set_level(sck, cpol ? 1 : 0);
    gpio_set_level(mosi, 0);
    esp_rom_delay_us(500);
    gpio_set_level(cs, 0);
    esp_rom_delay_us(500);

    for(int i = 7; i >= 0; i--) {
        gpio_set_level(mosi, (cmd >> i) & 1);
        esp_rom_delay_us(100);
        gpio_set_level(sck, cpol ? 0 : 1); /* leading edge */
        esp_rom_delay_us(100);
        gpio_set_level(sck, cpol ? 1 : 0); /* trailing edge: slave samples MOSI */
        esp_rom_delay_us(100);
    }

    uint8_t val = 0;
    for(int i = 7; i >= 0; i--) {
        gpio_set_level(sck, cpol ? 0 : 1); /* leading edge: slave shifts MISO out */
        esp_rom_delay_us(100);
        if(gpio_get_level(miso)) val |= (uint8_t)(1u << i);
        gpio_set_level(sck, cpol ? 1 : 0);
        esp_rom_delay_us(100);
    }

    gpio_set_level(cs, 1);
    esp_rom_delay_us(500);
    return val;
}

static void nfc_bb_probe(void) {
    const FuriHalSpiBusHandle* handle = &furi_hal_spi_bus_handle_nfc;
    const gpio_num_t cs = (gpio_num_t)handle->cs->pin;
    const gpio_num_t sck = (gpio_num_t)handle->sck->pin;
    const gpio_num_t mosi = (gpio_num_t)handle->mosi->pin;
    const gpio_num_t miso = (gpio_num_t)handle->miso->pin;

    const gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << cs) | (1ULL << sck) | (1ULL << mosi),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    const gpio_config_t in_cfg = {
        .pin_bit_mask = 1ULL << miso,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&out_cfg);
    gpio_config(&in_cfg);

    gpio_set_level(cs, 1);
    gpio_set_level(sck, 0);
    gpio_set_level(mosi, 0);
    furi_delay_ms(5);

    /* 1) Does CS reach the chip, and is the chip powered? The ST25R3916 turns
     * its MISO pull-down on (IO_CONF2 miso_pd1/pd2, default state) only while
     * SS is high, i.e. while the device is NOT selected. If pulling CS low
     * releases that pull-down and raising it brings it back, then the chip has
     * supply and it really does see our chip select -- that part of the wiring
     * (and the chip's IO ring) is proven good. Repeated 3x so a single glitch
     * cannot fake it. */
    for(int i = 1; i <= 3; i++) {
        gpio_set_level(cs, 1);
        furi_delay_ms(3);
        const int cs_high = gpio_get_level(miso);
        gpio_set_level(cs, 0);
        furi_delay_ms(3);
        const int cs_low = gpio_get_level(miso);
        gpio_set_level(cs, 1);
        furi_delay_ms(3);
        const int cs_high_again = gpio_get_level(miso);
        FURI_LOG_I(
            TAG,
            "bb cs#%d: cs_high=%d cs_low=%d again_high=%d (0,1,0 = chip saw CS)",
            i,
            cs_high,
            cs_low,
            cs_high_again);
    }

    /* 2) IC_IDENTITY (0x3F) read at ~5 kHz, both clock polarities, then once
     * more with MISO left completely floating: a chip that drives the line
     * cannot be confused with an MCU pull-up this way. */
    const uint8_t mode1 = nfc_bb_read_reg(cs, sck, mosi, miso, 0x7F, false);
    const uint8_t mode3 = nfc_bb_read_reg(cs, sck, mosi, miso, 0x7F, true);
    gpio_set_pull_mode(miso, GPIO_FLOATING);
    const uint8_t nopull = nfc_bb_read_reg(cs, sck, mosi, miso, 0x7F, false);
    gpio_set_pull_mode(miso, GPIO_PULLUP_ONLY);

    FURI_LOG_I(
        TAG,
        "bb probe: IC_IDENTITY mode1=0x%02X mode3=0x%02X nopull=0x%02X (want 0x2A)",
        mode1,
        mode3,
        nopull);

    /* Leave the bus in the same state furi_hal_init_early() created. */
    gpio_set_level(sck, 0);
    gpio_set_level(mosi, 0);
    gpio_set_level(cs, 1);
}
#endif /* BOARD_NFC_BITBANG_PROBE */

/* Bring-up helper: talk to the ST25R3916 as the very first thing in
 * furi_hal_init(), before the SD card and the LCD ever clock SPI2.
 *
 * The chip has no reset line on this board and keeps its state across an MCU
 * reset, so "does not answer" can mean either (a) the front-end is already in
 * a bad state at power-up, or (b) something in our own boot sequence put it
 * there. A probe that runs first separates the two.
 *
 * Note: an idle MISO on this chip reads low by design (IO_CONF2 miso_pd1/2
 * pull-downs), so a low MISO says nothing about the chip being alive. The only
 * reliable liveness check is IC_IDENTITY. */
void furi_hal_nfc_early_probe(void) {
    const FuriHalSpiBusHandle* handle = &furi_hal_spi_bus_handle_nfc;

    /* All select lines were driven high by furi_hal_init_early(). */
    FURI_LOG_I(
        TAG,
        "early probe: CS=%u IRQ=%u (all select lines deselected)",
        (unsigned)gpio_nfc_cs.pin,
        (unsigned)gpio_nfc_irq.pin);

    /* Supply check on IRQ. The ST25R3916 drives IRQ push-pull as soon as it is
     * supplied, so a line that just follows the internal pull means there is no
     * supply on the front-end (or the pin is not connected) -- something no
     * amount of SPI work can fix. Both levels identical = the line is driven. */
    const gpio_num_t irq = (gpio_num_t)gpio_nfc_irq.pin;
    gpio_set_direction(irq, GPIO_MODE_INPUT);
    gpio_set_pull_mode(irq, GPIO_PULLUP_ONLY);
    furi_delay_ms(2);
    const int irq_pullup = gpio_get_level(irq);
    gpio_set_pull_mode(irq, GPIO_PULLDOWN_ONLY);
    furi_delay_ms(2);
    const int irq_pulldown = gpio_get_level(irq);
    gpio_set_pull_mode(irq, GPIO_PULLUP_ONLY);
    FURI_LOG_I(TAG, "early probe: IRQ pullup=%d pulldown=%d", irq_pullup, irq_pulldown);
    if(irq_pullup != irq_pulldown) {
        FURI_LOG_E(TAG, "early probe: IRQ follows the pull -> front-end not supplied");
    }
#ifdef BOARD_NFC_IRQ_ADC_PROBE
    furi_hal_nfc_irq_adc_probe();
#endif

    /* Isolate the ESP-IDF SPI driver from the question "does the chip answer
     * at all": one slow, hand-clocked read before the bus is initialised. */
#ifdef BOARD_NFC_BITBANG_PROBE
    nfc_bb_probe();
#endif

    /* acquire() adds the SPI device (if it is not there yet), brings the bus
     * up and keeps the system out of light sleep for the duration. */
    furi_hal_spi_acquire(handle);

    st25r3916_direct_cmd(handle, ST25R3916_CMD_SET_DEFAULT);
    furi_delay_ms(5);

    bool responsive = false;
    uint8_t last_id = 0;
    for(int i = 0; i < 3; i++) {
        uint8_t id = 0;
        st25r3916_read_reg(handle, ST25R3916_REG_IC_IDENTITY, &id);
        last_id = id;
        if((id & ST25R3916_REG_IC_IDENTITY_ic_type_mask) ==
           ST25R3916_REG_IC_IDENTITY_ic_type_st25r3916) {
            responsive = true;
        }
        FURI_LOG_I(TAG, "early id[%d]=0x%02X", i, id);
    }
    if(!responsive) {
        FURI_LOG_E(TAG, "early probe: unusable IC_IDENTITY (last=0x%02X)", last_id);
#ifdef BOARD_NFC_EXEC_PROBE
        /* The chip is not answering on MISO. Ask the one question that does
         * not need MISO: is it executing our commands at all? */
        furi_hal_nfc_exec_probe(handle);
#endif
    }

    /* The handle stays registered on purpose: a registered SPI device keeps
     * its CS driven high between transactions, while remove_device() would
     * hand the line over to a ~45k pull-up right before the SD card and the
     * display start clocking the same bus at 10-40 MHz. */
    furi_hal_spi_release(handle);
}
#endif

static FuriHalNfcError furi_hal_nfc_init_impl(void);

FuriHalNfcError furi_hal_nfc_is_hal_ready(void) {
    FuriHalNfcError error = FuriHalNfcErrorNone;

    /* The boot-time bring-up happens early, while the SD card, the display and
     * the radios are still settling on the shared SPI2 bus, and it can fail
     * without the front-end being broken (the chip keeps its state across MCU
     * resets and may be found asleep). The app thread is a much quieter moment
     * to try again, and the only way to recover without a reboot: there is no
     * deinit/re-init path, because furi_hal_nfc_init() is a one-shot. */
    if(!furi_hal_nfc.ready) {
        if(!furi_hal_nfc.mutex) {
            furi_hal_nfc.mutex = furi_mutex_alloc(FuriMutexTypeNormal);
        }
        FURI_LOG_I(TAG, "is_hal_ready: HAL not configured yet, retrying init");
        error = furi_hal_nfc_init_impl();
        furi_hal_nfc.ready = (error == FuriHalNfcErrorNone);
        if(!furi_hal_nfc.ready) {
            FURI_LOG_E(TAG, "is_hal_ready: retry failed, chip still not usable");
            return error;
        }
    }

    do {
        error = furi_hal_nfc_acquire();
        if(error != FuriHalNfcErrorNone) break;

        const FuriHalSpiBusHandle* handle = &furi_hal_spi_bus_handle_nfc;
        uint8_t chip_id = 0;
        bool id_ok = furi_hal_nfc_read_chip_id(handle, &chip_id);
        FURI_LOG_I(
            TAG,
            "is_hal_ready: IC_IDENTITY=0x%02X (want mask 0x%02X = 0x%02X)",
            chip_id,
            ST25R3916_REG_IC_IDENTITY_ic_type_mask,
            ST25R3916_REG_IC_IDENTITY_ic_type_st25r3916);
        if(!id_ok) {
            FURI_LOG_E(TAG, "Wrong chip id");
            error = FuriHalNfcErrorCommunication;
        }

        furi_hal_nfc_release();
    } while(false);

    return error;
}

/* The actual bring-up. Split out of furi_hal_nfc_init() so that
 * furi_hal_nfc_is_hal_ready() can retry it on the app thread; the mutex and
 * the event object are created once and reused. */
static FuriHalNfcError furi_hal_nfc_init_impl(void) {
    FuriHalNfcError error = FuriHalNfcErrorNone;

    furi_hal_nfc_event_init();
    furi_hal_nfc_event_start();
    /* The timers are needed by every poller transaction and live for the whole
     * session. Creating them here (instead of only in low_power_mode_stop())
     * means the first transaction cannot find them missing. */
    furi_hal_nfc_timers_init();

    do {
        error = furi_hal_nfc_acquire();
        if(error != FuriHalNfcErrorNone) {
            furi_hal_nfc_low_power_mode_start();
        }

        const FuriHalSpiBusHandle* handle = &furi_hal_spi_bus_handle_nfc;
        /* The device is normally added by the early probe. Re-check it here as
         * well so a build without BOARD_NFC_EARLY_PROBE still works, but do
         * not tear the handle down and re-add it: on a shared bus that hands
         * CS to a weak pull-up for a moment while the LCD and the SD card keep
         * clocking. */
        furi_hal_spi_bus_handle_init(handle);
        if(!handle->initialized) {
            FURI_LOG_E(TAG, "init: NFC SPI device did not initialise");
            error = FuriHalNfcErrorCommunication;
            furi_hal_nfc_low_power_mode_start();
            furi_hal_nfc_release();
            break;
        }
        /* Set default state, start the oscillator and check the chip type */
#ifdef BOARD_NFC_CHIP_ID_SWEEP
        furi_hal_nfc_chip_id_sweep();
#endif
        /* Make sure the front-end answers before touching anything else.
         *
         * This board has no NFC reset line, so the ST25R3916 comes up in
         * whatever state the previous run left it in. One such state is
         * "everything reads back 0x00", including the read-only IC_IDENTITY,
         * with writes apparently lost; it was hit on this board after a
         * bring-up build and it survives MCU resets and reflashes. The wake
         * sweep showed that a single plain write of OP_CONTROL.en plus a
         * settle delay brings the chip back at any clock from 4 MHz down to
         * 100 kHz, so this is register state, not SPI timing.
         *
         * furi_hal_nfc_read_chip_id() does exactly that and costs one register
         * read when the chip is healthy, so recovery does not slow down or
         * disturb a normal boot. Doing it here also matters because
         * identify_chip() starts with SET_DEFAULT and a read-modify-write on
         * IO_CONF2: on a wedged chip those are lost too, every retry repeats
         * them, and the boot ends with the front-end dead. */
        uint8_t pre_op = 0;
        uint8_t pre_id = 0;
        st25r3916_read_reg(handle, ST25R3916_REG_OP_CONTROL, &pre_op);
        if(furi_hal_nfc_read_chip_id(handle, &pre_id)) {
            FURI_LOG_I(
                TAG,
                "init: chip found: OP_CONTROL=0x%02X IC_IDENTITY=0x%02X",
                pre_op,
                pre_id);
        } else {
            FURI_LOG_W(
                TAG,
                "init: front-end did not answer: OP_CONTROL=0x%02X IC_IDENTITY=0x%02X",
                pre_op,
                pre_id);
        }

        uint8_t chip_id = 0;
        const bool id_ok = furi_hal_nfc_identify_chip(handle, &chip_id);
        FURI_LOG_I(
            TAG,
            "init: IC_IDENTITY=0x%02X (want mask 0x%02X = 0x%02X)",
            chip_id,
            ST25R3916_REG_IC_IDENTITY_ic_type_mask,
            ST25R3916_REG_IC_IDENTITY_ic_type_st25r3916);
        if(!id_ok) {
            FURI_LOG_E(TAG, "Wrong chip id");
#ifdef BOARD_NFC_EXEC_PROBE
            furi_hal_nfc_exec_probe(handle);
#endif
            error = FuriHalNfcErrorCommunication;
            furi_hal_nfc_deinit_keep_osc(handle);
            furi_hal_nfc_release();
            break;
        }
        // Clear interrupts
        st25r3916_get_irq(handle);
        // Mask all interrupts
        st25r3916_mask_irq(handle, ST25R3916_IRQ_MASK_ALL);
        // Enable interrupts
        furi_hal_nfc_init_gpio_isr();
        // Disable internal overheat protection
        st25r3916_change_test_reg_bits(handle, 0x04, 0x10, 0x10);

        error = furi_hal_nfc_turn_on_osc(handle);
        if(error != FuriHalNfcErrorNone) {
            furi_hal_nfc_deinit_keep_osc(handle);
            furi_hal_nfc_release();
            break;
        }

        // Measure voltage
        // Set measure power supply voltage source
        st25r3916_change_reg_bits(
            handle,
            ST25R3916_REG_REGULATOR_CONTROL,
            ST25R3916_REG_REGULATOR_CONTROL_mpsv_mask,
            ST25R3916_REG_REGULATOR_CONTROL_mpsv_vdd);
        // Enable timer and interrupt register
        st25r3916_mask_irq(handle, ~ST25R3916_IRQ_MASK_DCT);
        st25r3916_direct_cmd(handle, ST25R3916_CMD_MEASURE_VDD);
        furi_hal_nfc_event_wait_for_specific_irq(handle, ST25R3916_IRQ_MASK_DCT, 100);
        st25r3916_mask_irq(handle, ST25R3916_IRQ_MASK_ALL);
        uint8_t ad_res = 0;
        st25r3916_read_reg(handle, ST25R3916_REG_AD_RESULT, &ad_res);
        uint16_t mV = ((uint16_t)ad_res) * 23U;
        mV += (((((uint16_t)ad_res) * 4U) + 5U) / 10U);
        FURI_LOG_I(TAG, "init: measured VDD=%umV", (unsigned)mV);

        if(mV < 3600) {
            st25r3916_change_reg_bits(
                handle,
                ST25R3916_REG_IO_CONF2,
                ST25R3916_REG_IO_CONF2_sup3V,
                ST25R3916_REG_IO_CONF2_sup3V_3V);
        } else {
            st25r3916_change_reg_bits(
                handle,
                ST25R3916_REG_IO_CONF2,
                ST25R3916_REG_IO_CONF2_sup3V,
                ST25R3916_REG_IO_CONF2_sup3V_5V);
        }

        // Disable MCU CLK
        st25r3916_change_reg_bits(
            handle,
            ST25R3916_REG_IO_CONF1,
            ST25R3916_REG_IO_CONF1_out_cl_mask | ST25R3916_REG_IO_CONF1_lf_clk_off,
            0x07);
        // Disable MISO pull-down
        st25r3916_change_reg_bits(
            handle,
            ST25R3916_REG_IO_CONF2,
            ST25R3916_REG_IO_CONF2_miso_pd1 | ST25R3916_REG_IO_CONF2_miso_pd2,
            0x00);
        // Set tx driver resistance to 1 Om
        st25r3916_change_reg_bits(
            handle, ST25R3916_REG_TX_DRIVER, ST25R3916_REG_TX_DRIVER_d_res_mask, 0x00);
        // Use minimum non-overlap
        st25r3916_change_reg_bits(
            handle,
            ST25R3916_REG_RES_AM_MOD,
            ST25R3916_REG_RES_AM_MOD_fa3_f,
            ST25R3916_REG_RES_AM_MOD_fa3_f);

        // Set activation threashold
        st25r3916_change_reg_bits(
            handle,
            ST25R3916_REG_FIELD_THRESHOLD_ACTV,
            ST25R3916_REG_FIELD_THRESHOLD_ACTV_trg_mask,
            ST25R3916_REG_FIELD_THRESHOLD_ACTV_trg_105mV);
        st25r3916_change_reg_bits(
            handle,
            ST25R3916_REG_FIELD_THRESHOLD_ACTV,
            ST25R3916_REG_FIELD_THRESHOLD_ACTV_rfe_mask,
            ST25R3916_REG_FIELD_THRESHOLD_ACTV_rfe_105mV);
        // Set deactivation threashold
        st25r3916_change_reg_bits(
            handle,
            ST25R3916_REG_FIELD_THRESHOLD_DEACTV,
            ST25R3916_REG_FIELD_THRESHOLD_DEACTV_trg_mask,
            ST25R3916_REG_FIELD_THRESHOLD_DEACTV_trg_75mV);
        st25r3916_change_reg_bits(
            handle,
            ST25R3916_REG_FIELD_THRESHOLD_DEACTV,
            ST25R3916_REG_FIELD_THRESHOLD_DEACTV_rfe_mask,
            ST25R3916_REG_FIELD_THRESHOLD_DEACTV_rfe_75mV);
        // Enable external load modulation
        st25r3916_change_reg_bits(
            handle,
            ST25R3916_REG_AUX_MOD,
            ST25R3916_REG_AUX_MOD_lm_ext,
            ST25R3916_REG_AUX_MOD_lm_ext);
        // Enable internal load modulation
        st25r3916_change_reg_bits(
            handle,
            ST25R3916_REG_AUX_MOD,
            ST25R3916_REG_AUX_MOD_lm_dri,
            ST25R3916_REG_AUX_MOD_lm_dri);
        // Adjust FDT
        st25r3916_change_reg_bits(
            handle,
            ST25R3916_REG_PASSIVE_TARGET,
            ST25R3916_REG_PASSIVE_TARGET_fdel_mask,
            (5U << ST25R3916_REG_PASSIVE_TARGET_fdel_shift));
        // Reduce RFO resistance in Modulated state
        st25r3916_change_reg_bits(
            handle,
            ST25R3916_REG_PT_MOD,
            ST25R3916_REG_PT_MOD_ptm_res_mask | ST25R3916_REG_PT_MOD_pt_res_mask,
            0x0f);
        // Enable RX start on first 4 bits
        st25r3916_change_reg_bits(
            handle,
            ST25R3916_REG_EMD_SUP_CONF,
            ST25R3916_REG_EMD_SUP_CONF_rx_start_emv,
            ST25R3916_REG_EMD_SUP_CONF_rx_start_emv_on);
        // Set antena tunning
        st25r3916_change_reg_bits(handle, ST25R3916_REG_ANT_TUNE_A, 0xff, 0x82);
        st25r3916_change_reg_bits(handle, ST25R3916_REG_ANT_TUNE_B, 0xff, 0x82);
        st25r3916_change_reg_bits(
            handle,
            ST25R3916_REG_OP_CONTROL,
            ST25R3916_REG_OP_CONTROL_en_fd_mask,
            ST25R3916_REG_OP_CONTROL_en_fd_auto_efd);

        // Perform calibration
        if(st25r3916_check_reg(
               handle,
               ST25R3916_REG_REGULATOR_CONTROL,
               ST25R3916_REG_REGULATOR_CONTROL_reg_s,
               0x00)) {
            FURI_LOG_I(TAG, "Adjusting regulators");
            // Reset logic
            st25r3916_set_reg_bits(
                handle, ST25R3916_REG_REGULATOR_CONTROL, ST25R3916_REG_REGULATOR_CONTROL_reg_s);
            st25r3916_clear_reg_bits(
                handle, ST25R3916_REG_REGULATOR_CONTROL, ST25R3916_REG_REGULATOR_CONTROL_reg_s);
            st25r3916_direct_cmd(handle, ST25R3916_CMD_ADJUST_REGULATORS);
            furi_delay_ms(6);
        }

        furi_hal_nfc_low_power_mode_start();
        furi_hal_nfc_release();
    } while(false);

    return error;
}

FuriHalNfcError furi_hal_nfc_init(void) {
    furi_check(furi_hal_nfc.mutex == NULL);

    furi_hal_nfc.mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    FuriHalNfcError error = furi_hal_nfc_init_impl();
    furi_hal_nfc.ready = (error == FuriHalNfcErrorNone);
    FURI_LOG_I(
        TAG, "init: front-end %s", furi_hal_nfc.ready ? "configured" : "NOT configured");

    return error;
}

static bool furi_hal_nfc_is_mine(void) {
    return furi_mutex_get_owner(furi_hal_nfc.mutex) == furi_thread_get_current_id();
}

FuriHalNfcError furi_hal_nfc_acquire(void) {
    furi_check(furi_hal_nfc.mutex);

    /* Lock the NFC owner first, then the bus: the bus is held for as long as
     * the owner keeps the front-end, so taking it while another thread may
     * still own the chip would block the whole shared bus for the duration of
     * the timeout. */
    FuriHalNfcError error = FuriHalNfcErrorNone;
    if(furi_mutex_acquire(furi_hal_nfc.mutex, 100) != FuriStatusOk) {
        error = FuriHalNfcErrorBusy;
    } else {
        furi_hal_spi_acquire(&furi_hal_spi_bus_handle_nfc);
    }

    return error;
}

FuriHalNfcError furi_hal_nfc_release(void) {
    furi_check(furi_hal_nfc.mutex);
    furi_check(furi_hal_nfc_is_mine());

    furi_hal_spi_release(&furi_hal_spi_bus_handle_nfc);
    furi_check(furi_mutex_release(furi_hal_nfc.mutex) == FuriStatusOk);

    return FuriHalNfcErrorNone;
}

FuriHalNfcError furi_hal_nfc_low_power_mode_start(void) {
    FuriHalNfcError error = FuriHalNfcErrorNone;
    const FuriHalSpiBusHandle* handle = &furi_hal_spi_bus_handle_nfc;

    st25r3916_direct_cmd(handle, ST25R3916_CMD_STOP);
    st25r3916_clear_reg_bits(
        handle,
        ST25R3916_REG_OP_CONTROL,
        (ST25R3916_REG_OP_CONTROL_en | ST25R3916_REG_OP_CONTROL_rx_en |
         ST25R3916_REG_OP_CONTROL_wu | ST25R3916_REG_OP_CONTROL_tx_en |
         ST25R3916_REG_OP_CONTROL_en_fd_mask));
    furi_hal_nfc_deinit_gpio_isr();
    furi_hal_nfc_timers_stop();
    furi_hal_nfc_event_stop();

    return error;
}

FuriHalNfcError furi_hal_nfc_low_power_mode_stop(void) {
    FuriHalNfcError error = FuriHalNfcErrorNone;
    const FuriHalSpiBusHandle* handle = &furi_hal_spi_bus_handle_nfc;

    do {
        furi_hal_nfc_init_gpio_isr();
        furi_hal_nfc_timers_init();
        /* Rebind the event thread: leaving low power always means the caller
         * is about to wait for chip events, and low_power_mode_start() above
         * cleared the binding. Without this the next wait aborts on its
         * furi_check(thread), or silently never wakes up. */
        furi_hal_nfc_event_start();
        error = furi_hal_nfc_turn_on_osc(handle);
        if(error != FuriHalNfcErrorNone) break;
        st25r3916_change_reg_bits(
            handle,
            ST25R3916_REG_OP_CONTROL,
            ST25R3916_REG_OP_CONTROL_en_fd_mask,
            ST25R3916_REG_OP_CONTROL_en_fd_auto_efd);

    } while(false);

    return error;
}

static FuriHalNfcError furi_hal_nfc_poller_init_common(const FuriHalSpiBusHandle* handle) {
    // Disable wake up
    st25r3916_clear_reg_bits(handle, ST25R3916_REG_OP_CONTROL, ST25R3916_REG_OP_CONTROL_wu);
    // Enable correlator
    st25r3916_change_reg_bits(
        handle,
        ST25R3916_REG_AUX,
        ST25R3916_REG_AUX_dis_corr,
        ST25R3916_REG_AUX_dis_corr_correlator);

    st25r3916_change_reg_bits(handle, ST25R3916_REG_ANT_TUNE_A, 0xff, 0x82);
    st25r3916_change_reg_bits(handle, ST25R3916_REG_ANT_TUNE_B, 0xFF, 0x82);

    st25r3916_write_reg(handle, ST25R3916_REG_OVERSHOOT_CONF1, 0x00);
    st25r3916_write_reg(handle, ST25R3916_REG_OVERSHOOT_CONF2, 0x00);
    st25r3916_write_reg(handle, ST25R3916_REG_UNDERSHOOT_CONF1, 0x00);
    st25r3916_write_reg(handle, ST25R3916_REG_UNDERSHOOT_CONF2, 0x00);

    return FuriHalNfcErrorNone;
}

static FuriHalNfcError furi_hal_nfc_listener_init_common(const FuriHalSpiBusHandle* handle) {
    UNUSED(handle);
    // No common listener configuration
    return FuriHalNfcErrorNone;
}

FuriHalNfcError furi_hal_nfc_set_mode(FuriHalNfcMode mode, FuriHalNfcTech tech) {
    furi_check(mode < FuriHalNfcModeNum);
    furi_check(tech < FuriHalNfcTechNum);

    const FuriHalSpiBusHandle* handle = &furi_hal_spi_bus_handle_nfc;

    FuriHalNfcError error = FuriHalNfcErrorNone;

    if(mode == FuriHalNfcModePoller) {
        do {
            error = furi_hal_nfc_poller_init_common(handle);
            if(error != FuriHalNfcErrorNone) break;
            error = furi_hal_nfc_tech[tech]->poller.init(handle);
        } while(false);

    } else if(mode == FuriHalNfcModeListener) {
        do {
            error = furi_hal_nfc_listener_init_common(handle);
            if(error != FuriHalNfcErrorNone) break;
            error = furi_hal_nfc_tech[tech]->listener.init(handle);
        } while(false);
    }

    furi_hal_nfc.mode = mode;
    furi_hal_nfc.tech = tech;
    return error;
}

FuriHalNfcError furi_hal_nfc_reset_mode(void) {
    FuriHalNfcError error = FuriHalNfcErrorNone;
    const FuriHalSpiBusHandle* handle = &furi_hal_spi_bus_handle_nfc;

    st25r3916_direct_cmd(handle, ST25R3916_CMD_STOP);

    const FuriHalNfcMode mode = furi_hal_nfc.mode;
    const FuriHalNfcTech tech = furi_hal_nfc.tech;
    if(mode == FuriHalNfcModePoller) {
        error = furi_hal_nfc_tech[tech]->poller.deinit(handle);
    } else if(mode == FuriHalNfcModeListener) {
        error = furi_hal_nfc_tech[tech]->listener.deinit(handle);
    }
    // Set default value in mode register
    st25r3916_write_reg(handle, ST25R3916_REG_MODE, ST25R3916_REG_MODE_om0);
    st25r3916_write_reg(handle, ST25R3916_REG_STREAM_MODE, 0);
    st25r3916_clear_reg_bits(handle, ST25R3916_REG_AUX, ST25R3916_REG_AUX_no_crc_rx);
    st25r3916_clear_reg_bits(
        handle,
        ST25R3916_REG_BIT_RATE,
        ST25R3916_REG_BIT_RATE_txrate_mask | ST25R3916_REG_BIT_RATE_rxrate_mask);

    // Write default values
    st25r3916_write_reg(handle, ST25R3916_REG_RX_CONF1, 0);
    st25r3916_write_reg(
        handle,
        ST25R3916_REG_RX_CONF2,
        ST25R3916_REG_RX_CONF2_sqm_dyn | ST25R3916_REG_RX_CONF2_agc_en |
            ST25R3916_REG_RX_CONF2_agc_m);

    st25r3916_write_reg(
        handle,
        ST25R3916_REG_CORR_CONF1,
        ST25R3916_REG_CORR_CONF1_corr_s7 | ST25R3916_REG_CORR_CONF1_corr_s4 |
            ST25R3916_REG_CORR_CONF1_corr_s1 | ST25R3916_REG_CORR_CONF1_corr_s0);
    st25r3916_write_reg(handle, ST25R3916_REG_CORR_CONF2, 0);

    return error;
}

FuriHalNfcError furi_hal_nfc_field_detect_start(void) {
    FuriHalNfcError error = FuriHalNfcErrorNone;
    const FuriHalSpiBusHandle* handle = &furi_hal_spi_bus_handle_nfc;

    st25r3916_write_reg(
        handle,
        ST25R3916_REG_OP_CONTROL,
        ST25R3916_REG_OP_CONTROL_en | ST25R3916_REG_OP_CONTROL_en_fd_mask);
    st25r3916_write_reg(
        handle, ST25R3916_REG_MODE, ST25R3916_REG_MODE_targ | ST25R3916_REG_MODE_om0);

    return error;
}

FuriHalNfcError furi_hal_nfc_field_detect_stop(void) {
    FuriHalNfcError error = FuriHalNfcErrorNone;
    const FuriHalSpiBusHandle* handle = &furi_hal_spi_bus_handle_nfc;

    st25r3916_clear_reg_bits(
        handle,
        ST25R3916_REG_OP_CONTROL,
        (ST25R3916_REG_OP_CONTROL_en | ST25R3916_REG_OP_CONTROL_en_fd_mask));

    return error;
}

bool furi_hal_nfc_field_is_present(void) {
    bool is_present = false;
    const FuriHalSpiBusHandle* handle = &furi_hal_spi_bus_handle_nfc;

    if(st25r3916_check_reg(
           handle,
           ST25R3916_REG_AUX_DISPLAY,
           ST25R3916_REG_AUX_DISPLAY_efd_o,
           ST25R3916_REG_AUX_DISPLAY_efd_o)) {
        is_present = true;
    }

    return is_present;
}

FuriHalNfcError furi_hal_nfc_poller_field_on(void) {
    FuriHalNfcError error = FuriHalNfcErrorNone;
    const FuriHalSpiBusHandle* handle = &furi_hal_spi_bus_handle_nfc;

    if(!st25r3916_check_reg(
           handle,
           ST25R3916_REG_OP_CONTROL,
           ST25R3916_REG_OP_CONTROL_tx_en,
           ST25R3916_REG_OP_CONTROL_tx_en)) {
        // Set min guard time
        st25r3916_write_reg(handle, ST25R3916_REG_FIELD_ON_GT, 0);
        // Enable tx rx
        st25r3916_set_reg_bits(
            handle,
            ST25R3916_REG_OP_CONTROL,
            (ST25R3916_REG_OP_CONTROL_rx_en | ST25R3916_REG_OP_CONTROL_tx_en));
    }

    return error;
}

FuriHalNfcError furi_hal_nfc_poller_tx_common(
    const FuriHalSpiBusHandle* handle,
    const uint8_t* tx_data,
    size_t tx_bits) {
    furi_check(tx_data);

    FuriHalNfcError err = FuriHalNfcErrorNone;

    // Prepare tx
    st25r3916_direct_cmd(handle, ST25R3916_CMD_CLEAR_FIFO);
    st25r3916_clear_reg_bits(
        handle, ST25R3916_REG_TIMER_EMV_CONTROL, ST25R3916_REG_TIMER_EMV_CONTROL_nrt_emv);
    st25r3916_change_reg_bits(
        handle,
        ST25R3916_REG_ISO14443A_NFC,
        (ST25R3916_REG_ISO14443A_NFC_no_tx_par | ST25R3916_REG_ISO14443A_NFC_no_rx_par),
        (ST25R3916_REG_ISO14443A_NFC_no_tx_par_off | ST25R3916_REG_ISO14443A_NFC_no_rx_par_off));
    uint32_t interrupts =
        (ST25R3916_IRQ_MASK_FWL | ST25R3916_IRQ_MASK_TXE | ST25R3916_IRQ_MASK_RXS |
         ST25R3916_IRQ_MASK_RXE | ST25R3916_IRQ_MASK_PAR | ST25R3916_IRQ_MASK_CRC |
         ST25R3916_IRQ_MASK_ERR1 | ST25R3916_IRQ_MASK_ERR2 | ST25R3916_IRQ_MASK_NRE);
    // Clear interrupts
    st25r3916_get_irq(handle);
    // Enable interrupts
    st25r3916_mask_irq(handle, ~interrupts);

    st25r3916_write_fifo(handle, tx_data, tx_bits);
    st25r3916_direct_cmd(handle, ST25R3916_CMD_TRANSMIT_WITHOUT_CRC);

    return err;
}

FuriHalNfcError furi_hal_nfc_common_fifo_tx(
    const FuriHalSpiBusHandle* handle,
    const uint8_t* tx_data,
    size_t tx_bits) {
    FuriHalNfcError err = FuriHalNfcErrorNone;

    st25r3916_direct_cmd(handle, ST25R3916_CMD_CLEAR_FIFO);
    st25r3916_write_fifo(handle, tx_data, tx_bits);
    st25r3916_direct_cmd(handle, ST25R3916_CMD_TRANSMIT_WITHOUT_CRC);

    return err;
}

FuriHalNfcError furi_hal_nfc_poller_tx(const uint8_t* tx_data, size_t tx_bits) {
    furi_check(furi_hal_nfc.mode == FuriHalNfcModePoller);
    furi_check(furi_hal_nfc.tech < FuriHalNfcTechNum);
    const FuriHalSpiBusHandle* handle = &furi_hal_spi_bus_handle_nfc;

    return furi_hal_nfc_tech[furi_hal_nfc.tech]->poller.tx(handle, tx_data, tx_bits);
}

FuriHalNfcError furi_hal_nfc_poller_rx(uint8_t* rx_data, size_t rx_data_size, size_t* rx_bits) {
    furi_check(furi_hal_nfc.mode == FuriHalNfcModePoller);
    furi_check(furi_hal_nfc.tech < FuriHalNfcTechNum);
    const FuriHalSpiBusHandle* handle = &furi_hal_spi_bus_handle_nfc;

    return furi_hal_nfc_tech[furi_hal_nfc.tech]->poller.rx(handle, rx_data, rx_data_size, rx_bits);
}

FuriHalNfcEvent furi_hal_nfc_poller_wait_event(uint32_t timeout_ms) {
    furi_check(furi_hal_nfc.mode == FuriHalNfcModePoller);
    furi_check(furi_hal_nfc.tech < FuriHalNfcTechNum);

    return furi_hal_nfc_tech[furi_hal_nfc.tech]->poller.wait_event(timeout_ms);
}

FuriHalNfcEvent furi_hal_nfc_listener_wait_event(uint32_t timeout_ms) {
    furi_check(furi_hal_nfc.mode == FuriHalNfcModeListener);
    furi_check(furi_hal_nfc.tech < FuriHalNfcTechNum);

    return furi_hal_nfc_tech[furi_hal_nfc.tech]->listener.wait_event(timeout_ms);
}

FuriHalNfcError furi_hal_nfc_listener_tx(const uint8_t* tx_data, size_t tx_bits) {
    furi_check(tx_data);

    furi_check(furi_hal_nfc.mode == FuriHalNfcModeListener);
    furi_check(furi_hal_nfc.tech < FuriHalNfcTechNum);

    const FuriHalSpiBusHandle* handle = &furi_hal_spi_bus_handle_nfc;
    return furi_hal_nfc_tech[furi_hal_nfc.tech]->listener.tx(handle, tx_data, tx_bits);
}

FuriHalNfcError furi_hal_nfc_common_fifo_rx(
    const FuriHalSpiBusHandle* handle,
    uint8_t* rx_data,
    size_t rx_data_size,
    size_t* rx_bits) {
    FuriHalNfcError error = FuriHalNfcErrorNone;

    if(!st25r3916_read_fifo(handle, rx_data, rx_data_size, rx_bits)) {
        error = FuriHalNfcErrorBufferOverflow;
    }

    return error;
}

FuriHalNfcError furi_hal_nfc_listener_rx(uint8_t* rx_data, size_t rx_data_size, size_t* rx_bits) {
    furi_check(rx_data);
    furi_check(rx_bits);

    furi_check(furi_hal_nfc.mode == FuriHalNfcModeListener);
    furi_check(furi_hal_nfc.tech < FuriHalNfcTechNum);

    const FuriHalSpiBusHandle* handle = &furi_hal_spi_bus_handle_nfc;
    return furi_hal_nfc_tech[furi_hal_nfc.tech]->listener.rx(
        handle, rx_data, rx_data_size, rx_bits);
}

FuriHalNfcError furi_hal_nfc_trx_reset(void) {
    const FuriHalSpiBusHandle* handle = &furi_hal_spi_bus_handle_nfc;

    st25r3916_direct_cmd(handle, ST25R3916_CMD_STOP);

    return FuriHalNfcErrorNone;
}

FuriHalNfcError furi_hal_nfc_listener_sleep(void) {
    furi_check(furi_hal_nfc.mode == FuriHalNfcModeListener);
    furi_check(furi_hal_nfc.tech < FuriHalNfcTechNum);

    const FuriHalSpiBusHandle* handle = &furi_hal_spi_bus_handle_nfc;

    return furi_hal_nfc_tech[furi_hal_nfc.tech]->listener.sleep(handle);
}

FuriHalNfcError furi_hal_nfc_listener_idle(void) {
    furi_check(furi_hal_nfc.mode == FuriHalNfcModeListener);
    furi_check(furi_hal_nfc.tech < FuriHalNfcTechNum);

    const FuriHalSpiBusHandle* handle = &furi_hal_spi_bus_handle_nfc;

    return furi_hal_nfc_tech[furi_hal_nfc.tech]->listener.idle(handle);
}

FuriHalNfcError furi_hal_nfc_listener_enable_rx(void) {
    const FuriHalSpiBusHandle* handle = &furi_hal_spi_bus_handle_nfc;

    st25r3916_direct_cmd(handle, ST25R3916_CMD_UNMASK_RECEIVE_DATA);

    return FuriHalNfcErrorNone;
}

/* ------------------------------------------------------------------------- */
/* Compatibility stubs for API extensions introduced by the ESP32/PN532 port */
/* ------------------------------------------------------------------------- */

void furi_hal_nfc_emu_set_ndef(const uint8_t* msg, size_t len) {
    UNUSED(msg);
    UNUSED(len);
    /* Bruce-style NDEF emulation is a PN532 backend feature; the ST25R3916
     * backend uses the raw passive-target flow. */
}

FuriHalNfcError furi_hal_nfc_pn532_mf_auth(
    uint8_t block_num,
    const uint8_t* key,
    uint8_t key_type,
    const uint8_t* uid,
    uint8_t uid_len) {
    UNUSED(block_num);
    UNUSED(key);
    UNUSED(key_type);
    UNUSED(uid);
    UNUSED(uid_len);
    return FuriHalNfcErrorCommunication;
}

bool furi_hal_nfc_pn532_mf_is_authed(void) {
    return false;
}

void furi_hal_nfc_pn532_mf_deauth(void) {
}

FuriHalNfcError furi_hal_nfc_iso15693_detect_mode(void) {
    return FuriHalNfcErrorCommunication;
}

FuriHalNfcError furi_hal_nfc_iso15693_force_1outof4(void) {
    return FuriHalNfcErrorCommunication;
}

FuriHalNfcError furi_hal_nfc_iso15693_force_1outof256(void) {
    return FuriHalNfcErrorCommunication;
}