#include "furi_hal_nrf24.h"

#include <esp_log.h>
#include <furi.h>
#include <furi_hal_nvs_blob.h>
#include <furi_hal_power.h>
#include <furi_hal_resources.h>
#include <furi_hal_spi.h>
#include <furi_hal_subghz.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include "boards/board.h"

static const char* TAG = "FuriHalNrf24";

#if defined(BOARD_NRF24_EXT_MODULE)
#define FURI_HAL_NRF24_HAS_EXT 1
#else
#define FURI_HAL_NRF24_HAS_EXT 0
#endif

#define FURI_HAL_NRF24_NVS_NAMESPACE "nrf24"
#define FURI_HAL_NRF24_NVS_KEY       "ext"
#define FURI_HAL_NRF24_BLOB_MAGIC    0xE2u
#define FURI_HAL_NRF24_BLOB_SIZE     4u

/* NRF24 SPI: W_REGISTER(CONFIG) = 0 -> power down (also valid when no chip answers). */
#define NRF24_CMD_W_CONFIG  0x20u
#define NRF24_CMD_W_REGISTER 0x20u
#define NRF24_CMD_R_REGISTER 0x00u
#define NRF24_REG_RF_CH      0x05u
#define NRF24_CONFIG_PWR_UP  0x02u

typedef struct {
    FuriHalNrf24ExtConfig config;
    bool config_loaded;
    /* Session reference count. Scenes nest their hardware use (e.g. the Jam scene keeps
     * its worker — and thus a session — alive while it pushes the Activity-scan scene,
     * which opens its own session): the route must survive until the LAST holder ends it,
     * so begin/end are balanced increments, routing only on 0->1 and tearing down on 1->0. */
    volatile uint8_t refcount;
    bool external; /* the open session routes the external module */
    GpioPin csn; /* header pins held while `external` */
    GpioPin ce;
    FuriMutex* mutex;
} FuriHalNrf24;

static FuriHalNrf24 furi_hal_nrf24 = {
    .csn = {.port = NULL, .pin = UINT16_MAX},
    .ce = {.port = NULL, .pin = UINT16_MAX},
};

/* Compared by pointer (furi_hal_gpio_reserve): one object. */
static const char furi_hal_nrf24_owner[] = "NRF24 ext";

#ifdef BOARD_PIN_NRF24_CE
static const GpioPin furi_hal_nrf24_board_ce = {.port = NULL, .pin = BOARD_PIN_NRF24_CE};
#else
static const GpioPin furi_hal_nrf24_board_ce = {.port = NULL, .pin = UINT16_MAX};
#endif

static FuriMutex* furi_hal_nrf24_lock(void) {
    static portMUX_TYPE create_lock = portMUX_INITIALIZER_UNLOCKED;
    FuriMutex* mutex = furi_hal_nrf24.mutex;
    if(!mutex) {
        FuriMutex* candidate = furi_mutex_alloc(FuriMutexTypeRecursive);
        furi_check(candidate);
        portENTER_CRITICAL(&create_lock);
        if(!furi_hal_nrf24.mutex) furi_hal_nrf24.mutex = candidate;
        mutex = furi_hal_nrf24.mutex;
        portEXIT_CRITICAL(&create_lock);
        if(mutex != candidate) furi_mutex_free(candidate);
    }
    furi_check(furi_mutex_acquire(mutex, FuriWaitForever) == FuriStatusOk);
    return mutex;
}

static void furi_hal_nrf24_unlock(FuriMutex* mutex) {
    furi_check(furi_mutex_release(mutex) == FuriStatusOk);
}

bool furi_hal_nrf24_ext_supported(void) {
    return FURI_HAL_NRF24_HAS_EXT;
}

bool furi_hal_nrf24_ext_pin_allowed(uint8_t pin) {
#if FURI_HAL_NRF24_HAS_EXT
    for(size_t i = 0; i < gpio_pins_count; i++) {
        if(gpio_pins[i].pin && gpio_pins[i].pin->pin == pin) return true;
    }
#else
    (void)pin;
#endif
    return false;
}

static bool furi_hal_nrf24_config_valid(const FuriHalNrf24ExtConfig* config) {
    return config->csn_pin != config->ce_pin && furi_hal_nrf24_ext_pin_allowed(config->csn_pin) &&
           furi_hal_nrf24_ext_pin_allowed(config->ce_pin);
}

static void furi_hal_nrf24_config_defaults(FuriHalNrf24ExtConfig* config) {
    config->use_external = false;
#if FURI_HAL_NRF24_HAS_EXT
    /* Distinct from the external Sub-GHz module's defaults (GPIO5/GPIO11) and from the
     * external IR transmitter pin (GPIO4). */
    config->csn_pin = BOARD_PIN_EXT_GPIO_4;
    config->ce_pin = BOARD_PIN_EXT_GPIO_5;
#else
    config->csn_pin = UINT8_MAX;
    config->ce_pin = UINT8_MAX;
#endif
}

static void furi_hal_nrf24_config_load_locked(void) {
    if(furi_hal_nrf24.config_loaded) return;

    FuriHalNrf24ExtConfig config;
    furi_hal_nrf24_config_defaults(&config);
#if FURI_HAL_NRF24_HAS_EXT
    uint8_t blob[FURI_HAL_NRF24_BLOB_SIZE] = {0};
    if(furi_hal_nvs_blob_read(
           FURI_HAL_NRF24_NVS_NAMESPACE, FURI_HAL_NRF24_NVS_KEY, blob, sizeof(blob)) &&
       blob[0] == FURI_HAL_NRF24_BLOB_MAGIC) {
        const FuriHalNrf24ExtConfig stored = {
            .use_external = blob[1] == 1,
            .csn_pin = blob[2],
            .ce_pin = blob[3],
        };
        if(furi_hal_nrf24_config_valid(&stored)) {
            config = stored;
        } else {
            ESP_LOGW(TAG, "Stored external-module pins are invalid; using defaults");
        }
    }
#endif
    furi_hal_nrf24.config = config;
    furi_hal_nrf24.config_loaded = true;
}

void furi_hal_nrf24_ext_get_config(FuriHalNrf24ExtConfig* config) {
    furi_check(config);
    FuriMutex* mutex = furi_hal_nrf24_lock();
    furi_hal_nrf24_config_load_locked();
    *config = furi_hal_nrf24.config;
    furi_hal_nrf24_unlock(mutex);
}

bool furi_hal_nrf24_ext_uses_pin(uint8_t pin) {
    if(!FURI_HAL_NRF24_HAS_EXT) return false;
    FuriMutex* mutex = furi_hal_nrf24_lock();
    furi_hal_nrf24_config_load_locked();
    const FuriHalNrf24ExtConfig* c = &furi_hal_nrf24.config;
    const bool used = c->use_external && (c->csn_pin == pin || c->ce_pin == pin);
    furi_hal_nrf24_unlock(mutex);
    return used;
}

bool furi_hal_nrf24_ext_set_config(const FuriHalNrf24ExtConfig* config) {
    furi_check(config);
    if(!FURI_HAL_NRF24_HAS_EXT || !furi_hal_nrf24_config_valid(config)) return false;
    /* Two modules cannot share a header pin (asked before our own lock, see Sub-GHz). */
    if(config->use_external &&
       (furi_hal_subghz_ext_uses_pin(config->csn_pin) ||
        furi_hal_subghz_ext_uses_pin(config->ce_pin))) {
        return false;
    }

    FuriMutex* mutex = furi_hal_nrf24_lock();
    furi_hal_nrf24_config_load_locked();
    FuriHalNrf24ExtConfig* current = &furi_hal_nrf24.config;
    const bool pins_changed =
        current->csn_pin != config->csn_pin || current->ce_pin != config->ce_pin;
    bool applied = true;
    if(furi_hal_nrf24.refcount > 0 && furi_hal_nrf24.external && pins_changed) {
        applied = false; /* the pads are in use by a running session */
    } else if(pins_changed || current->use_external != config->use_external) {
        *current = *config;
#if FURI_HAL_NRF24_HAS_EXT
        const uint8_t blob[FURI_HAL_NRF24_BLOB_SIZE] = {
            FURI_HAL_NRF24_BLOB_MAGIC,
            current->use_external ? 1 : 0,
            current->csn_pin,
            current->ce_pin,
        };
        (void)furi_hal_nvs_blob_write(
            FURI_HAL_NRF24_NVS_NAMESPACE, FURI_HAL_NRF24_NVS_KEY, blob, sizeof(blob));
#endif
    }
    furi_hal_nrf24_unlock(mutex);
    return applied;
}

const char* furi_hal_nrf24_ext_result_text(FuriHalNrf24ExtResult result) {
    switch(result) {
    case FuriHalNrf24ExtOk:
        return "OK";
    case FuriHalNrf24ExtUnsupported:
        return "Not supported on this board";
    case FuriHalNrf24ExtBadConfig:
        return "CSN and CE must be two different header pins";
    case FuriHalNrf24ExtBusy:
        return "NRF24 bus is busy";
    case FuriHalNrf24ExtPinInUse:
        return "A chosen pin is used by another feature";
    case FuriHalNrf24ExtNotDetected:
        return "No NRF24 detected on the chosen pins";
    default:
        return "Unknown error";
    }
}

bool furi_hal_nrf24_session_is_active(void) {
    return furi_hal_nrf24.refcount > 0;
}

bool furi_hal_nrf24_session_is_external(void) {
    return furi_hal_nrf24.refcount > 0 && furi_hal_nrf24.external;
}

const GpioPin* furi_hal_nrf24_get_ce_pin(void) {
    return (furi_hal_nrf24.refcount > 0 && furi_hal_nrf24.external) ? &furi_hal_nrf24.ce :
                                                                      &furi_hal_nrf24_board_ce;
}

static bool furi_hal_nrf24_power_down_board_module(void) {
#if defined(BOARD_HAS_NRF24) && BOARD_HAS_NRF24
    const uint8_t command[2] = {NRF24_CMD_W_CONFIG, 0x00};
    for(uint8_t attempt = 0; attempt < 3; attempt++) {
        furi_hal_spi_acquire(&furi_hal_spi_bus_handle_nrf24);
        const bool ok =
            furi_hal_spi_bus_tx(&furi_hal_spi_bus_handle_nrf24, command, sizeof(command), 100);
        furi_hal_spi_release(&furi_hal_spi_bus_handle_nrf24);
        if(ok) return true;
    }
    return false;
#else
    return true;
#endif
}

#if FURI_HAL_NRF24_HAS_EXT
/* Power the module routed by the NRF24 handle down. Harmless when no chip answers. */
static void furi_hal_nrf24_park_routed_module(void) {
    const uint8_t command[2] = {NRF24_CMD_W_CONFIG, 0x00};
    furi_hal_spi_acquire(&furi_hal_spi_bus_handle_nrf24);
    furi_hal_spi_bus_tx(&furi_hal_spi_bus_handle_nrf24, command, sizeof(command), 100);
    furi_hal_spi_release(&furi_hal_spi_bus_handle_nrf24);
}

/* Register round-trip probe on the already-routed SPI handle, mirroring nrf24_hw_probe()
 * (and furi_hal_subghz_ext's CC1101 probe): write two patterns to RF_CH and read them
 * back. Leaves the chip powered down either way. Caller holds the bus-free (no acquire
 * active) and the mutex. */
static bool furi_hal_nrf24_probe_routed_module(void) {
    const uint8_t pwr_up[2] = {NRF24_CMD_W_CONFIG, NRF24_CONFIG_PWR_UP};
    furi_hal_spi_acquire(&furi_hal_spi_bus_handle_nrf24);
    furi_hal_spi_bus_tx(&furi_hal_spi_bus_handle_nrf24, pwr_up, sizeof(pwr_up), 100);
    furi_hal_spi_release(&furi_hal_spi_bus_handle_nrf24);
    esp_rom_delay_us(5000);

    bool ok = false;
    for(uint32_t attempt = 0; attempt < 3 && !ok; attempt++) {
        const uint8_t w1[2] = {NRF24_CMD_W_REGISTER | NRF24_REG_RF_CH, 0x55};
        uint8_t r1_tx[2] = {NRF24_CMD_R_REGISTER | NRF24_REG_RF_CH, 0xFF};
        uint8_t r1_rx[2] = {0};
        const uint8_t w2[2] = {NRF24_CMD_W_REGISTER | NRF24_REG_RF_CH, 0x2A};
        uint8_t r2_tx[2] = {NRF24_CMD_R_REGISTER | NRF24_REG_RF_CH, 0xFF};
        uint8_t r2_rx[2] = {0};

        furi_hal_spi_acquire(&furi_hal_spi_bus_handle_nrf24);
        furi_hal_spi_bus_tx(&furi_hal_spi_bus_handle_nrf24, w1, sizeof(w1), 100);
        furi_hal_spi_bus_trx(&furi_hal_spi_bus_handle_nrf24, r1_tx, r1_rx, sizeof(r1_tx), 100);
        furi_hal_spi_bus_tx(&furi_hal_spi_bus_handle_nrf24, w2, sizeof(w2), 100);
        furi_hal_spi_bus_trx(&furi_hal_spi_bus_handle_nrf24, r2_tx, r2_rx, sizeof(r2_tx), 100);
        furi_hal_spi_release(&furi_hal_spi_bus_handle_nrf24);

        ok = (r1_rx[1] == 0x55 && r2_rx[1] == 0x2A);
        if(!ok) esp_rom_delay_us(2000);
    }
    furi_hal_nrf24_park_routed_module();
    return ok;
}

static void furi_hal_nrf24_ext_pads_idle(void) {
    const GpioPin csn = furi_hal_nrf24.csn;
    const GpioPin ce = furi_hal_nrf24.ce;
    furi_hal_gpio_init_owned(&csn, GpioModeInput, GpioPullNo, GpioSpeedLow, furi_hal_nrf24_owner);
    furi_hal_gpio_init_owned(&ce, GpioModeInput, GpioPullNo, GpioSpeedLow, furi_hal_nrf24_owner);
    furi_hal_gpio_unreserve(&csn, furi_hal_nrf24_owner);
    furi_hal_gpio_unreserve(&ce, furi_hal_nrf24_owner);
    furi_hal_nrf24.csn.pin = UINT16_MAX;
    furi_hal_nrf24.ce.pin = UINT16_MAX;
}

static void furi_hal_nrf24_end_external_locked(void);

static FuriHalNrf24ExtResult furi_hal_nrf24_begin_external_locked(
    const FuriHalNrf24ExtConfig* config) {
    if(!furi_hal_nrf24_config_valid(config)) return FuriHalNrf24ExtBadConfig;

    const GpioPin csn = {.port = NULL, .pin = config->csn_pin};
    const GpioPin ce = {.port = NULL, .pin = config->ce_pin};
    if(!furi_hal_gpio_reserve(&csn, furi_hal_nrf24_owner)) {
        ESP_LOGW(
            TAG,
            "External module: GPIO%u is in use by %s",
            (unsigned)csn.pin,
            furi_hal_gpio_get_reservation(&csn));
        return FuriHalNrf24ExtPinInUse;
    }
    if(!furi_hal_gpio_reserve(&ce, furi_hal_nrf24_owner)) {
        ESP_LOGW(
            TAG,
            "External module: GPIO%u is in use by %s",
            (unsigned)ce.pin,
            furi_hal_gpio_get_reservation(&ce));
        furi_hal_gpio_unreserve(&csn, furi_hal_nrf24_owner);
        return FuriHalNrf24ExtPinInUse;
    }

    /* The board's module is powered down and deselected before the bus is handed over. */
    furi_hal_gpio_write(&furi_hal_nrf24_board_ce, false);
    furi_hal_nrf24_park_routed_module();

    furi_hal_nrf24.csn.pin = config->csn_pin;
    furi_hal_nrf24.ce.pin = config->ce_pin;
    if(!furi_hal_spi_bus_handle_set_cs(&furi_hal_spi_bus_handle_nrf24, &furi_hal_nrf24.csn)) {
        furi_hal_gpio_unreserve(&csn, furi_hal_nrf24_owner);
        furi_hal_gpio_unreserve(&ce, furi_hal_nrf24_owner);
        furi_hal_nrf24.csn.pin = UINT16_MAX;
        furi_hal_nrf24.ce.pin = UINT16_MAX;
        return FuriHalNrf24ExtBusy;
    }
    /* The SPI driver let go of the board module's CSN pad: keep it high (deselected) and its
     * CE low for the whole session, MISO is shared. */
    furi_hal_gpio_init(&gpio_nrf24_cs, GpioModeOutputPushPull, GpioPullNo, GpioSpeedLow);
    furi_hal_gpio_write(&gpio_nrf24_cs, true);
    furi_hal_gpio_init(&furi_hal_nrf24_board_ce, GpioModeOutputPushPull, GpioPullNo, GpioSpeedLow);
    furi_hal_gpio_write(&furi_hal_nrf24_board_ce, false);

    furi_hal_gpio_init_owned(
        &furi_hal_nrf24.ce,
        GpioModeOutputPushPull,
        GpioPullUp,
        GpioSpeedVeryHigh,
        furi_hal_nrf24_owner);
    furi_hal_gpio_write(&furi_hal_nrf24.ce, false);
    /* Automatic light sleep would otherwise pull the (header) CSN pad low. */
    (void)furi_hal_power_light_sleep_hold_pin_high(csn.pin, true);

    if(!furi_hal_nrf24_probe_routed_module()) {
        ESP_LOGW(
            TAG,
            "External module: no NRF24 on CSN=GPIO%u CE=GPIO%u",
            (unsigned)csn.pin,
            (unsigned)ce.pin);
        furi_hal_nrf24_end_external_locked();
        return FuriHalNrf24ExtNotDetected;
    }

    furi_hal_nrf24.external = true;
    ESP_LOGI(
        TAG,
        "External NRF24 routed: CSN=GPIO%u CE=GPIO%u",
        (unsigned)csn.pin,
        (unsigned)ce.pin);
    return FuriHalNrf24ExtOk;
}

static void furi_hal_nrf24_end_external_locked(void) {
    furi_hal_gpio_write(&furi_hal_nrf24.ce, false);
    furi_hal_nrf24_park_routed_module();

    bool restored = false;
    for(uint32_t i = 0; i < 100 && !restored; i++) {
        restored = furi_hal_spi_bus_handle_set_cs(&furi_hal_spi_bus_handle_nrf24, &gpio_nrf24_cs);
        if(!restored) furi_delay_ms(5);
    }
    if(!restored) {
        /* The handle is stuck inside a transaction; staying routed keeps things consistent. */
        ESP_LOGE(TAG, "External module: SPI handle busy, session stays open");
        return;
    }

    (void)furi_hal_power_light_sleep_hold_pin_high(furi_hal_nrf24.csn.pin, false);
    furi_hal_nrf24_ext_pads_idle();
    /* Board module back to the idle the NRF24 app leaves it in. */
    furi_hal_gpio_init_simple(&furi_hal_nrf24_board_ce, GpioModeAnalog);
    furi_hal_nrf24.external = false;
    ESP_LOGI(TAG, "External NRF24 released");
}
#endif

FuriHalNrf24ExtResult furi_hal_nrf24_session_begin(void) {
    FuriMutex* mutex = furi_hal_nrf24_lock();
    FuriHalNrf24ExtResult result = FuriHalNrf24ExtOk;
    if(furi_hal_nrf24.refcount > 0) {
        /* Already open (possibly by an outer scene): just take another reference. */
        furi_hal_nrf24.refcount++;
    } else {
        furi_hal_nrf24_config_load_locked();
        const FuriHalNrf24ExtConfig config = furi_hal_nrf24.config;
        furi_hal_nrf24.external = false;
        if(config.use_external) {
#if FURI_HAL_NRF24_HAS_EXT
            result = furi_hal_nrf24_begin_external_locked(&config);
#else
            result = FuriHalNrf24ExtUnsupported;
#endif
        } else {
            /* Board module: CE becomes an output driven low (what the app always did). */
            furi_hal_gpio_init(
                &furi_hal_nrf24_board_ce, GpioModeOutputPushPull, GpioPullUp, GpioSpeedVeryHigh);
            furi_hal_gpio_write(&furi_hal_nrf24_board_ce, false);
        }
        if(result == FuriHalNrf24ExtOk) furi_hal_nrf24.refcount = 1;
    }
    if(result == FuriHalNrf24ExtOk) {
        /* CE/RX/TX may remain active while the scene releases the SPI host. */
        furi_hal_power_insomnia_enter();
    }
    furi_hal_nrf24_unlock(mutex);
    return result;
}

void furi_hal_nrf24_session_end(void) {
    FuriMutex* mutex = furi_hal_nrf24_lock();
    if(furi_hal_nrf24.refcount > 1) {
        /* An outer holder is still using the radio: drop our reference only. */
        furi_hal_nrf24.refcount--;
        furi_hal_power_insomnia_exit();
    } else if(furi_hal_nrf24.refcount == 1) {
#if FURI_HAL_NRF24_HAS_EXT
        if(furi_hal_nrf24.external) {
            furi_hal_nrf24_end_external_locked();
            if(furi_hal_nrf24.external) { /* could not be released yet: keep the session */
                furi_hal_nrf24_unlock(mutex);
                return;
            }
        } else
#endif
        {
            furi_hal_gpio_write(&furi_hal_nrf24_board_ce, false);
            if(!furi_hal_nrf24_power_down_board_module()) {
                ESP_LOGW(TAG, "Board NRF24 power-down SPI command failed after retries");
            }
            furi_hal_gpio_init_simple(&furi_hal_nrf24_board_ce, GpioModeAnalog);
        }
        furi_hal_nrf24.refcount = 0;
        furi_hal_power_insomnia_exit();
    }
    furi_hal_nrf24_unlock(mutex);
}
