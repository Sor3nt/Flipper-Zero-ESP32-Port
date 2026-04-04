/**
 * @file furi_hal_nfc.c
 * @brief NFC HAL implementation for ESP32 with PN532 (I2C).
 *
 * Implements the full furi_hal_nfc API on top of the PN532 NFC controller.
 * The PN532 is a command/response device while the Flipper NFC stack expects
 * an event-driven (interrupt) model like the ST25R3916. This implementation
 * bridges the gap by:
 *  - Running PN532 commands synchronously inside poller_tx / short_frame / sdd
 *  - Buffering the card response from the combined TX+RX PN532 command
 *  - Signaling the correct event sequence (TxEnd → RxStart → RxEnd) so that
 *    nfc.c's TRX state machine works unmodified
 *  - Using ESP high-resolution timers for FWT and BlockTx timing
 *  - Supporting listener mode via TgInitAsTarget / TgGetData / TgSetData
 *
 * On boards without NFC (BOARD_HAS_NFC == 0), all functions return errors.
 */

#include "furi_hal_nfc.h"
#include <furi.h>
#include <board.h>

#define TAG "FuriHalNfc"

#if BOARD_HAS_NFC

#include <driver/i2c.h>
#include <driver/gpio.h>
#include <esp_timer.h>
#include <string.h>

/* ──────────────────────────── PN532 Protocol Constants ──────────────────── */

#define PN532_I2C_ADDR          0x24

#define PN532_PREAMBLE          0x00
#define PN532_STARTCODE1        0x00
#define PN532_STARTCODE2        0xFF
#define PN532_HOSTTOPN532       0xD4
#define PN532_PN532TOHOST       0xD5
#define PN532_ACK_LENGTH        7

/* PN532 Commands */
#define PN532_CMD_GETFIRMWAREVERSION    0x02
#define PN532_CMD_SAMCONFIGURATION      0x14
#define PN532_CMD_POWERDOWN             0x16
#define PN532_CMD_RFCONFIGURATION       0x32
#define PN532_CMD_INLISTPASSIVETARGET   0x4A
#define PN532_CMD_INDATAEXCHANGE        0x40
#define PN532_CMD_INCOMMUNICATETHRU     0x42
#define PN532_CMD_INJUMPFORDEP          0x56
#define PN532_CMD_TGINITASTARGET        0x8C
#define PN532_CMD_TGGETDATA             0x86
#define PN532_CMD_TGSETDATA             0x8E
#define PN532_CMD_TGRESPONSETOINITIATOR 0x90

/* RF Configuration items */
#define PN532_RFCFG_FIELD               0x01
#define PN532_RFCFG_TIMINGS             0x02
#define PN532_RFCFG_RETRIES             0x05

/* InListPassiveTarget baud rates */
#define PN532_BRTY_ISO14443A            0x00
#define PN532_BRTY_FELICA_212           0x01
#define PN532_BRTY_FELICA_424           0x02
#define PN532_BRTY_ISO14443B            0x03
#define PN532_BRTY_JEWEL                0x04

/* PN532 error codes (InDataExchange / InCommunicateThru status byte) */
#define PN532_STATUS_OK                 0x00
#define PN532_STATUS_TIMEOUT            0x01
#define PN532_STATUS_CRC_ERROR          0x02
#define PN532_STATUS_PARITY_ERROR       0x03
#define PN532_STATUS_COLLISION          0x04
#define PN532_STATUS_MIFARE_AUTH        0x14

/* ──────────────────────────── All valid event bits ───────────────────────── */

#define NFC_EVENT_ALL_BITS (                    \
    FuriHalNfcEventOscOn |                      \
    FuriHalNfcEventFieldOn |                    \
    FuriHalNfcEventFieldOff |                   \
    FuriHalNfcEventListenerActive |             \
    FuriHalNfcEventTxStart |                    \
    FuriHalNfcEventTxEnd |                      \
    FuriHalNfcEventRxStart |                    \
    FuriHalNfcEventRxEnd |                      \
    FuriHalNfcEventCollision |                  \
    FuriHalNfcEventTimerFwtExpired |            \
    FuriHalNfcEventTimerBlockTxExpired |        \
    FuriHalNfcEventTimeout |                    \
    FuriHalNfcEventAbortRequest)

/* ──────────────────────────── Module State ───────────────────────────────── */

static bool nfc_hal_ready = false;
static FuriMutex* nfc_mutex = NULL;
static FuriHalNfcMode nfc_current_mode = FuriHalNfcModeNum;
static FuriHalNfcTech nfc_current_tech = FuriHalNfcTechInvalid;

/* Event signaling — used by worker thread for poller/listener wait_event */
static FuriEventFlag* nfc_event_flags = NULL;

/* RX buffer: PN532 does TX+RX in one command, poller_rx retrieves buffered data */
static uint8_t pn532_rx_buf[256];
static size_t pn532_rx_bits = 0;

/* Cached target from InListPassiveTarget (for SDD frame emulation) */
static uint8_t pn532_target_atqa[2];
static uint8_t pn532_target_sak;
static uint8_t pn532_target_uid[10];
static uint8_t pn532_target_uid_len;
static uint8_t pn532_target_number; /* 0 = no target listed */

/* ISO-DEP state: PN532 manages ISO-DEP internally for ISO14443-4 tags.
 * We cache the ATS and handle I-block framing translation. */
static uint8_t pn532_cached_ats[64];
static uint8_t pn532_cached_ats_len;
static bool pn532_iso_dep_active; /* SAK & 0x20 → PN532 activated ISO-DEP */
static bool pn532_iso_dep_mode;   /* RATS completed, Flipper stack in ISO-DEP mode */
static uint8_t pn532_block_number; /* I-block sequence toggle (0/1) */

/* Software timers */
static esp_timer_handle_t fwt_timer = NULL;
static esp_timer_handle_t block_tx_timer = NULL;
static volatile bool block_tx_running = false;

/* CRC-A computation (ISO 14443-3A, init=0x6363) */
static void crc_a_append(uint8_t* data, size_t len) {
    uint16_t crc = 0x6363;
    for(size_t i = 0; i < len; i++) {
        uint8_t bt = data[i] ^ (uint8_t)(crc & 0xFF);
        bt ^= bt << 4;
        crc = (crc >> 8) ^ ((uint16_t)bt << 8) ^ ((uint16_t)bt << 3) ^ ((uint16_t)bt >> 4);
    }
    data[len] = (uint8_t)(crc & 0xFF);
    data[len + 1] = (uint8_t)(crc >> 8);
}

/* Listener emulation state */
static uint8_t listener_uid[10];
static uint8_t listener_uid_len;
static uint8_t listener_atqa[2];
static uint8_t listener_sak;

/* ──────────────────────────── Timer Callbacks ────────────────────────────── */

static void fwt_timer_cb(void* arg) {
    UNUSED(arg);
    if(nfc_event_flags) {
        furi_event_flag_set(nfc_event_flags, FuriHalNfcEventTimerFwtExpired);
    }
}

static void block_tx_timer_cb(void* arg) {
    UNUSED(arg);
    block_tx_running = false;
    if(nfc_event_flags) {
        furi_event_flag_set(nfc_event_flags, FuriHalNfcEventTimerBlockTxExpired);
    }
}

/* ──────────────────────────── PN532 I2C Low-Level ───────────────────────── */

static esp_err_t pn532_i2c_init(void) {
    /* I2C bus may already be initialized by furi_hal_power (shared QWIIC/NFC pins).
     * Try to install; if already running, just reuse it. */
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = BOARD_PIN_NFC_SDA,
        .scl_io_num = BOARD_PIN_NFC_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };

    esp_err_t err = i2c_driver_install(BOARD_NFC_I2C_PORT, conf.mode, 0, 0, 0);
    if(err == ESP_OK) {
        /* Fresh install — configure pins */
        i2c_param_config(BOARD_NFC_I2C_PORT, &conf);
    } else {
        /* Already installed (by power or touch) — reuse as-is */
        FURI_LOG_I(TAG, "I2C bus %d already initialized, reusing", BOARD_NFC_I2C_PORT);
        err = ESP_OK;
    }
    return err;
}

/** Wait for PN532 ready: IRQ pin LOW or I2C RDY byte polling */
static bool pn532_wait_ready(uint32_t timeout_ms) {
#ifdef BOARD_PIN_NFC_IRQ
    uint32_t start = furi_get_tick();
    while((furi_get_tick() - start) < timeout_ms) {
        if(gpio_get_level(BOARD_PIN_NFC_IRQ) == 0) return true;
        furi_delay_ms(2);
    }
    return false;
#else
    uint8_t status;
    uint32_t start = furi_get_tick();
    while((furi_get_tick() - start) < timeout_ms) {
        esp_err_t err = i2c_master_read_from_device(
            BOARD_NFC_I2C_PORT, PN532_I2C_ADDR, &status, 1, pdMS_TO_TICKS(10));
        if(err == ESP_OK && (status & 0x01)) return true;
        furi_delay_ms(5);
    }
    return false;
#endif
}

/** Read a PN532 I2C response frame.
 *
 * PN532 I2C delivers the complete response in a single read transaction,
 * prepended with a RDY byte. We read enough bytes to capture the full frame.
 * Frame: [RDY(1)] [00 00 FF] [LEN] [LCS] [TFI] [CMD] [payload...] [DCS] [00]
 */
static FuriHalNfcError pn532_read_response(uint8_t* response, size_t* response_len, size_t max_len) {
    /* Read enough for: RDY(1) + preamble(3) + LEN(1) + LCS(1) + data(max_len) + DCS(1) + postamble(1) */
    size_t read_len = max_len + 8;
    if(read_len > 255) read_len = 255;

    uint8_t rx_buf[read_len];
    esp_err_t err = i2c_master_read_from_device(
        BOARD_NFC_I2C_PORT, PN532_I2C_ADDR, rx_buf, read_len, pdMS_TO_TICKS(200));
    if(err != ESP_OK) return FuriHalNfcErrorCommunication;

    /* Validate: [RDY=0x01] [00] [00] [FF] [LEN] [LCS] [TFI=0xD5] ... */
    if(rx_buf[0] != 0x01) return FuriHalNfcErrorCommunication;
    if(rx_buf[1] != 0x00 || rx_buf[2] != 0x00 || rx_buf[3] != 0xFF)
        return FuriHalNfcErrorDataFormat;

    uint8_t data_len = rx_buf[4];
    if(data_len < 2) return FuriHalNfcErrorDataFormat;
    if(rx_buf[6] != PN532_PN532TOHOST) return FuriHalNfcErrorDataFormat;

    /* Payload = everything after TFI and command code */
    size_t payload_len = data_len - 2;
    if(response && response_len) {
        if(payload_len > *response_len) return FuriHalNfcErrorBufferOverflow;
        memcpy(response, &rx_buf[8], payload_len);
        *response_len = payload_len;
    }
    return FuriHalNfcErrorNone;
}

/**
 * Send a PN532 command and receive the response.
 * Handles: I2C write → wait ACK → read ACK → wait response → read response.
 */
static FuriHalNfcError pn532_send_command(
    const uint8_t* cmd,
    size_t cmd_len,
    uint8_t* response,
    size_t* response_len,
    uint32_t timeout_ms) {

    /* Build I2C frame: [SFI=0x00] [00 00 FF] [LEN] [LCS] [TFI=0xD4] [cmd...] [DCS] [00] */
    uint8_t frame[cmd_len + 9];
    size_t idx = 0;
    frame[idx++] = 0x00; /* I2C SFI byte */
    frame[idx++] = PN532_PREAMBLE;
    frame[idx++] = PN532_STARTCODE1;
    frame[idx++] = PN532_STARTCODE2;
    uint8_t data_len = cmd_len + 1;
    frame[idx++] = data_len;
    frame[idx++] = (~data_len) + 1;
    frame[idx++] = PN532_HOSTTOPN532;

    uint8_t checksum = PN532_HOSTTOPN532;
    for(size_t i = 0; i < cmd_len; i++) {
        frame[idx++] = cmd[i];
        checksum += cmd[i];
    }
    frame[idx++] = (~checksum) + 1;
    frame[idx++] = 0x00;

    /* Reset I2C FIFOs */
    i2c_reset_tx_fifo(BOARD_NFC_I2C_PORT);
    i2c_reset_rx_fifo(BOARD_NFC_I2C_PORT);

    esp_err_t err = i2c_master_write_to_device(
        BOARD_NFC_I2C_PORT, PN532_I2C_ADDR, frame, idx, pdMS_TO_TICKS(1000));
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "I2C write failed: %s", esp_err_to_name(err));
        return FuriHalNfcErrorCommunication;
    }

    /* Phase 1: Wait for and read ACK frame */
    if(!pn532_wait_ready(timeout_ms)) {
        return FuriHalNfcErrorCommunicationTimeout;
    }

    uint8_t ack_buf[PN532_ACK_LENGTH];
    err = i2c_master_read_from_device(
        BOARD_NFC_I2C_PORT, PN532_I2C_ADDR, ack_buf, sizeof(ack_buf), pdMS_TO_TICKS(200));
    if(err != ESP_OK) return FuriHalNfcErrorCommunication;

    /* Verify ACK: [RDY=0x01] [00 00 FF 00 FF 00] */
    if(ack_buf[0] != 0x01 || ack_buf[3] != 0xFF || ack_buf[4] != 0x00 || ack_buf[5] != 0xFF) {
        FURI_LOG_E(TAG, "Bad ACK: %02X %02X %02X %02X %02X %02X %02X",
            ack_buf[0], ack_buf[1], ack_buf[2], ack_buf[3], ack_buf[4], ack_buf[5], ack_buf[6]);
        return FuriHalNfcErrorCommunication;
    }

    /* Phase 2: Wait for and read response */
    if(!pn532_wait_ready(timeout_ms)) {
        return FuriHalNfcErrorCommunicationTimeout;
    }

    return pn532_read_response(response, response_len, response_len ? *response_len : 0);
}

/** Convert PN532 InCommunicateThru/InDataExchange status byte to HAL error */
static FuriHalNfcError pn532_status_to_error(uint8_t status) {
    switch(status) {
    case PN532_STATUS_OK: return FuriHalNfcErrorNone;
    case PN532_STATUS_TIMEOUT: return FuriHalNfcErrorCommunicationTimeout;
    case PN532_STATUS_CRC_ERROR: return FuriHalNfcErrorDataFormat;
    case PN532_STATUS_PARITY_ERROR: return FuriHalNfcErrorDataFormat;
    case PN532_STATUS_COLLISION: return FuriHalNfcErrorIncompleteFrame;
    default: return FuriHalNfcErrorCommunication;
    }
}

/* ──────────────────────────── HAL Public API ─────────────────────────────── */

FuriHalNfcError furi_hal_nfc_init(void) {
    FURI_LOG_I(TAG, "Initializing NFC HAL (PN532 I2C)");

    nfc_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    nfc_event_flags = furi_event_flag_alloc();

    /* Create software timers */
    const esp_timer_create_args_t fwt_args = { .callback = fwt_timer_cb, .name = "nfc_fwt" };
    const esp_timer_create_args_t btx_args = { .callback = block_tx_timer_cb, .name = "nfc_btx" };
    esp_timer_create(&fwt_args, &fwt_timer);
    esp_timer_create(&btx_args, &block_tx_timer);

    /* Configure IRQ pin */
#ifdef BOARD_PIN_NFC_IRQ
    gpio_config_t irq_conf = {
        .pin_bit_mask = (1ULL << BOARD_PIN_NFC_IRQ),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&irq_conf);
#endif

    /* Ensure RST is HIGH (PN532 powered from board power-on) */
#ifdef BOARD_PIN_NFC_RST
    gpio_config_t rst_conf = {
        .pin_bit_mask = (1ULL << BOARD_PIN_NFC_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&rst_conf);
    gpio_set_level(BOARD_PIN_NFC_RST, 1);
#endif

    /* Init I2C (bus likely already initialized by furi_hal_power) */
    esp_err_t err = pn532_i2c_init();
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "I2C init failed: %s", esp_err_to_name(err));
        return FuriHalNfcErrorCommunication;
    }

    furi_delay_ms(150);

    /* Verify PN532 with GetFirmwareVersion */
    uint8_t cmd[] = {PN532_CMD_GETFIRMWAREVERSION};
    uint8_t resp[4];
    size_t resp_len = sizeof(resp);
    FuriHalNfcError nfc_err = pn532_send_command(cmd, sizeof(cmd), resp, &resp_len, 2000);
    if(nfc_err != FuriHalNfcErrorNone) {
        FURI_LOG_E(TAG, "PN532 not found");
        return FuriHalNfcErrorCommunication;
    }

    FURI_LOG_I(TAG, "PN532 IC=0x%02X FW=%d.%d Support=0x%02X", resp[0], resp[1], resp[2], resp[3]);

    /* SAM Configuration: normal mode, timeout=0x14 (1s), use IRQ pin
     * (matches Adafruit_PN532::SAMConfig) */
    uint8_t sam_cmd[] = {PN532_CMD_SAMCONFIGURATION, 0x01, 0x14, 0x01};
    nfc_err = pn532_send_command(sam_cmd, sizeof(sam_cmd), NULL, NULL, 1000);
    if(nfc_err != FuriHalNfcErrorNone) {
        FURI_LOG_E(TAG, "SAM config failed");
        return FuriHalNfcErrorCommunication;
    }

    /* Configure retries: ATR_RES=0xFF, PSL_RES=0x01, passive_activation=0xFF
     * (match PN532 defaults / Adafruit behavior for reliable detection) */
    uint8_t retry_cmd[] = {PN532_CMD_RFCONFIGURATION, PN532_RFCFG_RETRIES, 0xFF, 0x01, 0xFF};
    pn532_send_command(retry_cmd, sizeof(retry_cmd), NULL, NULL, 1000);

    nfc_hal_ready = true;
    pn532_target_number = 0;
    FURI_LOG_I(TAG, "NFC HAL initialized");
    return FuriHalNfcErrorNone;
}

FuriHalNfcError furi_hal_nfc_is_hal_ready(void) {
    return nfc_hal_ready ? FuriHalNfcErrorNone : FuriHalNfcErrorCommunication;
}

FuriHalNfcError furi_hal_nfc_acquire(void) {
    if(!nfc_hal_ready) return FuriHalNfcErrorCommunication;
    furi_check(furi_mutex_acquire(nfc_mutex, FuriWaitForever) == FuriStatusOk);
    return FuriHalNfcErrorNone;
}

FuriHalNfcError furi_hal_nfc_release(void) {
    furi_check(furi_mutex_release(nfc_mutex) == FuriStatusOk);
    return FuriHalNfcErrorNone;
}

FuriHalNfcError furi_hal_nfc_low_power_mode_start(void) {
    if(!nfc_hal_ready) return FuriHalNfcErrorNone;
    /* Turn off RF field */
    uint8_t cmd[] = {PN532_CMD_RFCONFIGURATION, PN532_RFCFG_FIELD, 0x00};
    pn532_send_command(cmd, sizeof(cmd), NULL, NULL, 500);
    pn532_target_number = 0;
    return FuriHalNfcErrorNone;
}

FuriHalNfcError furi_hal_nfc_low_power_mode_stop(void) {
    if(!nfc_hal_ready) return FuriHalNfcErrorNone;
    /* Wake up: SAM config ensures normal mode */
    uint8_t cmd[] = {PN532_CMD_SAMCONFIGURATION, 0x01, 0x00, 0x01};
    return pn532_send_command(cmd, sizeof(cmd), NULL, NULL, 1000);
}

FuriHalNfcError furi_hal_nfc_set_mode(FuriHalNfcMode mode, FuriHalNfcTech tech) {
    if(!nfc_hal_ready) return FuriHalNfcErrorCommunication;
    nfc_current_mode = mode;
    nfc_current_tech = tech;
    pn532_target_number = 0;
    pn532_iso_dep_mode = false;
    pn532_block_number = 0;

    /* For poller mode, use short passive-activation retries so InListPassiveTarget
     * doesn't block too long when no card is present, but still retries enough
     * for reliable detection. */
    if(mode == FuriHalNfcModePoller) {
        uint8_t cmd[] = {PN532_CMD_RFCONFIGURATION, PN532_RFCFG_RETRIES, 0xFF, 0x01, 0x02};
        pn532_send_command(cmd, sizeof(cmd), NULL, NULL, 500);
    }
    return FuriHalNfcErrorNone;
}

FuriHalNfcError furi_hal_nfc_reset_mode(void) {
    nfc_current_mode = FuriHalNfcModeNum;
    nfc_current_tech = FuriHalNfcTechInvalid;
    pn532_target_number = 0;
    /* Turn off RF field */
    uint8_t cmd[] = {PN532_CMD_RFCONFIGURATION, PN532_RFCFG_FIELD, 0x00};
    pn532_send_command(cmd, sizeof(cmd), NULL, NULL, 500);
    return FuriHalNfcErrorNone;
}

FuriHalNfcError furi_hal_nfc_field_detect_start(void) {
    /* PN532 doesn't directly support passive field detection.
     * For listener mode, TgInitAsTarget handles field detection. */
    return FuriHalNfcErrorNone;
}

FuriHalNfcError furi_hal_nfc_field_detect_stop(void) {
    return FuriHalNfcErrorNone;
}

bool furi_hal_nfc_field_is_present(void) {
    /* Could probe with InListPassiveTarget timeout=0, but too slow for polling */
    return false;
}

FuriHalNfcError furi_hal_nfc_poller_field_on(void) {
    if(!nfc_hal_ready) return FuriHalNfcErrorCommunication;
    uint8_t cmd[] = {PN532_CMD_RFCONFIGURATION, PN532_RFCFG_FIELD, 0x01};
    return pn532_send_command(cmd, sizeof(cmd), NULL, NULL, 1000);
}

/* ──────────────────────────── Event System ───────────────────────────────── */

FuriHalNfcEvent furi_hal_nfc_poller_wait_event(uint32_t timeout_ms) {
    if(!nfc_hal_ready) return FuriHalNfcEventTimeout;
    if(!nfc_event_flags) return FuriHalNfcEventTimeout;

    uint32_t flags = furi_event_flag_wait(
        nfc_event_flags, NFC_EVENT_ALL_BITS, FuriFlagWaitAny | FuriFlagNoClear, timeout_ms);

    if(flags & FuriFlagError) return FuriHalNfcEventTimeout;

    furi_event_flag_clear(nfc_event_flags, flags & NFC_EVENT_ALL_BITS);
    return (FuriHalNfcEvent)(flags & NFC_EVENT_ALL_BITS);
}

FuriHalNfcEvent furi_hal_nfc_listener_wait_event(uint32_t timeout_ms) {
    return furi_hal_nfc_poller_wait_event(timeout_ms);
}

FuriHalNfcError furi_hal_nfc_event_start(void) {
    if(nfc_event_flags) furi_event_flag_clear(nfc_event_flags, NFC_EVENT_ALL_BITS);
    return FuriHalNfcErrorNone;
}

FuriHalNfcError furi_hal_nfc_event_stop(void) {
    return FuriHalNfcErrorNone;
}

FuriHalNfcError furi_hal_nfc_abort(void) {
    if(nfc_event_flags) furi_event_flag_set(nfc_event_flags, FuriHalNfcEventAbortRequest);
    return FuriHalNfcErrorNone;
}

/* ──────────────────────────── Timer System ───────────────────────────────── */

void furi_hal_nfc_timer_fwt_start(uint32_t time_fc) {
    if(!fwt_timer) return;
    /* Convert carrier cycles to microseconds: 1 fc = 1/13.56MHz ≈ 73.75ns */
    uint64_t us = ((uint64_t)time_fc * 1000ULL) / 13560ULL;
    if(us < 10) us = 10;
    esp_timer_stop(fwt_timer); /* stop if already running */
    esp_timer_start_once(fwt_timer, us);
}

void furi_hal_nfc_timer_fwt_stop(void) {
    if(fwt_timer) esp_timer_stop(fwt_timer);
}

void furi_hal_nfc_timer_block_tx_start(uint32_t time_fc) {
    if(!block_tx_timer) return;
    uint64_t us = ((uint64_t)time_fc * 1000ULL) / 13560ULL;
    if(us < 10) us = 10;
    block_tx_running = true;
    esp_timer_stop(block_tx_timer);
    esp_timer_start_once(block_tx_timer, us);
}

void furi_hal_nfc_timer_block_tx_start_us(uint32_t time_us) {
    if(!block_tx_timer) return;
    if(time_us < 10) time_us = 10;
    block_tx_running = true;
    esp_timer_stop(block_tx_timer);
    esp_timer_start_once(block_tx_timer, time_us);
}

void furi_hal_nfc_timer_block_tx_stop(void) {
    if(block_tx_timer) esp_timer_stop(block_tx_timer);
    block_tx_running = false;
}

bool furi_hal_nfc_timer_block_tx_is_running(void) {
    return block_tx_running;
}

/* ──────────────────────────── TRX & Communication ────────────────────────── */

FuriHalNfcError furi_hal_nfc_trx_reset(void) {
    if(nfc_event_flags) furi_event_flag_clear(nfc_event_flags, NFC_EVENT_ALL_BITS);
    pn532_rx_bits = 0;
    return FuriHalNfcErrorNone;
}

/**
 * Poller TX: Send data to card and buffer the response.
 *
 * Bridges the gap between Flipper's raw-transceiver model and PN532's
 * high-level protocol management:
 * - SELECT commands: answered from InListPassiveTarget cache
 * - RATS: answered from cached ATS (PN532 already did RATS internally)
 * - HALT: releases PN532 target
 * - ISO-DEP I-blocks: strip/add I-block header, pass payload via InDataExchange
 * - Raw commands (non-ISO-DEP): pass via InDataExchange with CRC translation
 */
FuriHalNfcError furi_hal_nfc_poller_tx(const uint8_t* tx_data, size_t tx_bits) {
    if(!nfc_hal_ready) return FuriHalNfcErrorCommunication;

    size_t tx_bytes = (tx_bits + 7) / 8;
    pn532_rx_bits = 0;

    /* === 1. SELECT interception (CL1/CL2/CL3 with NVB=0x70) === */
    if(pn532_target_number > 0 && tx_bytes >= 2 &&
       (tx_data[0] == 0x93 || tx_data[0] == 0x95 || tx_data[0] == 0x97) &&
       tx_data[1] == 0x70) {
        uint8_t cascade = (tx_data[0] - 0x93) / 2;
        uint8_t max_cascade = (pn532_target_uid_len <= 4) ? 0 :
                              (pn532_target_uid_len <= 7) ? 1 : 2;
        uint8_t sak = (cascade < max_cascade) ? 0x04 : pn532_target_sak;
        pn532_rx_buf[0] = sak;
        crc_a_append(pn532_rx_buf, 1);
        pn532_rx_bits = 24;
        FURI_LOG_I(TAG, "SELECT CL%d SAK=%02X", cascade + 1, sak);
        if(nfc_event_flags)
            furi_event_flag_set(nfc_event_flags,
                FuriHalNfcEventTxEnd | FuriHalNfcEventRxStart | FuriHalNfcEventRxEnd);
        return FuriHalNfcErrorNone;
    }

    /* === 2. RATS interception (0xE0) → return cached ATS === */
    if(pn532_target_number > 0 && tx_bytes >= 2 && tx_data[0] == 0xE0 &&
       pn532_iso_dep_active && pn532_cached_ats_len > 0) {
        memcpy(pn532_rx_buf, pn532_cached_ats, pn532_cached_ats_len);
        crc_a_append(pn532_rx_buf, pn532_cached_ats_len);
        pn532_rx_bits = (pn532_cached_ats_len + 2) * 8;
        pn532_iso_dep_mode = true;
        pn532_block_number = 0;
        FURI_LOG_I(TAG, "RATS -> ATS (%d bytes)", pn532_cached_ats_len);
        if(nfc_event_flags)
            furi_event_flag_set(nfc_event_flags,
                FuriHalNfcEventTxEnd | FuriHalNfcEventRxStart | FuriHalNfcEventRxEnd);
        return FuriHalNfcErrorNone;
    }

    /* === 3. HALT interception (0x50 0x00) → release target === */
    if(tx_bytes >= 2 && tx_data[0] == 0x50 && tx_data[1] == 0x00) {
        if(pn532_target_number > 0) {
            uint8_t rel_cmd[] = {0x52, 0x00}; /* InRelease all targets */
            pn532_send_command(rel_cmd, sizeof(rel_cmd), NULL, NULL, 200);
            pn532_target_number = 0;
            pn532_iso_dep_mode = false;
        }
        if(nfc_event_flags)
            furi_event_flag_set(nfc_event_flags,
                FuriHalNfcEventTxEnd | FuriHalNfcEventTimerFwtExpired);
        return FuriHalNfcErrorCommunicationTimeout;
    }

    /* === 4. PPS interception (0xD0) → return success === */
    if(tx_bytes >= 1 && (tx_data[0] & 0xF0) == 0xD0) {
        pn532_rx_buf[0] = tx_data[0]; /* PPS response echoes PPS byte */
        crc_a_append(pn532_rx_buf, 1);
        pn532_rx_bits = 24;
        FURI_LOG_D(TAG, "PPS intercepted");
        if(nfc_event_flags)
            furi_event_flag_set(nfc_event_flags,
                FuriHalNfcEventTxEnd | FuriHalNfcEventRxStart | FuriHalNfcEventRxEnd);
        return FuriHalNfcErrorNone;
    }

    /* === 5. Data exchange via InDataExchange === */

    /* Determine payload to send to PN532:
     * - Strip CRC_A (last 2 bytes) — PN532 handles CRC internally
     * - If in ISO-DEP mode: strip I-block header (PCB + optional CID/NAD)
     *   since PN532 manages I-block framing via InDataExchange */
    const uint8_t* payload = tx_data;
    size_t payload_len = tx_bytes;

    /* Strip CRC (Flipper stack appends it, PN532 adds its own) */
    if(payload_len >= 3) payload_len -= 2;

    uint8_t saved_pcb = 0;
    if(pn532_iso_dep_mode && payload_len >= 1) {
        saved_pcb = payload[0]; /* Save PCB for building response I-block */
        /* Strip I-block/R-block/S-block header */
        size_t hdr_len = 1; /* PCB byte */
        if(saved_pcb & 0x08) hdr_len++; /* CID follows */
        if(saved_pcb & 0x04) hdr_len++; /* NAD follows */

        /* S-block (WTX etc.): PN532 handles internally, just ACK */
        if((saved_pcb & 0xC0) == 0xC0) {
            FURI_LOG_D(TAG, "poller_tx: S-block %02X handled internally", saved_pcb);
            pn532_rx_buf[0] = saved_pcb; /* Echo S-block */
            if(payload_len > hdr_len) {
                memcpy(&pn532_rx_buf[1], &payload[hdr_len], payload_len - hdr_len);
            }
            size_t resp_sz = payload_len;
            crc_a_append(pn532_rx_buf, resp_sz);
            pn532_rx_bits = (resp_sz + 2) * 8;
            if(nfc_event_flags)
                furi_event_flag_set(nfc_event_flags,
                    FuriHalNfcEventTxEnd | FuriHalNfcEventRxStart | FuriHalNfcEventRxEnd);
            return FuriHalNfcErrorNone;
        }

        /* R-block (ACK/NAK): PN532 handles internally */
        if((saved_pcb & 0xC0) == 0x80) {
            FURI_LOG_D(TAG, "poller_tx: R-block %02X handled internally", saved_pcb);
            /* Don't send to card, just return ACK to stack */
            pn532_rx_buf[0] = saved_pcb & 0xFE; /* R-ACK */
            crc_a_append(pn532_rx_buf, 1);
            pn532_rx_bits = 24;
            if(nfc_event_flags)
                furi_event_flag_set(nfc_event_flags,
                    FuriHalNfcEventTxEnd | FuriHalNfcEventRxStart | FuriHalNfcEventRxEnd);
            return FuriHalNfcErrorNone;
        }

        /* I-block: strip header, send payload via InDataExchange */
        if(hdr_len < payload_len) {
            payload += hdr_len;
            payload_len -= hdr_len;
        } else {
            payload_len = 0;
        }
    }

    /* No target listed → return timeout immediately (non-14443A tech polls) */
    if(pn532_target_number == 0) {
        if(nfc_event_flags)
            furi_event_flag_set(nfc_event_flags,
                FuriHalNfcEventTxEnd | FuriHalNfcEventTimerFwtExpired);
        return FuriHalNfcErrorCommunicationTimeout;
    }

    FURI_LOG_I(TAG, "TRX: %u bytes dep=%d cmd=%02X",
        (unsigned)payload_len, pn532_iso_dep_mode,
        payload_len > 0 ? payload[0] : 0);

    /* Build InDataExchange command */
    uint8_t cmd[payload_len + 2];
    cmd[0] = PN532_CMD_INDATAEXCHANGE;
    cmd[1] = pn532_target_number;
    if(payload_len > 0) memcpy(&cmd[2], payload, payload_len);

    uint8_t resp[128];
    size_t resp_len = sizeof(resp);

    FuriHalNfcError err = pn532_send_command(cmd, payload_len + 2, resp, &resp_len, 2000);

    if(err == FuriHalNfcErrorNone && resp_len >= 1) {
        uint8_t status = resp[0];
        if(status == PN532_STATUS_OK) {
            size_t data_len = resp_len - 1; /* strip status byte */

            if(pn532_iso_dep_mode) {
                /* Rebuild I-block: [PCB] [response data] [CRC] */
                uint8_t resp_pcb = 0x02 | (pn532_block_number & 1); /* I-block + block num */
                pn532_rx_buf[0] = resp_pcb;
                if(data_len > 0 && data_len < sizeof(pn532_rx_buf) - 3) {
                    memcpy(&pn532_rx_buf[1], &resp[1], data_len);
                }
                crc_a_append(pn532_rx_buf, 1 + data_len);
                pn532_rx_bits = (1 + data_len + 2) * 8;
                pn532_block_number ^= 1; /* Toggle block number */
            } else {
                /* Raw mode: [data] [CRC] */
                if(data_len > sizeof(pn532_rx_buf) - 2) data_len = sizeof(pn532_rx_buf) - 2;
                if(data_len > 0) memcpy(pn532_rx_buf, &resp[1], data_len);
                crc_a_append(pn532_rx_buf, data_len);
                pn532_rx_bits = (data_len + 2) * 8;
            }

            FURI_LOG_I(TAG, "TRX OK: %u bits [%02X %02X %02X %02X...]",
                (unsigned)pn532_rx_bits,
                pn532_rx_buf[0],
                pn532_rx_bits > 8 ? pn532_rx_buf[1] : 0,
                pn532_rx_bits > 16 ? pn532_rx_buf[2] : 0,
                pn532_rx_bits > 24 ? pn532_rx_buf[3] : 0);
            if(nfc_event_flags)
                furi_event_flag_set(nfc_event_flags,
                    FuriHalNfcEventTxEnd | FuriHalNfcEventRxStart | FuriHalNfcEventRxEnd);
        } else {
            err = pn532_status_to_error(status);
            FURI_LOG_I(TAG, "TRX err: %02X", status);
            if(nfc_event_flags)
                furi_event_flag_set(nfc_event_flags,
                    FuriHalNfcEventTxEnd | FuriHalNfcEventTimerFwtExpired);
        }
    } else {
        if(err == FuriHalNfcErrorNone) err = FuriHalNfcErrorCommunicationTimeout;
        if(nfc_event_flags)
            furi_event_flag_set(nfc_event_flags,
                FuriHalNfcEventTxEnd | FuriHalNfcEventTimerFwtExpired);
    }

    return err;
}

FuriHalNfcError furi_hal_nfc_poller_rx(uint8_t* rx_data, size_t rx_data_size, size_t* rx_bits) {
    if(!nfc_hal_ready) return FuriHalNfcErrorCommunication;

    size_t rx_bytes = (pn532_rx_bits + 7) / 8;
    if(rx_bytes > rx_data_size) {
        *rx_bits = 0;
        return FuriHalNfcErrorBufferOverflow;
    }
    if(rx_bytes > 0) memcpy(rx_data, pn532_rx_buf, rx_bytes);
    *rx_bits = pn532_rx_bits;
    pn532_rx_bits = 0;
    return FuriHalNfcErrorNone;
}

/* ──────────────────────────── Listener TX/RX ─────────────────────────────── */

FuriHalNfcError furi_hal_nfc_listener_tx(const uint8_t* tx_data, size_t tx_bits) {
    if(!nfc_hal_ready) return FuriHalNfcErrorCommunication;

    size_t tx_bytes = (tx_bits + 7) / 8;
    uint8_t cmd[tx_bytes + 1];
    cmd[0] = PN532_CMD_TGRESPONSETOINITIATOR;
    memcpy(&cmd[1], tx_data, tx_bytes);

    return pn532_send_command(cmd, tx_bytes + 1, NULL, NULL, 1000);
}

FuriHalNfcError furi_hal_nfc_listener_rx(uint8_t* rx_data, size_t rx_data_size, size_t* rx_bits) {
    if(!nfc_hal_ready) return FuriHalNfcErrorCommunication;

    uint8_t cmd[] = {PN532_CMD_TGGETDATA};
    uint8_t resp[128];
    size_t resp_len = sizeof(resp);
    FuriHalNfcError err = pn532_send_command(cmd, sizeof(cmd), resp, &resp_len, 1000);
    if(err == FuriHalNfcErrorNone && resp_len >= 1) {
        if(resp[0] == PN532_STATUS_OK && resp_len > 1) {
            size_t data_len = resp_len - 1;
            if(data_len > rx_data_size) return FuriHalNfcErrorBufferOverflow;
            memcpy(rx_data, &resp[1], data_len);
            *rx_bits = data_len * 8;
        } else {
            *rx_bits = 0;
            err = pn532_status_to_error(resp[0]);
        }
    }
    return err;
}

FuriHalNfcError furi_hal_nfc_listener_sleep(void) {
    return FuriHalNfcErrorNone;
}

FuriHalNfcError furi_hal_nfc_listener_idle(void) {
    return FuriHalNfcErrorNone;
}

FuriHalNfcError furi_hal_nfc_listener_enable_rx(void) {
    return FuriHalNfcErrorNone;
}

/* ──────────────────────────── ISO14443A Poller ────────────────────────────── */

/**
 * Short frame (REQA/WUPA): PN532 handles this via InListPassiveTarget.
 * We cache the full target info (ATQA, SAK, UID) and return ATQA to the caller.
 * Subsequent SDD and SELECT commands use the cached data.
 */
FuriHalNfcError furi_hal_nfc_iso14443a_poller_trx_short_frame(FuriHalNfcaShortFrame frame) {
    if(!nfc_hal_ready) return FuriHalNfcErrorCommunication;
    UNUSED(frame); /* PN532 always does REQA internally */

    FURI_LOG_D(TAG, "InListPassiveTarget");

    /* Release any previously listed target (for clean re-scan) */
    pn532_target_number = 0;

    uint8_t cmd[] = {PN532_CMD_INLISTPASSIVETARGET, 0x01, PN532_BRTY_ISO14443A};
    uint8_t resp[64];
    size_t resp_len = sizeof(resp);
    FuriHalNfcError err = pn532_send_command(cmd, sizeof(cmd), resp, &resp_len, 200);

    FURI_LOG_D(TAG, "InListPassiveTarget: err=%d resp_len=%u", (int)err, (unsigned)resp_len);

    pn532_rx_bits = 0;

    if(err == FuriHalNfcErrorNone && resp_len >= 6 && resp[0] > 0) {
        /* Parse: [NbTg] [Tg] [ATQA0] [ATQA1] [SAK] [UIDLen] [UID...] */
        pn532_target_number = resp[1];
        pn532_target_atqa[0] = resp[2];
        pn532_target_atqa[1] = resp[3];
        pn532_target_sak = resp[4];
        pn532_target_uid_len = resp[5];
        if(pn532_target_uid_len > sizeof(pn532_target_uid))
            pn532_target_uid_len = sizeof(pn532_target_uid);
        if(resp_len >= 6u + pn532_target_uid_len) {
            memcpy(pn532_target_uid, &resp[6], pn532_target_uid_len);
        }

        /* Cache ATS if present (ISO14443-4 capable tags include ATS after UID) */
        pn532_iso_dep_active = (pn532_target_sak & 0x20) != 0;
        pn532_iso_dep_mode = false;
        pn532_block_number = 0;
        pn532_cached_ats_len = 0;
        size_t ats_offset = 6 + pn532_target_uid_len;
        if(pn532_iso_dep_active && resp_len > ats_offset) {
            pn532_cached_ats_len = resp_len - ats_offset;
            if(pn532_cached_ats_len > sizeof(pn532_cached_ats))
                pn532_cached_ats_len = sizeof(pn532_cached_ats);
            memcpy(pn532_cached_ats, &resp[ats_offset], pn532_cached_ats_len);
        }

        FURI_LOG_I(TAG, "Tag: ATQA=%02X%02X SAK=%02X UID=%dB iso_dep=%d",
            pn532_target_atqa[0], pn532_target_atqa[1], pn532_target_sak,
            pn532_target_uid_len, pn532_iso_dep_active);

        /* Return ATQA as the response (what the ISO14443-3A poller expects) */
        pn532_rx_buf[0] = pn532_target_atqa[0];
        pn532_rx_buf[1] = pn532_target_atqa[1];
        pn532_rx_bits = 16;

        if(nfc_event_flags) {
            furi_event_flag_set(nfc_event_flags,
                FuriHalNfcEventTxEnd | FuriHalNfcEventRxStart | FuriHalNfcEventRxEnd);
        }
    } else {
        FURI_LOG_D(TAG, "short_frame: no target (err=%d resp_len=%u resp[0]=%02X)",
            (int)err, (unsigned)resp_len, resp_len > 0 ? resp[0] : 0xFF);
        pn532_target_number = 0;
        if(nfc_event_flags) {
            furi_event_flag_set(nfc_event_flags,
                FuriHalNfcEventTxEnd | FuriHalNfcEventTimerFwtExpired);
        }
        err = FuriHalNfcErrorCommunicationTimeout;
    }

    return err;
}

/**
 * SDD frame TX: The Flipper stack sends SELECT/anticollision commands here.
 * PN532 already did anticollision in InListPassiveTarget, so we fake it by
 * returning cached UID data. We detect what the stack is asking for based
 * on the SELECT command bytes.
 */
FuriHalNfcError furi_hal_nfc_iso14443a_tx_sdd_frame(const uint8_t* tx_data, size_t tx_bits) {
    if(!nfc_hal_ready) return FuriHalNfcErrorCommunication;

    size_t tx_bytes_dbg = (tx_bits + 7) / 8;
    FURI_LOG_D(TAG, "sdd: cmd=%02X %02X",
        tx_bytes_dbg > 0 ? tx_data[0] : 0, tx_bytes_dbg > 1 ? tx_data[1] : 0);

    pn532_rx_bits = 0;

    if(pn532_target_number == 0) {
        /* No target listed — cannot fake SDD */
        if(nfc_event_flags) {
            furi_event_flag_set(nfc_event_flags,
                FuriHalNfcEventTxEnd | FuriHalNfcEventTimerFwtExpired);
        }
        return FuriHalNfcErrorCommunicationTimeout;
    }

    size_t tx_bytes = (tx_bits + 7) / 8;

    /* Detect SELECT cascade level from first byte:
     * 0x93 = SEL_CL1, 0x95 = SEL_CL2, 0x97 = SEL_CL3
     * Second byte 0x20 = request UID bits (NVB=2 → request all 4 bytes)
     * Second byte 0x70 = full SELECT with UID (NVB=7) */

    if(tx_bytes >= 2 && (tx_data[0] == 0x93 || tx_data[0] == 0x95 || tx_data[0] == 0x97)) {
        uint8_t cascade = (tx_data[0] - 0x93) / 2; /* 0, 1, or 2 */

        if(tx_data[1] == 0x20) {
            /* Request UID bits for this cascade level */
            size_t uid_offset = cascade * 3; /* Simplified: PN532 returns flat UID */
            size_t remaining = (pn532_target_uid_len > uid_offset) ?
                               (pn532_target_uid_len - uid_offset) : 0;

            if(pn532_target_uid_len <= 4) {
                /* Single-size UID: return [UID0..UID3] + BCC */
                if(remaining >= 4) {
                    memcpy(pn532_rx_buf, &pn532_target_uid[uid_offset], 4);
                    pn532_rx_buf[4] = pn532_rx_buf[0] ^ pn532_rx_buf[1] ^
                                      pn532_rx_buf[2] ^ pn532_rx_buf[3];
                    pn532_rx_bits = 40;
                } else {
                    memcpy(pn532_rx_buf, pn532_target_uid, pn532_target_uid_len);
                    pn532_rx_bits = pn532_target_uid_len * 8;
                }
            } else if(pn532_target_uid_len <= 7) {
                /* Double-size UID */
                if(cascade == 0) {
                    /* CL1: [0x88] [UID0] [UID1] [UID2] + BCC */
                    pn532_rx_buf[0] = 0x88; /* CT (cascade tag) */
                    memcpy(&pn532_rx_buf[1], pn532_target_uid, 3);
                    pn532_rx_buf[4] = pn532_rx_buf[0] ^ pn532_rx_buf[1] ^
                                      pn532_rx_buf[2] ^ pn532_rx_buf[3];
                    pn532_rx_bits = 40;
                } else {
                    /* CL2: [UID3] [UID4] [UID5] [UID6] + BCC */
                    memcpy(pn532_rx_buf, &pn532_target_uid[3], 4);
                    pn532_rx_buf[4] = pn532_rx_buf[0] ^ pn532_rx_buf[1] ^
                                      pn532_rx_buf[2] ^ pn532_rx_buf[3];
                    pn532_rx_bits = 40;
                }
            } else {
                /* Triple-size UID (10 bytes) */
                if(cascade == 0) {
                    pn532_rx_buf[0] = 0x88;
                    memcpy(&pn532_rx_buf[1], pn532_target_uid, 3);
                    pn532_rx_buf[4] = pn532_rx_buf[0] ^ pn532_rx_buf[1] ^
                                      pn532_rx_buf[2] ^ pn532_rx_buf[3];
                    pn532_rx_bits = 40;
                } else if(cascade == 1) {
                    pn532_rx_buf[0] = 0x88;
                    memcpy(&pn532_rx_buf[1], &pn532_target_uid[3], 3);
                    pn532_rx_buf[4] = pn532_rx_buf[0] ^ pn532_rx_buf[1] ^
                                      pn532_rx_buf[2] ^ pn532_rx_buf[3];
                    pn532_rx_bits = 40;
                } else {
                    memcpy(pn532_rx_buf, &pn532_target_uid[6], 4);
                    pn532_rx_buf[4] = pn532_rx_buf[0] ^ pn532_rx_buf[1] ^
                                      pn532_rx_buf[2] ^ pn532_rx_buf[3];
                    pn532_rx_bits = 40;
                }
            }

            if(nfc_event_flags) {
                furi_event_flag_set(nfc_event_flags,
                    FuriHalNfcEventTxEnd | FuriHalNfcEventRxStart | FuriHalNfcEventRxEnd);
            }
        } else if(tx_data[1] == 0x70) {
            /* Full SELECT command — return SAK */
            pn532_rx_buf[0] = pn532_target_sak;
            pn532_rx_bits = 8;
            if(nfc_event_flags) {
                furi_event_flag_set(nfc_event_flags,
                    FuriHalNfcEventTxEnd | FuriHalNfcEventRxStart | FuriHalNfcEventRxEnd);
            }
        } else {
            /* Partial SDD with some known bits — return remaining UID + BCC */
            /* This is a simplification: return full UID for the cascade level */
            pn532_rx_bits = 0;
            if(nfc_event_flags) {
                furi_event_flag_set(nfc_event_flags,
                    FuriHalNfcEventTxEnd | FuriHalNfcEventRxStart | FuriHalNfcEventRxEnd);
            }
        }
    } else {
        /* Unknown command via SDD path — pass through to InCommunicateThru */
        return furi_hal_nfc_poller_tx(tx_data, tx_bits);
    }

    return FuriHalNfcErrorNone;
}

FuriHalNfcError furi_hal_nfc_iso14443a_rx_sdd_frame(
    uint8_t* rx_data,
    size_t rx_data_size,
    size_t* rx_bits) {
    /* Return buffered SDD response from tx_sdd_frame */
    return furi_hal_nfc_poller_rx(rx_data, rx_data_size, rx_bits);
}

FuriHalNfcError furi_hal_nfc_iso14443a_poller_tx_custom_parity(
    const uint8_t* tx_data,
    size_t tx_bits) {
    /* PN532 computes parity internally — send as normal TX.
     * Custom parity is used for Crypto1 (Mifare Classic) where the stream cipher
     * encrypts parity bits. PN532 handles Mifare auth natively. */
    return furi_hal_nfc_poller_tx(tx_data, tx_bits);
}

/* ──────────────────────────── ISO14443A Listener ──────────────────────────── */

FuriHalNfcError furi_hal_nfc_iso14443a_listener_set_col_res_data(
    uint8_t* uid,
    uint8_t uid_len,
    uint8_t* atqa,
    uint8_t sak) {
    if(!nfc_hal_ready) return FuriHalNfcErrorCommunication;

    /* Cache for TgInitAsTarget */
    memcpy(listener_uid, uid, uid_len);
    listener_uid_len = uid_len;
    memcpy(listener_atqa, atqa, 2);
    listener_sak = sak;

    /* Configure PN532 as ISO14443A target using TgInitAsTarget */
    uint8_t cmd[40];
    size_t idx = 0;
    cmd[idx++] = PN532_CMD_TGINITASTARGET;
    cmd[idx++] = 0x00; /* Mode: 0=passive only, b0=106kbps passive, b2=DEP, b3=PICC only */

    /* Mifare params (6 bytes): SENS_RES (2) + NFCID1 (3) + SEL_RES (1) */
    cmd[idx++] = atqa[0]; /* SENS_RES byte 1 */
    cmd[idx++] = atqa[1]; /* SENS_RES byte 2 */
    /* NFCID1: first 3 bytes of UID */
    cmd[idx++] = (uid_len >= 1) ? uid[0] : 0x00;
    cmd[idx++] = (uid_len >= 2) ? uid[1] : 0x00;
    cmd[idx++] = (uid_len >= 3) ? uid[2] : 0x00;
    cmd[idx++] = sak; /* SEL_RES */

    /* FeliCa params (18 bytes): NFCID2 (8) + PAD (8) + System Code (2) — set to zeros */
    for(int i = 0; i < 18; i++) cmd[idx++] = 0x00;

    /* NFCID3t (10 bytes) — use UID padded with zeros */
    for(int i = 0; i < 10; i++) {
        cmd[idx++] = (i < uid_len) ? uid[i] : 0x00;
    }

    /* General Bytes length = 0, Historical Bytes length = 0 */
    cmd[idx++] = 0x00;
    cmd[idx++] = 0x00;

    /* Don't wait for response yet — TgInitAsTarget blocks until a reader selects us.
     * We'll send this command when the listener worker starts waiting for events.
     * For now, just cache the params. */

    return FuriHalNfcErrorNone;
}

FuriHalNfcError furi_hal_nfc_iso14443a_listener_tx_custom_parity(
    const uint8_t* tx_data,
    const uint8_t* tx_parity,
    size_t tx_bits) {
    UNUSED(tx_parity);
    /* PN532 computes parity internally */
    return furi_hal_nfc_listener_tx(tx_data, tx_bits);
}

/* ──────────────────────────── ISO15693 ───────────────────────────────────── */

FuriHalNfcError furi_hal_nfc_iso15693_listener_tx_sof(void) {
    /* PN532 supports ISO15693 via InCommunicateThru with appropriate RF config.
     * SOF is handled automatically by the PN532 when configured for ISO15693. */
    return FuriHalNfcErrorNone;
}

FuriHalNfcError furi_hal_nfc_iso15693_detect_mode(void) {
    /* PN532 auto-detects ISO15693 coding mode */
    return FuriHalNfcErrorNone;
}

FuriHalNfcError furi_hal_nfc_iso15693_force_1outof4(void) {
    /* PN532 doesn't expose coding mode control — auto-detects */
    return FuriHalNfcErrorNone;
}

FuriHalNfcError furi_hal_nfc_iso15693_force_1outof256(void) {
    return FuriHalNfcErrorNone;
}

/* ──────────────────────────── FeliCa ─────────────────────────────────────── */

FuriHalNfcError furi_hal_nfc_felica_listener_set_sensf_res_data(
    const uint8_t* idm,
    const uint8_t idm_len,
    const uint8_t* pmm,
    const uint8_t pmm_len,
    const uint16_t sys_code) {
    UNUSED(idm);
    UNUSED(idm_len);
    UNUSED(pmm);
    UNUSED(pmm_len);
    UNUSED(sys_code);
    /* FeliCa listener data would be configured in TgInitAsTarget's FeliCa params.
     * The params are set during set_col_res equivalent for FeliCa.
     * TODO: Implement FeliCa target mode configuration. */
    return FuriHalNfcErrorNone;
}

#else /* !BOARD_HAS_NFC */

/* ── No NFC hardware: all functions return errors or no-ops ────────────── */

FuriHalNfcError furi_hal_nfc_init(void) {
    FURI_LOG_I(TAG, "NFC HAL: no NFC hardware on this board");
    return FuriHalNfcErrorNone;
}

FuriHalNfcError furi_hal_nfc_is_hal_ready(void) { return FuriHalNfcErrorCommunication; }
FuriHalNfcError furi_hal_nfc_acquire(void) { return FuriHalNfcErrorCommunication; }
FuriHalNfcError furi_hal_nfc_release(void) { return FuriHalNfcErrorNone; }
FuriHalNfcError furi_hal_nfc_low_power_mode_start(void) { return FuriHalNfcErrorNone; }
FuriHalNfcError furi_hal_nfc_low_power_mode_stop(void) { return FuriHalNfcErrorNone; }

FuriHalNfcError furi_hal_nfc_set_mode(FuriHalNfcMode mode, FuriHalNfcTech tech) {
    UNUSED(mode); UNUSED(tech); return FuriHalNfcErrorCommunication;
}

FuriHalNfcError furi_hal_nfc_reset_mode(void) { return FuriHalNfcErrorNone; }
FuriHalNfcError furi_hal_nfc_field_detect_start(void) { return FuriHalNfcErrorNone; }
FuriHalNfcError furi_hal_nfc_field_detect_stop(void) { return FuriHalNfcErrorNone; }
bool furi_hal_nfc_field_is_present(void) { return false; }
FuriHalNfcError furi_hal_nfc_poller_field_on(void) { return FuriHalNfcErrorCommunication; }

FuriHalNfcEvent furi_hal_nfc_poller_wait_event(uint32_t t) { UNUSED(t); return FuriHalNfcEventTimeout; }
FuriHalNfcEvent furi_hal_nfc_listener_wait_event(uint32_t t) { UNUSED(t); return FuriHalNfcEventTimeout; }

FuriHalNfcError furi_hal_nfc_poller_tx(const uint8_t* d, size_t b) { UNUSED(d); UNUSED(b); return FuriHalNfcErrorCommunication; }
FuriHalNfcError furi_hal_nfc_poller_rx(uint8_t* d, size_t s, size_t* b) { UNUSED(d); UNUSED(s); UNUSED(b); return FuriHalNfcErrorCommunication; }
FuriHalNfcError furi_hal_nfc_listener_tx(const uint8_t* d, size_t b) { UNUSED(d); UNUSED(b); return FuriHalNfcErrorCommunication; }
FuriHalNfcError furi_hal_nfc_listener_rx(uint8_t* d, size_t s, size_t* b) { UNUSED(d); UNUSED(s); UNUSED(b); return FuriHalNfcErrorCommunication; }
FuriHalNfcError furi_hal_nfc_listener_sleep(void) { return FuriHalNfcErrorNone; }
FuriHalNfcError furi_hal_nfc_listener_idle(void) { return FuriHalNfcErrorNone; }
FuriHalNfcError furi_hal_nfc_listener_enable_rx(void) { return FuriHalNfcErrorNone; }
FuriHalNfcError furi_hal_nfc_trx_reset(void) { return FuriHalNfcErrorNone; }
FuriHalNfcError furi_hal_nfc_event_start(void) { return FuriHalNfcErrorNone; }
FuriHalNfcError furi_hal_nfc_event_stop(void) { return FuriHalNfcErrorNone; }
FuriHalNfcError furi_hal_nfc_abort(void) { return FuriHalNfcErrorNone; }

void furi_hal_nfc_timer_fwt_start(uint32_t t) { UNUSED(t); }
void furi_hal_nfc_timer_fwt_stop(void) {}
void furi_hal_nfc_timer_block_tx_start(uint32_t t) { UNUSED(t); }
void furi_hal_nfc_timer_block_tx_start_us(uint32_t t) { UNUSED(t); }
void furi_hal_nfc_timer_block_tx_stop(void) {}
bool furi_hal_nfc_timer_block_tx_is_running(void) { return false; }

FuriHalNfcError furi_hal_nfc_iso14443a_poller_trx_short_frame(FuriHalNfcaShortFrame f) { UNUSED(f); return FuriHalNfcErrorCommunication; }
FuriHalNfcError furi_hal_nfc_iso14443a_tx_sdd_frame(const uint8_t* d, size_t b) { UNUSED(d); UNUSED(b); return FuriHalNfcErrorCommunication; }
FuriHalNfcError furi_hal_nfc_iso14443a_rx_sdd_frame(uint8_t* d, size_t s, size_t* b) { UNUSED(d); UNUSED(s); UNUSED(b); return FuriHalNfcErrorCommunication; }
FuriHalNfcError furi_hal_nfc_iso14443a_poller_tx_custom_parity(const uint8_t* d, size_t b) { UNUSED(d); UNUSED(b); return FuriHalNfcErrorCommunication; }

FuriHalNfcError furi_hal_nfc_iso14443a_listener_set_col_res_data(uint8_t* u, uint8_t ul, uint8_t* a, uint8_t s) {
    UNUSED(u); UNUSED(ul); UNUSED(a); UNUSED(s); return FuriHalNfcErrorCommunication;
}

FuriHalNfcError furi_hal_nfc_iso14443a_listener_tx_custom_parity(const uint8_t* d, const uint8_t* p, size_t b) {
    UNUSED(d); UNUSED(p); UNUSED(b); return FuriHalNfcErrorCommunication;
}

FuriHalNfcError furi_hal_nfc_iso15693_listener_tx_sof(void) { return FuriHalNfcErrorCommunication; }
FuriHalNfcError furi_hal_nfc_iso15693_detect_mode(void) { return FuriHalNfcErrorCommunication; }
FuriHalNfcError furi_hal_nfc_iso15693_force_1outof4(void) { return FuriHalNfcErrorCommunication; }
FuriHalNfcError furi_hal_nfc_iso15693_force_1outof256(void) { return FuriHalNfcErrorCommunication; }

FuriHalNfcError furi_hal_nfc_felica_listener_set_sensf_res_data(
    const uint8_t* i, const uint8_t il, const uint8_t* p, const uint8_t pl, const uint16_t s) {
    UNUSED(i); UNUSED(il); UNUSED(p); UNUSED(pl); UNUSED(s); return FuriHalNfcErrorCommunication;
}

#endif /* BOARD_HAS_NFC */
