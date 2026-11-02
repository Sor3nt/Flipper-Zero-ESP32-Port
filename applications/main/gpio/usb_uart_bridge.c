#include "usb_uart_bridge.h"

#include <cli/cli_vcp.h>
#include <driver/gpio.h>
#include <driver/uart.h>
#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_power.h>
#include <furi_hal_uart_shared.h>
#include <furi_hal_usb.h>
#include <furi_hal_usb_cdc.h>
#include <furi_hal_usb_tinyusb_composite.h>
#include <boards/board.h>
#include <sdkconfig.h>
#include <soc/soc_caps.h>
#if CONFIG_IDF_TARGET_ESP32S3
#include <tusb.h>
#endif

#include "../../services/desktop/helpers/qflipper_bridge.h"

#define TAG "UsbUartBridge"
#define USB_UART_TX_BUFFER_SIZE 1024U
/* Worst case still queued for transmission: driver ring buffer + hardware FIFO. */
#define USB_UART_TX_DRAIN_BYTES (USB_UART_TX_BUFFER_SIZE + SOC_UART_FIFO_LEN)
/* The UART divider is a 12-bit integer behind an 8-bit prescaler of the 80 MHz
 * APB clock; slower rates are silently programmed wrong by uart_param_config(). */
#define USB_UART_BAUDRATE_MIN 77U
#define USB_UART_BAUDRATE_MAX ((uint32_t)SOC_UART_BITRATE_MAX)

#define USB_UART_STR_(x) #x
#define USB_UART_STR(x)  USB_UART_STR_(x)

static TickType_t usb_uart_tx_timeout_ticks(size_t byte_count, uint32_t baudrate);

/* RTS/DTR and DE/RE are header GPIOs on boards with BOARD_NRF24_EXT_MODULE /
 * BOARD_SUBGHZ_EXT_MODULE (paradoxt); they are claimed through the GPIO arbiter so an
 * external Sub-GHz/NRF24 module already using one of them is never also driven as a UART
 * control line, and vice versa -- exactly like the Infrared external-TX pin. The fixed
 * UART1 data pair is out of scope here: it is arbitrated separately (furi_hal_uart_shared). */
static const char USB_UART_GPIO_OWNER[] = "USB-UART";

static GpioPin usb_uart_gpio_wrap(gpio_num_t pin) {
    return (GpioPin){.port = NULL, .pin = (uint16_t)pin};
}

static bool usb_uart_reserve_gpio(gpio_num_t pin) {
    if(pin == GPIO_NUM_NC) return true;
    const GpioPin gpio = usb_uart_gpio_wrap(pin);
    if(!furi_hal_gpio_reserve(&gpio, USB_UART_GPIO_OWNER)) {
        FURI_LOG_E(
            TAG,
            "GPIO%d is in use by %s",
            (int)pin,
            furi_hal_gpio_get_reservation(&gpio));
        return false;
    }
    return true;
}

static void usb_uart_unreserve_gpio(gpio_num_t pin) {
    if(pin == GPIO_NUM_NC) return;
    const GpioPin gpio = usb_uart_gpio_wrap(pin);
    furi_hal_gpio_unreserve(&gpio, USB_UART_GPIO_OWNER);
}

#if CONFIG_TINYUSB_CDC_COUNT > 1
#define USB_UART_USB_CHANNEL_COUNT 2U
#else
#define USB_UART_USB_CHANNEL_COUNT 1U
#endif

#ifndef BOARD_USB_UART_NUM
#define BOARD_USB_UART_NUM UART_NUM_1
#endif
#ifndef BOARD_USB_UART_TX
#define BOARD_USB_UART_TX UART_PIN_NO_CHANGE
#endif
#ifndef BOARD_USB_UART_RX
#define BOARD_USB_UART_RX UART_PIN_NO_CHANGE
#endif

#if defined(BOARD_HAS_RFID) && BOARD_HAS_RFID && \
    defined(BOARD_HAS_USB_UART_BRIDGE) && BOARD_HAS_USB_UART_BRIDGE
#if BOARD_USB_UART_NUM != BOARD_RFID_UART_NUM
#error "USB-UART Bridge must use the shared RFID UART peripheral"
#endif
#endif

typedef struct {
    int tx;
    int rx;
    const char* label;
} UsbUartPinPair;

typedef struct {
    gpio_num_t rts;
    gpio_num_t dtr;
    const char* label;
} UsbUartControlPinPair;

typedef enum {
    UsbUartLineCodingApplied,
    UsbUartLineCodingUnsupported,
    UsbUartLineCodingError,
} UsbUartLineCodingResult;

static const UsbUartPinPair usb_uart_pin_pairs[] = {
    {BOARD_USB_UART_TX, BOARD_USB_UART_RX, "Board default"},
#if defined(BOARD_USB_UART_ALT_TX) && defined(BOARD_USB_UART_ALT_RX)
    {BOARD_USB_UART_ALT_TX,
     BOARD_USB_UART_ALT_RX,
     "GPIO" USB_UART_STR(BOARD_USB_UART_ALT_TX) "," USB_UART_STR(BOARD_USB_UART_ALT_RX)},
#endif
};

static const UsbUartControlPinPair usb_uart_control_pin_pairs[] = {
    {GPIO_NUM_NC, GPIO_NUM_NC, "None"},
#if defined(BOARD_USB_UART_RTS_GPIO) && defined(BOARD_USB_UART_DTR_GPIO)
    {BOARD_USB_UART_RTS_GPIO,
     BOARD_USB_UART_DTR_GPIO,
     "GPIO" USB_UART_STR(BOARD_USB_UART_RTS_GPIO) "," USB_UART_STR(BOARD_USB_UART_DTR_GPIO)},
#endif
};

typedef struct {
    gpio_num_t pin;
    uint8_t tx_level; /* level while transmitting; the opposite while idle/receiving */
    const char* label;
} UsbUartDeRe;

static const UsbUartDeRe usb_uart_de_re_pins[] = {
    {GPIO_NUM_NC, 1, "None"},
#if defined(BOARD_USB_UART_DE_RE_GPIO)
    /* Standard RS-485 transceivers (DE and /RE tied): high drives the bus. */
    {BOARD_USB_UART_DE_RE_GPIO, 1, "GPIO" USB_UART_STR(BOARD_USB_UART_DE_RE_GPIO)},
    /* Original Flipper polarity, made for its own RS-485 board: low while sending. */
    {BOARD_USB_UART_DE_RE_GPIO, 0, "GPIO" USB_UART_STR(BOARD_USB_UART_DE_RE_GPIO) " Inv"},
#endif
};

const uint32_t usb_uart_baudrate_presets[USB_UART_BAUDRATE_PRESET_COUNT] = {
    1200, 2400, 4800, 9600, 19200, 28800, 38400, 57600, 115200, 230400, 460800, 921600,
};

struct UsbUartBridge {
    UsbUartConfig cfg;
    UsbUartState state;
    FuriThread* thread;
    FuriSemaphore* wake_sem;
    FuriMutex* mutex;
    CliVcp* cli_vcp;
    FuriHalUsbInterface* previous_usb_config;
    bool cli_vcp_was_enabled;
    bool cli_vcp_disabled;
    bool qflipper_was_active;
    bool qflipper_stopped;
    uint8_t cdc0_owner_channel;
    bool uart_claimed;
    bool uart_installed;
    bool usb_locked;
    bool usb_configured;
    bool composite_used;
    bool sleep_inhibited;
    volatile bool stopping;
    bool usb_mounted;
    bool line_coding_rejected;
    struct usb_cdc_line_coding rejected_line_coding;
    bool applied_config_valid;
    uint8_t active_uart_pin_pair;
    uint8_t active_control_pin_pair;
    uint8_t active_de_re_pin;
    uint32_t applied_baudrate;
    uint8_t applied_data_bits;
    uint8_t applied_parity;
    uint8_t applied_stop_bits;
    uint8_t uart_rx_buffer[CDC_DATA_SZ];
    size_t uart_rx_size;
    size_t uart_rx_offset;
};

static UsbUartBridge* usb_uart_pending_cleanup_bridge;

void usb_uart_config_init(UsbUartConfig* cfg) {
    furi_assert(cfg);
    *cfg = (UsbUartConfig){
        .usb_channel = 0,
        .baudrate_mode = 0,
        .uart_pin_pair = 0,
        .control_pin_pair = 0,
        .de_re_pin = 0,
        .baudrate = 0,
    };
}

uint8_t usb_uart_get_usb_channel_count(void) {
    return USB_UART_USB_CHANNEL_COUNT;
}

uint8_t usb_uart_get_uart_pin_pair_count(void) {
    return COUNT_OF(usb_uart_pin_pairs);
}

int usb_uart_get_uart_tx_pin(uint8_t index) {
    furi_assert(index < COUNT_OF(usb_uart_pin_pairs));
    return usb_uart_pin_pairs[index].tx;
}

int usb_uart_get_uart_rx_pin(uint8_t index) {
    furi_assert(index < COUNT_OF(usb_uart_pin_pairs));
    return usb_uart_pin_pairs[index].rx;
}

const char* usb_uart_get_uart_pin_pair_text(uint8_t index) {
    furi_assert(index < COUNT_OF(usb_uart_pin_pairs));
    return usb_uart_pin_pairs[index].label;
}

uint8_t usb_uart_get_control_pin_pair_count(void) {
    return COUNT_OF(usb_uart_control_pin_pairs);
}

const char* usb_uart_get_control_pin_pair_text(uint8_t index) {
    furi_assert(index < COUNT_OF(usb_uart_control_pin_pairs));
    return usb_uart_control_pin_pairs[index].label;
}

uint8_t usb_uart_get_de_re_pin_count(void) {
    return COUNT_OF(usb_uart_de_re_pins);
}

const char* usb_uart_get_de_re_pin_text(uint8_t index) {
    furi_assert(index < COUNT_OF(usb_uart_de_re_pins));
    return usb_uart_de_re_pins[index].label;
}

static bool usb_uart_config_is_valid(const UsbUartConfig* cfg) {
    if(cfg->usb_channel >= USB_UART_USB_CHANNEL_COUNT ||
       cfg->baudrate_mode > USB_UART_BAUDRATE_PRESET_COUNT ||
       cfg->uart_pin_pair >= COUNT_OF(usb_uart_pin_pairs) ||
       cfg->control_pin_pair >= COUNT_OF(usb_uart_control_pin_pairs) ||
       cfg->de_re_pin >= COUNT_OF(usb_uart_de_re_pins)) {
        return false;
    }
    if((cfg->baudrate_mode == 0 && cfg->baudrate != 0) ||
       (cfg->baudrate_mode != 0 &&
        cfg->baudrate != usb_uart_baudrate_presets[cfg->baudrate_mode - 1])) {
        return false;
    }

    const UsbUartPinPair* uart_pair = &usb_uart_pin_pairs[cfg->uart_pin_pair];
    const UsbUartControlPinPair* control_pair =
        &usb_uart_control_pin_pairs[cfg->control_pin_pair];
    const gpio_num_t de_re_pin = usb_uart_de_re_pins[cfg->de_re_pin].pin;
    if(control_pair->rts != GPIO_NUM_NC &&
       (control_pair->rts == uart_pair->tx || control_pair->rts == uart_pair->rx ||
        control_pair->dtr == uart_pair->tx || control_pair->dtr == uart_pair->rx)) {
        return false;
    }
    if(de_re_pin != GPIO_NUM_NC &&
       (de_re_pin == uart_pair->tx || de_re_pin == uart_pair->rx ||
        de_re_pin == control_pair->rts || de_re_pin == control_pair->dtr)) {
        return false;
    }
    return true;
}

static bool usb_uart_release_gpio(gpio_num_t pin) {
    if(pin == GPIO_NUM_NC) return true;

    bool success = true;
    esp_err_t err = gpio_reset_pin(pin);
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "GPIO %d reset failed: %s", (int)pin, esp_err_to_name(err));
        success = false;
    }
    err = gpio_set_direction(pin, GPIO_MODE_INPUT);
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "GPIO %d input restore failed: %s", (int)pin, esp_err_to_name(err));
        success = false;
    }
    err = gpio_set_pull_mode(pin, GPIO_FLOATING);
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "GPIO %d floating restore failed: %s", (int)pin, esp_err_to_name(err));
        success = false;
    }
    return success;
}

static bool usb_uart_configure_output(gpio_num_t pin, int initial_level) {
    if(pin == GPIO_NUM_NC) return true;
    const esp_err_t level_err = gpio_set_level(pin, initial_level);
    if(level_err != ESP_OK) {
        FURI_LOG_E(
            TAG, "GPIO %d output preload failed: %s", (int)pin, esp_err_to_name(level_err));
        return false;
    }

    gpio_config_t config = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    const esp_err_t err = gpio_config(&config);
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "GPIO %d output init failed: %s", (int)pin, esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool usb_uart_usb_mounted(void) {
#if CONFIG_IDF_TARGET_ESP32S3
    return tud_mounted();
#else
    return true;
#endif
}

/* The CDC line state survives a cable pull; an unmounted port asserts nothing. */
static uint8_t usb_uart_get_ctrl_lines(uint8_t usb_channel) {
    if(!usb_uart_usb_mounted()) return 0;
    return furi_hal_cdc_get_ctrl_line_state(usb_channel);
}

static bool usb_uart_release_control_pair(UsbUartBridge* bridge) {
    if(bridge->active_control_pin_pair >= COUNT_OF(usb_uart_control_pin_pairs)) return true;

    const UsbUartControlPinPair* pair =
        &usb_uart_control_pin_pairs[bridge->active_control_pin_pair];
    bool success = true;
    if(pair->rts != GPIO_NUM_NC && gpio_set_level(pair->rts, 1) != ESP_OK) {
        FURI_LOG_E(TAG, "Could not deassert RTS GPIO");
        success = false;
    }
    if(pair->dtr != GPIO_NUM_NC && gpio_set_level(pair->dtr, 1) != ESP_OK) {
        FURI_LOG_E(TAG, "Could not deassert DTR GPIO");
        success = false;
    }
    success = usb_uart_release_gpio(pair->rts) && success;
    success = usb_uart_release_gpio(pair->dtr) && success;
    if(success) {
        usb_uart_unreserve_gpio(pair->rts);
        usb_uart_unreserve_gpio(pair->dtr);
        bridge->active_control_pin_pair = UINT8_MAX;
    }
    return success;
}

static bool usb_uart_release_de_re(UsbUartBridge* bridge) {
    if(bridge->active_de_re_pin >= COUNT_OF(usb_uart_de_re_pins)) return true;

    const UsbUartDeRe* de_re = &usb_uart_de_re_pins[bridge->active_de_re_pin];
    const gpio_num_t pin = de_re->pin;
    bool success = true;
    if(pin != GPIO_NUM_NC && gpio_set_level(pin, !de_re->tx_level) != ESP_OK) {
        FURI_LOG_E(TAG, "Could not deassert DE/RE GPIO");
        success = false;
    }
    success = usb_uart_release_gpio(pin) && success;
    if(success) {
        usb_uart_unreserve_gpio(pin);
        bridge->active_de_re_pin = UINT8_MAX;
    }
    return success;
}

static bool usb_uart_release_uart_pair(UsbUartBridge* bridge) {
    if(bridge->active_uart_pin_pair >= COUNT_OF(usb_uart_pin_pairs)) return true;

    const UsbUartPinPair* pair = &usb_uart_pin_pairs[bridge->active_uart_pin_pair];
    bool success = usb_uart_release_gpio((gpio_num_t)pair->tx);
    success = usb_uart_release_gpio((gpio_num_t)pair->rx) && success;
    if(success) bridge->active_uart_pin_pair = UINT8_MAX;
    return success;
}

static bool usb_uart_release_control_pins(UsbUartBridge* bridge) {
    const bool control_released = usb_uart_release_control_pair(bridge);
    const bool de_re_released = usb_uart_release_de_re(bridge);
    return control_released && de_re_released;
}

static bool usb_uart_apply_pin_config(UsbUartBridge* bridge, const UsbUartConfig* cfg) {
    const bool uart_pair_changed = cfg->uart_pin_pair != bridge->active_uart_pin_pair;
    const bool control_pair_changed = cfg->control_pin_pair != bridge->active_control_pin_pair;
    const bool de_re_pin_changed = cfg->de_re_pin != bridge->active_de_re_pin;

    if(uart_pair_changed || control_pair_changed || de_re_pin_changed) {
        const esp_err_t wait_err = uart_wait_tx_done(
            BOARD_USB_UART_NUM,
            usb_uart_tx_timeout_ticks(USB_UART_TX_DRAIN_BYTES, bridge->applied_baudrate));
        if(wait_err != ESP_OK) {
            FURI_LOG_E(TAG, "Timed out draining UART before pin configuration change");
            return false;
        }
    }

    if(control_pair_changed && !usb_uart_release_control_pair(bridge)) {
        FURI_LOG_E(TAG, "Could not release previous RTS/DTR GPIO pair");
        return false;
    }
    if(de_re_pin_changed && !usb_uart_release_de_re(bridge)) {
        FURI_LOG_E(TAG, "Could not release previous DE/RE GPIO");
        return false;
    }
    if(uart_pair_changed && !usb_uart_release_uart_pair(bridge)) {
        FURI_LOG_E(TAG, "Could not release previous UART GPIO pair");
        return false;
    }

    if(uart_pair_changed) {
        const UsbUartPinPair* pin_pair = &usb_uart_pin_pairs[cfg->uart_pin_pair];
        bridge->active_uart_pin_pair = cfg->uart_pin_pair;
        const esp_err_t err = uart_set_pin(
            BOARD_USB_UART_NUM,
            pin_pair->tx,
            pin_pair->rx,
            UART_PIN_NO_CHANGE,
            UART_PIN_NO_CHANGE);
        if(err != ESP_OK) {
            FURI_LOG_E(TAG, "UART pin selection failed: %s", esp_err_to_name(err));
            return false;
        }
    }

    if(control_pair_changed) {
        const UsbUartControlPinPair* new_pair =
            &usb_uart_control_pin_pairs[cfg->control_pin_pair];
        if(!usb_uart_reserve_gpio(new_pair->rts)) {
            bridge->active_control_pin_pair = UINT8_MAX;
            return false;
        }
        if(!usb_uart_reserve_gpio(new_pair->dtr)) {
            usb_uart_unreserve_gpio(new_pair->rts);
            bridge->active_control_pin_pair = UINT8_MAX;
            return false;
        }
        bridge->active_control_pin_pair = cfg->control_pin_pair;
        const uint8_t control_state = usb_uart_get_ctrl_lines(cfg->usb_channel);
        if(!usb_uart_configure_output(
               new_pair->rts, (control_state & CdcCtrlLineRTS) == 0) ||
           !usb_uart_configure_output(
               new_pair->dtr, (control_state & CdcCtrlLineDTR) == 0)) {
            usb_uart_unreserve_gpio(new_pair->rts);
            usb_uart_unreserve_gpio(new_pair->dtr);
            bridge->active_control_pin_pair = UINT8_MAX;
            return false;
        }
    }

    if(de_re_pin_changed) {
        const UsbUartDeRe* de_re = &usb_uart_de_re_pins[cfg->de_re_pin];
        if(!usb_uart_reserve_gpio(de_re->pin)) return false;
        bridge->active_de_re_pin = cfg->de_re_pin;
        if(!usb_uart_configure_output(de_re->pin, !de_re->tx_level)) {
            usb_uart_unreserve_gpio(de_re->pin);
            bridge->active_de_re_pin = UINT8_MAX;
            return false;
        }
    }

    const UsbUartControlPinPair* control_pair =
        &usb_uart_control_pin_pairs[cfg->control_pin_pair];
    if(control_pair->rts != GPIO_NUM_NC) {
        const uint8_t control_state = usb_uart_get_ctrl_lines(cfg->usb_channel);
        if(gpio_set_level(control_pair->rts, (control_state & CdcCtrlLineRTS) == 0) != ESP_OK ||
           gpio_set_level(control_pair->dtr, (control_state & CdcCtrlLineDTR) == 0) != ESP_OK) {
            FURI_LOG_E(TAG, "USB-UART RTS/DTR update failed");
            return false;
        }
    }
    return true;
}

static TickType_t usb_uart_tx_timeout_ticks(size_t byte_count, uint32_t baudrate) {
    if(baudrate == 0) baudrate = 115200;
    const uint64_t duration_ms =
        ((uint64_t)byte_count * 12U * 1000U + baudrate - 1U) / baudrate + 100U;
    const uint32_t bounded_ms =
        duration_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)duration_ms;
    TickType_t ticks = pdMS_TO_TICKS(bounded_ms);
    return ticks == 0 ? 1 : ticks;
}

static bool usb_uart_write_to_serial(
    UsbUartBridge* bridge,
    const UsbUartConfig* cfg,
    const uint8_t* data,
    size_t length) {
    const gpio_num_t de_re_pin = usb_uart_de_re_pins[cfg->de_re_pin].pin;
    const uint32_t de_re_tx = usb_uart_de_re_pins[cfg->de_re_pin].tx_level;
    const uint32_t de_re_idle = !de_re_tx;
    if(de_re_pin != GPIO_NUM_NC && gpio_set_level(de_re_pin, de_re_tx) != ESP_OK) {
        FURI_LOG_E(TAG, "Could not assert DE/RE GPIO");
        return false;
    }

    size_t written_total = 0;
    size_t zero_write_retries = 0;
    while(written_total < length) {
        const int written = uart_write_bytes(
            BOARD_USB_UART_NUM, data + written_total, length - written_total);
        if(written < 0) {
            FURI_LOG_E(TAG, "UART write failed");
            if(de_re_pin != GPIO_NUM_NC) gpio_set_level(de_re_pin, de_re_idle);
            return false;
        }
        if(written == 0) {
            if(++zero_write_retries > 20) {
                FURI_LOG_E(TAG, "UART TX stalled while draining USB packet");
                if(de_re_pin != GPIO_NUM_NC) gpio_set_level(de_re_pin, de_re_idle);
                return false;
            }
            furi_delay_ms(1);
            continue;
        }
        written_total += (size_t)written;
    }

    if(de_re_pin != GPIO_NUM_NC) {
        const esp_err_t err = uart_wait_tx_done(
            BOARD_USB_UART_NUM,
            usb_uart_tx_timeout_ticks(written_total, bridge->applied_baudrate));
        const esp_err_t gpio_err = gpio_set_level(de_re_pin, de_re_idle);
        if(err != ESP_OK || gpio_err != ESP_OK) {
            FURI_LOG_E(TAG, "Failed to finish DE/RE transmission");
            return false;
        }
    }

    furi_check(furi_mutex_acquire(bridge->mutex, FuriWaitForever) == FuriStatusOk);
    bridge->state.tx_cnt += (uint32_t)written_total;
    furi_check(furi_mutex_release(bridge->mutex) == FuriStatusOk);
    return true;
}

static const char* usb_uart_status_text(UsbUartStatus status) {
    switch(status) {
    case UsbUartStatusActive:
        return "Active";
    case UsbUartStatusMemoryError:
        return "Out of memory";
    case UsbUartStatusUnsupported:
        return "Not supported";
    case UsbUartStatusUsbBusy:
        return "USB busy";
    case UsbUartStatusUartBusy:
        return "UART busy";
    case UsbUartStatusUsbError:
        return "USB init failed";
    case UsbUartStatusUartError:
        return "UART init failed";
    case UsbUartStatusCleanupError:
        return "Cleanup failed";
    default:
        return "Starting";
    }
}

const char* usb_uart_get_status_text(UsbUartStatus status) {
    return usb_uart_status_text(status);
}

static UsbUartLineCodingResult usb_uart_apply_line_coding(UsbUartBridge* bridge) {
    UsbUartConfig cfg;
    furi_check(furi_mutex_acquire(bridge->mutex, FuriWaitForever) == FuriStatusOk);
    cfg = bridge->cfg;
    furi_check(furi_mutex_release(bridge->mutex) == FuriStatusOk);

    uint32_t baudrate = cfg.baudrate;
    uart_word_length_t data_bits = UART_DATA_8_BITS;
    uart_parity_t parity = UART_PARITY_DISABLE;
    uart_stop_bits_t stop_bits = UART_STOP_BITS_1;

    if(baudrate == 0) {
        struct usb_cdc_line_coding line;
        if(!furi_hal_cdc_get_port_settings(cfg.usb_channel, &line) || line.dwDTERate == 0)
            return UsbUartLineCodingUnsupported;

        bool supported = line.dwDTERate >= USB_UART_BAUDRATE_MIN &&
                         line.dwDTERate <= USB_UART_BAUDRATE_MAX;
        baudrate = line.dwDTERate;
        switch(line.bDataBits) {
        case 5:
            data_bits = UART_DATA_5_BITS;
            break;
        case 6:
            data_bits = UART_DATA_6_BITS;
            break;
        case 7:
            data_bits = UART_DATA_7_BITS;
            break;
        case 8:
            data_bits = UART_DATA_8_BITS;
            break;
        default:
            supported = false;
            break;
        }

        switch(line.bParityType) {
        case 0:
            parity = UART_PARITY_DISABLE;
            break;
        case 1:
            parity = UART_PARITY_ODD;
            break;
        case 2:
            parity = UART_PARITY_EVEN;
            break;
        default:
            supported = false;
            break;
        }

        switch(line.bCharFormat) {
        case 0:
            stop_bits = UART_STOP_BITS_1;
            break;
        case 1:
            stop_bits = UART_STOP_BITS_1_5;
            break;
        case 2:
            stop_bits = UART_STOP_BITS_2;
            break;
        default:
            supported = false;
            break;
        }

        if(!supported) {
            /* Polled every loop: report each rejected setting once, keep the last good one. */
            const bool already_reported =
                bridge->line_coding_rejected &&
                bridge->rejected_line_coding.dwDTERate == line.dwDTERate &&
                bridge->rejected_line_coding.bDataBits == line.bDataBits &&
                bridge->rejected_line_coding.bParityType == line.bParityType &&
                bridge->rejected_line_coding.bCharFormat == line.bCharFormat;
            if(!already_reported) {
                FURI_LOG_W(
                    TAG,
                    "Ignoring unsupported host line coding: %lu baud, %u data bits, parity %u, stop %u",
                    (unsigned long)line.dwDTERate,
                    (unsigned)line.bDataBits,
                    (unsigned)line.bParityType,
                    (unsigned)line.bCharFormat);
                bridge->line_coding_rejected = true;
                bridge->rejected_line_coding = line;
            }
            return UsbUartLineCodingUnsupported;
        }
        bridge->line_coding_rejected = false;
    }

    uart_config_t config = {
        .baud_rate = (int)baudrate,
        .data_bits = data_bits,
        .parity = parity,
        .stop_bits = stop_bits,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    if(bridge->applied_config_valid && bridge->applied_baudrate == baudrate &&
       bridge->applied_data_bits == config.data_bits &&
       bridge->applied_parity == config.parity &&
       bridge->applied_stop_bits == config.stop_bits) {
        return UsbUartLineCodingApplied;
    }

    if(bridge->applied_config_valid) {
        const esp_err_t wait_err = uart_wait_tx_done(
            BOARD_USB_UART_NUM,
            usb_uart_tx_timeout_ticks(USB_UART_TX_DRAIN_BYTES, bridge->applied_baudrate));
        if(wait_err != ESP_OK) {
            FURI_LOG_E(TAG, "Timed out draining UART before line-coding change");
            return UsbUartLineCodingError;
        }
    }

    esp_err_t err = uart_param_config(BOARD_USB_UART_NUM, &config);
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "UART configuration failed: %s", esp_err_to_name(err));
        return UsbUartLineCodingError;
    }

    furi_check(furi_mutex_acquire(bridge->mutex, FuriWaitForever) == FuriStatusOk);
    bridge->state.baudrate_cur = baudrate;
    furi_check(furi_mutex_release(bridge->mutex) == FuriStatusOk);
    bridge->applied_config_valid = true;
    bridge->applied_baudrate = baudrate;
    bridge->applied_data_bits = config.data_bits;
    bridge->applied_parity = config.parity;
    bridge->applied_stop_bits = config.stop_bits;
    return UsbUartLineCodingApplied;
}

static void usb_uart_set_status(UsbUartBridge* bridge, UsbUartStatus status) {
    furi_check(furi_mutex_acquire(bridge->mutex, FuriWaitForever) == FuriStatusOk);
    bridge->state.status = status;
    furi_check(furi_mutex_release(bridge->mutex) == FuriStatusOk);
}

static void usb_uart_wake(UsbUartBridge* bridge) {
    if(bridge->wake_sem) {
        furi_semaphore_release(bridge->wake_sem);
    }
}

/* Like the original: bridging CDC0 takes that port, bridging CDC1 leaves CDC0
 * to its previous owner. Here that is qFlipper when it was running (it also
 * serves the CLI on this port), otherwise the CLI. Only transitions act, so a
 * failed restart is not retried every loop. */
static void usb_uart_update_cdc0_owner(UsbUartBridge* bridge, uint8_t usb_channel) {
    if(bridge->cdc0_owner_channel == usb_channel) return;
    bridge->cdc0_owner_channel = usb_channel;
    const bool take_cdc0 = usb_channel == 0;

    if(bridge->qflipper_was_active) {
        /* The CLI stays as it was: enabled, with qFlipper holding CDC0. */
        if(take_cdc0 && qflipper_bridge_is_active()) {
            qflipper_bridge_stop();
            bridge->qflipper_stopped = true;
        } else if(!take_cdc0 && bridge->qflipper_stopped) {
            if(qflipper_bridge_start()) {
                bridge->qflipper_stopped = false;
            } else {
                FURI_LOG_E(TAG, "Could not give CDC0 back to qFlipper");
            }
        }
    } else if(bridge->cli_vcp_was_enabled) {
        if(take_cdc0 && !bridge->cli_vcp_disabled) {
            cli_vcp_disable(bridge->cli_vcp);
            bridge->cli_vcp_disabled = true;
        } else if(!take_cdc0 && bridge->cli_vcp_disabled) {
            cli_vcp_enable(bridge->cli_vcp);
            bridge->cli_vcp_disabled = false;
        }
    }
}

static int32_t usb_uart_worker(void* context) {
    UsbUartBridge* bridge = context;
    uint8_t buffer[CDC_DATA_SZ];
    bool immediate_poll = false;

    while(!bridge->stopping) {
        furi_semaphore_acquire(bridge->wake_sem, immediate_poll ? 0 : 2);
        immediate_poll = false;
        if(bridge->stopping) break;

        UsbUartConfig cfg;
        furi_check(furi_mutex_acquire(bridge->mutex, FuriWaitForever) == FuriStatusOk);
        cfg = bridge->cfg;
        furi_check(furi_mutex_release(bridge->mutex) == FuriStatusOk);

        usb_uart_update_cdc0_owner(bridge, cfg.usb_channel);

        if(!usb_uart_apply_pin_config(bridge, &cfg)) {
            usb_uart_set_status(bridge, UsbUartStatusUartError);
            break;
        }

        const UsbUartLineCodingResult line_coding_result =
            usb_uart_apply_line_coding(bridge);
        if(line_coding_result == UsbUartLineCodingError ||
           (line_coding_result == UsbUartLineCodingUnsupported && cfg.baudrate != 0)) {
            usb_uart_set_status(bridge, UsbUartStatusUartError);
            break;
        }

        int32_t usb_size;
        for(size_t packet = 0;
            packet < 8 && !bridge->stopping &&
            (usb_size = furi_hal_cdc_receive(cfg.usb_channel, buffer, sizeof(buffer))) > 0;
            packet++) {
            if(!usb_uart_write_to_serial(bridge, &cfg, buffer, (size_t)usb_size)) {
                usb_uart_set_status(bridge, UsbUartStatusUartError);
                goto worker_done;
            }
            immediate_poll = true;
        }

        /* Data flows whenever the host has the device mounted, like the original
         * bridge. DTR only marks the port as open in the UI: tools such as esptool
         * talk to the target with DTR deasserted after toggling reset. */
        const bool usb_mounted = usb_uart_usb_mounted();
        const bool port_open =
            (usb_uart_get_ctrl_lines(cfg.usb_channel) & CdcCtrlLineDTR) != 0;
        furi_check(furi_mutex_acquire(bridge->mutex, FuriWaitForever) == FuriStatusOk);
        bridge->state.usb_connected = port_open;
        furi_check(furi_mutex_release(bridge->mutex) == FuriStatusOk);
        if(bridge->usb_mounted && !usb_mounted) {
            const esp_err_t flush_err = uart_flush_input(BOARD_USB_UART_NUM);
            if(flush_err != ESP_OK) {
                FURI_LOG_W(TAG, "Could not flush stale UART RX data: %s", esp_err_to_name(flush_err));
            }
            bridge->uart_rx_size = 0;
            bridge->uart_rx_offset = 0;
        }
        bridge->usb_mounted = usb_mounted;

        int uart_size;
        for(size_t packet = 0; packet < 8; packet++) {
            if(bridge->uart_rx_offset == bridge->uart_rx_size) {
                uart_size = uart_read_bytes(
                    BOARD_USB_UART_NUM, bridge->uart_rx_buffer, sizeof(bridge->uart_rx_buffer), 0);
                if(uart_size < 0) {
                    FURI_LOG_E(TAG, "UART read failed");
                    usb_uart_set_status(bridge, UsbUartStatusUartError);
                    goto worker_done;
                }
                if(uart_size == 0) break;
                bridge->uart_rx_size = (size_t)uart_size;
                bridge->uart_rx_offset = 0;
            }

            if(!usb_mounted) {
                /* No host to deliver to: drop it like an unplugged USB-UART adapter
                 * instead of replaying a stale backlog on the next plug-in. */
                bridge->uart_rx_size = 0;
                bridge->uart_rx_offset = 0;
                continue;
            }

            int32_t sent = furi_hal_cdc_send_nonblocking(
                cfg.usb_channel,
                bridge->uart_rx_buffer + bridge->uart_rx_offset,
                (uint16_t)(bridge->uart_rx_size - bridge->uart_rx_offset));
            if(sent < 0) {
                FURI_LOG_E(TAG, "USB CDC TX failed");
                usb_uart_set_status(bridge, UsbUartStatusUsbError);
                goto worker_done;
            }
            if(sent == 0) break;

            immediate_poll = true;
            bridge->uart_rx_offset += (size_t)sent;
            furi_check(furi_mutex_acquire(bridge->mutex, FuriWaitForever) == FuriStatusOk);
            bridge->state.rx_cnt += (uint32_t)sent;
            furi_check(furi_mutex_release(bridge->mutex) == FuriStatusOk);

            if(bridge->uart_rx_offset == bridge->uart_rx_size) {
                bridge->uart_rx_size = 0;
                bridge->uart_rx_offset = 0;
            }
        }
    }

worker_done:
    if(!usb_uart_release_control_pins(bridge)) {
        FURI_LOG_E(TAG, "Could not release all USB-UART control GPIOs when worker stopped");
    }
    if(bridge->active_uart_pin_pair < COUNT_OF(usb_uart_pin_pairs)) {
        const esp_err_t tx_err = uart_wait_tx_done(BOARD_USB_UART_NUM, pdMS_TO_TICKS(100));
        if(tx_err == ESP_OK) {
            const UsbUartPinPair* pair = &usb_uart_pin_pairs[bridge->active_uart_pin_pair];
            if(usb_uart_release_gpio((gpio_num_t)pair->tx) &&
               usb_uart_release_gpio((gpio_num_t)pair->rx)) {
                bridge->active_uart_pin_pair = UINT8_MAX;
            } else {
                FURI_LOG_E(TAG, "Could not release USB-UART data GPIOs when worker stopped");
            }
        } else {
            FURI_LOG_E(
                TAG,
                "Could not flush UART before releasing pins: %s",
                esp_err_to_name(tx_err));
        }
    }
    return 0;
}

static bool usb_uart_cleanup(UsbUartBridge* bridge) {
    if(bridge->thread) {
        bridge->stopping = true;
        usb_uart_wake(bridge);
        furi_thread_join(bridge->thread);
        furi_thread_free(bridge->thread);
        bridge->thread = NULL;
    }

    bool uart_safe_to_release = !bridge->uart_installed;
    if(bridge->uart_installed) {
        esp_err_t err = uart_wait_tx_done(BOARD_USB_UART_NUM, pdMS_TO_TICKS(100));
        if(err != ESP_OK) {
            FURI_LOG_W(TAG, "Timed out flushing UART TX: %s", esp_err_to_name(err));
        }
        /* A flush timeout is recoverable if deleting the driver succeeds. */
        err = uart_driver_delete(BOARD_USB_UART_NUM);
        if(err != ESP_OK) {
            FURI_LOG_E(TAG, "Could not remove UART driver: %s", esp_err_to_name(err));
            uart_safe_to_release = false;
        } else {
            bridge->uart_installed = false;
            uart_safe_to_release = true;
        }
    }

    if(!usb_uart_release_control_pins(bridge)) {
        FURI_LOG_E(TAG, "One or more USB-UART control GPIOs could not be released");
    }

    if(bridge->active_uart_pin_pair < COUNT_OF(usb_uart_pin_pairs)) {
        if(!uart_safe_to_release) {
            FURI_LOG_W(TAG, "Detaching UART GPIOs despite driver cleanup failure");
        }
        if(!usb_uart_release_uart_pair(bridge)) {
            FURI_LOG_E(TAG, "One or more USB-UART data GPIOs could not be released");
        }
    }

    if(bridge->uart_claimed && uart_safe_to_release) {
        if(furi_hal_uart_shared_release()) {
            bridge->uart_claimed = false;
        } else {
            FURI_LOG_E(TAG, "Could not restore shared UART pins; keeping UART locked");
            uart_safe_to_release = false;
        }
    } else if(bridge->uart_claimed) {
        FURI_LOG_E(TAG, "Keeping shared UART locked after driver cleanup failure");
    }

    if(bridge->sleep_inhibited && !bridge->uart_claimed && uart_safe_to_release) {
        bridge->sleep_inhibited = false;
        furi_hal_power_light_sleep_inhibit_exit();
    } else if(bridge->sleep_inhibited) {
        FURI_LOG_E(TAG, "Keeping light-sleep inhibitor after UART cleanup failure");
    }

    if(bridge->usb_locked) {
        bridge->usb_locked = false;
        furi_hal_usb_unlock();
    }
    if(bridge->usb_configured) {
        if(furi_hal_usb_set_config(bridge->previous_usb_config, NULL)) {
            bridge->usb_configured = false;
        } else {
            FURI_LOG_E(TAG, "Could not restore USB configuration");
        }
    }

    if(bridge->usb_configured) goto done;

    /* Give CDC0 back to its owner from before the bridge. */
    if(bridge->qflipper_stopped) {
        bridge->qflipper_stopped = false;
        if(!qflipper_bridge_start()) {
            FURI_LOG_E(TAG, "Could not restart qFlipper bridge after UART bridge");
        }
    }

    /* Nobody else uses the composite now (we installed it, qFlipper failed to
     * restart, or the PC closed qFlipper during the session): remove it so
     * USB-Serial-JTAG comes back. Teardown also ends a CLI shell on CDC0. */
    if(bridge->composite_used && !qflipper_bridge_is_active()) {
        if(!furi_hal_usb_composite_uninstall()) {
            FURI_LOG_E(TAG, "Could not uninstall USB composite after UART bridge");
            goto done;
        }
    }
    bridge->composite_used = false;

    /* Only now, with no host line state left on CDC0, so no stray shell starts. */
    if(bridge->cli_vcp) {
        if(bridge->cli_vcp_disabled) cli_vcp_enable(bridge->cli_vcp);
        bridge->cli_vcp_disabled = false;
        furi_record_close(RECORD_CLI_VCP);
        bridge->cli_vcp = NULL;
    }

done:
    return !bridge->uart_claimed && !bridge->uart_installed && !bridge->usb_locked &&
           !bridge->usb_configured && !bridge->composite_used && !bridge->sleep_inhibited &&
           !bridge->cli_vcp && !bridge->qflipper_stopped &&
           bridge->active_uart_pin_pair >= COUNT_OF(usb_uart_pin_pairs) &&
           bridge->active_control_pin_pair >= COUNT_OF(usb_uart_control_pin_pairs) &&
           bridge->active_de_re_pin >= COUNT_OF(usb_uart_de_re_pins);
}

UsbUartBridge* usb_uart_enable(UsbUartConfig* cfg) {
    if(!cfg || !usb_uart_config_is_valid(cfg)) {
        FURI_LOG_E(TAG, "Rejected invalid USB-UART configuration");
        return NULL;
    }

    if(usb_uart_pending_cleanup_bridge) {
        UsbUartBridge* pending_bridge = usb_uart_pending_cleanup_bridge;
        if(usb_uart_cleanup(pending_bridge)) {
            usb_uart_pending_cleanup_bridge = NULL;
            if(pending_bridge->wake_sem) furi_semaphore_free(pending_bridge->wake_sem);
            if(pending_bridge->mutex) furi_mutex_free(pending_bridge->mutex);
            free(pending_bridge);
        } else {
            usb_uart_set_status(pending_bridge, UsbUartStatusCleanupError);
            FURI_LOG_E(TAG, "Previous USB-UART cleanup is still pending");
            return pending_bridge;
        }
    }

    UsbUartBridge* bridge = calloc(1, sizeof(UsbUartBridge));
    if(!bridge) return NULL;

    bridge->cfg = *cfg;
    bridge->active_uart_pin_pair = UINT8_MAX;
    bridge->active_control_pin_pair = UINT8_MAX;
    bridge->active_de_re_pin = UINT8_MAX;
    bridge->state.status = UsbUartStatusStarting;
    bridge->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    bridge->wake_sem = furi_semaphore_alloc(1, 0);
    if(!bridge->mutex || !bridge->wake_sem) {
        if(bridge->wake_sem) furi_semaphore_free(bridge->wake_sem);
        if(bridge->mutex) furi_mutex_free(bridge->mutex);
        free(bridge);
        return NULL;
    }

#if !defined(BOARD_HAS_USB_UART_BRIDGE) || !BOARD_HAS_USB_UART_BRIDGE
    usb_uart_set_status(bridge, UsbUartStatusUnsupported);
    return bridge;
#else
    if(furi_hal_usb_is_locked()) {
        usb_uart_set_status(bridge, UsbUartStatusUsbBusy);
        return bridge;
    }

    /* The GPIO app keeps Expansion disabled for its whole lifetime (as on the
     * original Flipper), so the shared UART is only busy if someone else holds it. */
    FuriHalUartSharedAcquireResult acquire_result = furi_hal_uart_shared_acquire(0);
    if(acquire_result != FuriHalUartSharedAcquireAcquired) {
        usb_uart_set_status(
            bridge,
            acquire_result == FuriHalUartSharedAcquireBusy ? UsbUartStatusUartBusy :
                                                             UsbUartStatusUartError);
        return bridge;
    }
    bridge->uart_claimed = true;
    furi_hal_power_light_sleep_inhibit_enter();
    bridge->sleep_inhibited = true;

    if(furi_hal_usb_is_locked()) {
        usb_uart_set_status(bridge, UsbUartStatusUsbBusy);
        usb_uart_cleanup(bridge);
        return bridge;
    }

    bridge->previous_usb_config = furi_hal_usb_get_config();
    if(bridge->previous_usb_config && bridge->previous_usb_config != &usb_cdc_single &&
       bridge->previous_usb_config != &usb_cdc_dual) {
        usb_uart_set_status(bridge, UsbUartStatusUsbBusy);
        usb_uart_cleanup(bridge);
        return bridge;
    }

    if(!furi_hal_usb_composite_install(0, 0, NULL, NULL)) {
        usb_uart_set_status(bridge, UsbUartStatusUsbError);
        usb_uart_cleanup(bridge);
        return bridge;
    }
    bridge->composite_used = true;

    bridge->qflipper_was_active = qflipper_bridge_is_active();
    bridge->cli_vcp = furi_record_open(RECORD_CLI_VCP);
    bridge->cli_vcp_was_enabled = cli_vcp_is_enabled(bridge->cli_vcp);
    bridge->cdc0_owner_channel = UINT8_MAX;
    usb_uart_update_cdc0_owner(bridge, cfg->usb_channel);

    if(!furi_hal_usb_set_config(&usb_cdc_dual, NULL)) {
        usb_uart_set_status(bridge, UsbUartStatusUsbError);
        usb_uart_cleanup(bridge);
        return bridge;
    }
    bridge->usb_configured = true;

    furi_hal_usb_lock();
    bridge->usb_locked = true;
    esp_err_t err =
        uart_driver_install(BOARD_USB_UART_NUM, 1024, USB_UART_TX_BUFFER_SIZE, 0, NULL, 0);
    if(err == ESP_OK) {
        bridge->uart_installed = true;
        err = uart_param_config(BOARD_USB_UART_NUM, &(uart_config_t){
            .baud_rate = 115200,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        });
    }
    if(err == ESP_OK) {
        const UsbUartPinPair* pin_pair = &usb_uart_pin_pairs[cfg->uart_pin_pair];
        bridge->active_uart_pin_pair = cfg->uart_pin_pair;
        err = uart_set_pin(
            BOARD_USB_UART_NUM,
            pin_pair->tx,
            pin_pair->rx,
            UART_PIN_NO_CHANGE,
            UART_PIN_NO_CHANGE);
    }
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "UART init failed: %s", esp_err_to_name(err));
        usb_uart_set_status(bridge, UsbUartStatusUartError);
        usb_uart_cleanup(bridge);
        return bridge;
    }

    const UsbUartLineCodingResult line_coding_result = usb_uart_apply_line_coding(bridge);
    if(line_coding_result == UsbUartLineCodingError ||
       (line_coding_result == UsbUartLineCodingUnsupported && cfg->baudrate != 0)) {
        usb_uart_set_status(bridge, UsbUartStatusUartError);
        usb_uart_cleanup(bridge);
        return bridge;
    }

    bridge->thread = furi_thread_alloc_ex("UsbUartWorker", 3072, usb_uart_worker, bridge);
    if(!bridge->thread) {
        usb_uart_set_status(bridge, UsbUartStatusMemoryError);
        usb_uart_cleanup(bridge);
        return bridge;
    }
    bridge->state.status = UsbUartStatusActive;
    furi_thread_start(bridge->thread);
    return bridge;
#endif
}

bool usb_uart_disable(UsbUartBridge* bridge) {
    furi_assert(bridge);

    if(!usb_uart_cleanup(bridge)) {
        usb_uart_set_status(bridge, UsbUartStatusCleanupError);
        usb_uart_pending_cleanup_bridge = bridge;
        FURI_LOG_E(TAG, "USB-UART cleanup incomplete; bridge retained for retry");
        return false;
    }
    if(usb_uart_pending_cleanup_bridge == bridge) {
        usb_uart_pending_cleanup_bridge = NULL;
    }
    if(bridge->wake_sem) furi_semaphore_free(bridge->wake_sem);
    if(bridge->mutex) furi_mutex_free(bridge->mutex);
    free(bridge);
    return true;
}

void usb_uart_set_config(UsbUartBridge* bridge, UsbUartConfig* cfg) {
    furi_assert(bridge);
    furi_assert(cfg);
    if(!usb_uart_config_is_valid(cfg)) {
        FURI_LOG_E(TAG, "Rejected invalid USB-UART configuration");
        return;
    }
    furi_check(furi_mutex_acquire(bridge->mutex, FuriWaitForever) == FuriStatusOk);
    bridge->cfg = *cfg;
    furi_check(furi_mutex_release(bridge->mutex) == FuriStatusOk);
    usb_uart_wake(bridge);
}

void usb_uart_get_config(UsbUartBridge* bridge, UsbUartConfig* cfg) {
    furi_assert(bridge);
    furi_assert(cfg);
    furi_check(furi_mutex_acquire(bridge->mutex, FuriWaitForever) == FuriStatusOk);
    *cfg = bridge->cfg;
    furi_check(furi_mutex_release(bridge->mutex) == FuriStatusOk);
}

void usb_uart_get_state(UsbUartBridge* bridge, UsbUartState* state) {
    furi_assert(bridge);
    furi_assert(state);
    furi_check(furi_mutex_acquire(bridge->mutex, FuriWaitForever) == FuriStatusOk);
    *state = bridge->state;
    furi_check(furi_mutex_release(bridge->mutex) == FuriStatusOk);
}
