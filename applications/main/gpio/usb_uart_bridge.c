#include "usb_uart_bridge.h"

#include <cli/cli_vcp.h>
#include <driver/gpio.h>
#include <driver/uart.h>
#include <esp_err.h>
#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_external_uart.h>
#include <furi_hal_shared_pins.h>
#include <furi_hal_uart_shared.h>
#include <furi_hal_usb_cdc.h>
#include <furi_hal_usb_tinyusb_composite.h>

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "boards/board.h"
#include "sdkconfig.h"

#define TAG "UsbUart"
#define USB_UART_CDC_IF 0
#define USB_UART_RX_BUF_SIZE 1024
#define USB_UART_TX_BUF_SIZE 1024
#define USB_UART_PACKET_SIZE CDC_DATA_SZ
#define USB_UART_ACQUIRE_TIMEOUT_MS 1500
#define USB_UART_TX_DRAIN_TIMEOUT_MS 250

#if CONFIG_IDF_TARGET_ESP32S3 && defined(BOARD_HAS_USB_UART_BRIDGE) && \
    BOARD_HAS_USB_UART_BRIDGE && defined(BOARD_USB_UART_NUM) && \
    defined(BOARD_USB_UART_TX) && defined(BOARD_USB_UART_RX) && \
    BOARD_USB_UART_NUM >= 0 && BOARD_USB_UART_TX >= 0 && BOARD_USB_UART_RX >= 0
#define USB_UART_BRIDGE_SUPPORTED 1
#else
#define USB_UART_BRIDGE_SUPPORTED 0
#endif

enum {
    UsbUartWorkerEvtStop = 1U << 0,
};

struct UsbUartBridge {
    FuriThread* worker;
    FuriMutex* mutex;
    CliVcp* cli_vcp;
    UsbUartConfig cfg;
    FuriHalUsbInterface* usb_previous;
    atomic_bool stop;
    atomic_bool active;
    atomic_bool error;
    atomic_uint tx_cnt;
    atomic_uint rx_cnt;
    atomic_uint host_baudrate;
    atomic_uint baudrate_cur;
    bool callbacks_registered;
    bool cli_record_open;
    bool cli_disabled;
    bool usb_lease_acquired;
    bool usb_composite_installed;
    bool shared_pins_acquired;
    bool shared_pins_saved;
    bool shared_uart_acquired;
    bool sleep_inhibit_entered;
    bool uart_driver_installed;
};

static const char s_usb_owner[] = "USB UART Bridge";
static const char s_shared_pins_owner[] = "USB UART Bridge";
static atomic_bool s_bridge_active;
static UsbUartBridge* s_pending_cleanup;

static int32_t usb_uart_worker(void* context);
static bool usb_uart_cleanup(UsbUartBridge* bridge);

static void usb_uart_set_error(UsbUartBridge* bridge, const char* operation, esp_err_t err) {
    FURI_LOG_E(TAG, "%s failed: %s (%d)", operation, esp_err_to_name(err), (int)err);
    atomic_store_explicit(&bridge->error, true, memory_order_release);
}

static void usb_uart_apply_baudrate(UsbUartBridge* bridge) {
    furi_check(furi_mutex_acquire(bridge->mutex, FuriWaitForever) == FuriStatusOk);
    const uint32_t configured_baudrate = bridge->cfg.baudrate;
    furi_check(furi_mutex_release(bridge->mutex) == FuriStatusOk);

    uint32_t baudrate = configured_baudrate;
    if(baudrate == 0) {
        baudrate = atomic_load_explicit(&bridge->host_baudrate, memory_order_acquire);
    }
    if(!baudrate ||
       baudrate == atomic_load_explicit(&bridge->baudrate_cur, memory_order_acquire)) {
        return;
    }

#if USB_UART_BRIDGE_SUPPORTED
    const esp_err_t err = uart_set_baudrate((uart_port_t)BOARD_USB_UART_NUM, baudrate);
    if(err != ESP_OK) {
        usb_uart_set_error(bridge, "Setting UART baud rate", err);
        return;
    }
    atomic_store_explicit(&bridge->baudrate_cur, baudrate, memory_order_release);
#else
    UNUSED(bridge);
#endif
}

static void usb_uart_cdc_line_config(void* context, struct usb_cdc_line_coding* config) {
    UsbUartBridge* bridge = context;
    if(config && config->dwDTERate) {
        atomic_store_explicit(
            &bridge->host_baudrate, config->dwDTERate, memory_order_release);
    }
}

static const CdcCallbacks usb_uart_cdc_callbacks = {
    .tx_ep_callback = NULL,
    .rx_ep_callback = NULL,
    .state_callback = NULL,
    .ctrl_line_callback = NULL,
    .config_callback = usb_uart_cdc_line_config,
};

static bool usb_uart_acquire_uart(UsbUartBridge* bridge) {
#if USB_UART_BRIDGE_SUPPORTED
    if(furi_hal_uart_shared_acquire(USB_UART_ACQUIRE_TIMEOUT_MS) !=
       FuriHalUartSharedAcquireAcquired) {
        FURI_LOG_E(TAG, "Shared UART is busy");
        return false;
    }
    bridge->shared_uart_acquired = true;

    if(!furi_hal_shared_pins_acquire(&s_shared_pins_owner)) {
        FURI_LOG_E(TAG, "Shared external UART pins are already leased");
        return false;
    }
    bridge->shared_pins_acquired = true;

    furi_hal_power_light_sleep_inhibit_enter();
    bridge->sleep_inhibit_entered = true;

#if defined(BOARD_EXTERNAL_UART_SAVE_RESTORE_SHARED_PINS) && \
    BOARD_EXTERNAL_UART_SAVE_RESTORE_SHARED_PINS
    if(!furi_hal_shared_pins_save(&s_shared_pins_owner)) {
        FURI_LOG_E(TAG, "Could not preserve the external UART pin state");
        return false;
    }
    bridge->shared_pins_saved = true;
#endif

    const uart_port_t uart_num = (uart_port_t)BOARD_USB_UART_NUM;
    if(uart_is_driver_installed(uart_num)) {
        FURI_LOG_E(TAG, "UART%d driver is already installed", (int)uart_num);
        return false;
    }

    esp_err_t err = uart_driver_install(
        uart_num, USB_UART_RX_BUF_SIZE, USB_UART_TX_BUF_SIZE, 0, NULL, 0);
    if(err != ESP_OK) {
        usb_uart_set_error(bridge, "Installing UART driver", err);
        return false;
    }
    bridge->uart_driver_installed = true;

    const uart_config_t config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    err = uart_param_config(uart_num, &config);
    if(err == ESP_OK) {
        err = uart_set_pin(
            uart_num,
            BOARD_USB_UART_TX,
            BOARD_USB_UART_RX,
            UART_PIN_NO_CHANGE,
            UART_PIN_NO_CHANGE);
    }
    if(err == ESP_OK) {
        err = gpio_set_pull_mode((gpio_num_t)BOARD_USB_UART_RX, GPIO_PULLUP_ONLY);
    }
    if(err == ESP_OK) err = uart_flush_input(uart_num);
    if(err != ESP_OK) {
        usb_uart_set_error(bridge, "Configuring external UART", err);
        return false;
    }

    return true;
#else
    UNUSED(bridge);
    return false;
#endif
}

static bool usb_uart_release_uart(UsbUartBridge* bridge) {
#if USB_UART_BRIDGE_SUPPORTED
    const uart_port_t uart_num = (uart_port_t)BOARD_USB_UART_NUM;
    if(bridge->uart_driver_installed) {
        esp_err_t err = uart_wait_tx_done(
            uart_num, pdMS_TO_TICKS(USB_UART_TX_DRAIN_TIMEOUT_MS));
        if(err != ESP_OK && uart_is_driver_installed(uart_num)) {
            usb_uart_set_error(bridge, "Waiting for UART TX to drain", err);
            return false;
        }
        if(err != ESP_OK) {
            FURI_LOG_W(TAG, "UART driver disappeared before TX drain completed");
        }

        err = uart_driver_delete(uart_num);
        if(err != ESP_OK && uart_is_driver_installed(uart_num)) {
            usb_uart_set_error(bridge, "Removing UART driver", err);
            return false;
        }
        if(err != ESP_OK) {
            FURI_LOG_W(TAG, "UART driver was already removed");
        }
        bridge->uart_driver_installed = false;
    }

    if(bridge->shared_pins_saved) {
        furi_hal_shared_pins_restore(&s_shared_pins_owner);
        if(furi_hal_shared_pins_is_saved(&s_shared_pins_owner)) {
            FURI_LOG_E(TAG, "Could not restore shared UART pin state; retaining leases");
            atomic_store_explicit(&bridge->error, true, memory_order_release);
            return false;
        }
        bridge->shared_pins_saved = false;
    }

    if(bridge->shared_uart_acquired) {
        if(!furi_hal_uart_shared_release()) {
            FURI_LOG_E(TAG, "Could not release the shared UART; retaining pin lease");
            atomic_store_explicit(&bridge->error, true, memory_order_release);
            return false;
        }
        bridge->shared_uart_acquired = false;
    }
    if(bridge->shared_pins_acquired) {
        furi_hal_shared_pins_release(&s_shared_pins_owner);
        bridge->shared_pins_acquired = false;
    }
    if(bridge->sleep_inhibit_entered) {
        furi_hal_power_light_sleep_inhibit_exit();
        bridge->sleep_inhibit_entered = false;
    }
    return true;
#else
    UNUSED(bridge);
    return true;
#endif
}

static bool usb_uart_cleanup(UsbUartBridge* bridge) {
    if(bridge->callbacks_registered) {
        furi_hal_cdc_set_callbacks(USB_UART_CDC_IF, NULL, NULL);
        bridge->callbacks_registered = false;
    }

    if(bridge->worker) {
        atomic_store_explicit(&bridge->stop, true, memory_order_release);
        furi_thread_flags_set(
            furi_thread_get_id(bridge->worker), UsbUartWorkerEvtStop);
        furi_thread_join(bridge->worker);
        furi_thread_free(bridge->worker);
        bridge->worker = NULL;
    }

    bool cleanup_ok = usb_uart_release_uart(bridge);

    if(bridge->usb_lease_acquired && furi_hal_usb_composite_is_installed()) {
        bridge->usb_composite_installed = true;
    }
    if(bridge->usb_composite_installed) {
        if(!furi_hal_usb_composite_uninstall_for_owner(s_usb_owner)) {
            FURI_LOG_E(TAG, "Could not uninstall USB composite; retaining USB lease");
            cleanup_ok = false;
        } else {
            bridge->usb_composite_installed = false;
        }
    }

    if(bridge->usb_lease_acquired && !bridge->usb_composite_installed) {
        if(!furi_hal_usb_lease_release(s_usb_owner, bridge->usb_previous)) {
            if(furi_hal_usb_is_lease_owner(s_usb_owner)) {
                FURI_LOG_E(TAG, "Could not release USB lease; USB remains reserved");
                cleanup_ok = false;
            } else {
                bridge->usb_lease_acquired = false;
                cleanup_ok = false;
            }
        } else {
            bridge->usb_lease_acquired = false;
        }
    }

    if(bridge->usb_lease_acquired) cleanup_ok = false;

    if(!bridge->usb_lease_acquired) {
        if(bridge->cli_disabled && !furi_hal_usb_composite_is_installed()) {
            cli_vcp_enable(bridge->cli_vcp);
        }
        bridge->cli_disabled = false;
        if(bridge->cli_record_open) {
            furi_record_close(RECORD_CLI_VCP);
            bridge->cli_record_open = false;
            bridge->cli_vcp = NULL;
        }
    }

    atomic_store_explicit(&bridge->active, false, memory_order_release);
    return cleanup_ok && !bridge->uart_driver_installed &&
           !bridge->shared_uart_acquired && !bridge->shared_pins_acquired &&
           !bridge->usb_lease_acquired;
}

static int32_t usb_uart_worker(void* context) {
    UsbUartBridge* bridge = context;
    atomic_store_explicit(&bridge->active, true, memory_order_release);
    uint8_t data[USB_UART_PACKET_SIZE];

    while(!atomic_load_explicit(&bridge->stop, memory_order_acquire)) {
        usb_uart_apply_baudrate(bridge);
        if(atomic_load_explicit(&bridge->error, memory_order_acquire)) break;

#if USB_UART_BRIDGE_SUPPORTED
        int received = uart_read_bytes(
            (uart_port_t)BOARD_USB_UART_NUM, data, sizeof(data), pdMS_TO_TICKS(10));
        if(received < 0) {
            usb_uart_set_error(bridge, "Reading external UART", ESP_FAIL);
            break;
        }
        if(received > 0) {
            furi_hal_cdc_send(USB_UART_CDC_IF, data, (uint16_t)received);
            atomic_fetch_add_explicit(&bridge->rx_cnt, (uint32_t)received, memory_order_relaxed);
        }

        for(size_t packet = 0; packet < 8; ++packet) {
            int32_t length =
                furi_hal_cdc_receive(USB_UART_CDC_IF, data, sizeof(data));
            if(length <= 0) break;
            const int written =
                uart_write_bytes(
                    (uart_port_t)BOARD_USB_UART_NUM, (const char*)data, (size_t)length);
            if(written != length) {
                usb_uart_set_error(bridge, "Writing external UART", ESP_FAIL);
                break;
            }
            atomic_fetch_add_explicit(&bridge->tx_cnt, (uint32_t)written, memory_order_relaxed);
        }
#else
        break;
#endif
    }

    atomic_store_explicit(&bridge->active, false, memory_order_release);
    return 0;
}

UsbUartBridge* usb_uart_enable(UsbUartConfig* cfg) {
#if USB_UART_BRIDGE_SUPPORTED
    if(!cfg || !furi_hal_usb_uart_bridge_available()) {
        FURI_LOG_E(TAG, "USB UART bridge is not enabled for this board profile");
        return NULL;
    }

    if(s_pending_cleanup) {
        if(!usb_uart_cleanup(s_pending_cleanup)) {
            FURI_LOG_E(TAG, "Previous bridge cleanup is still pending");
            return NULL;
        }
        furi_mutex_free(s_pending_cleanup->mutex);
        free(s_pending_cleanup);
        s_pending_cleanup = NULL;
        atomic_store_explicit(&s_bridge_active, false, memory_order_release);
    }

    bool expected = false;
    if(!atomic_compare_exchange_strong_explicit(
           &s_bridge_active, &expected, true, memory_order_acq_rel, memory_order_acquire)) {
        FURI_LOG_E(TAG, "USB UART bridge is already active");
        return NULL;
    }

    UsbUartBridge* bridge = calloc(1, sizeof(UsbUartBridge));
    if(!bridge) {
        atomic_store_explicit(&s_bridge_active, false, memory_order_release);
        FURI_LOG_E(TAG, "Could not allocate bridge state");
        return NULL;
    }
    bridge->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    if(!bridge->mutex) {
        free(bridge);
        atomic_store_explicit(&s_bridge_active, false, memory_order_release);
        FURI_LOG_E(TAG, "Could not allocate bridge mutex");
        return NULL;
    }
    atomic_init(&bridge->stop, false);
    atomic_init(&bridge->active, false);
    atomic_init(&bridge->error, false);
    atomic_init(&bridge->tx_cnt, 0);
    atomic_init(&bridge->rx_cnt, 0);
    atomic_init(&bridge->host_baudrate, 115200);
    atomic_init(&bridge->baudrate_cur, 0);

    bridge->cfg = *cfg;
    if(bridge->cfg.vcp_ch || bridge->cfg.uart_ch || bridge->cfg.flow_pins ||
       bridge->cfg.software_de_re) {
        FURI_LOG_E(TAG, "Requested USB UART pins/channels are unsupported on this board");
        goto failed;
    }
    bridge->cfg.vcp_ch = 0;
    bridge->cfg.uart_ch = 0;
    bridge->cfg.flow_pins = 0;
    bridge->cfg.software_de_re = 0;

    if(!furi_hal_usb_lease_acquire(s_usb_owner, &bridge->usb_previous)) {
        FURI_LOG_E(TAG, "USB is busy or its current owner cannot release it");
        goto failed;
    }
    bridge->usb_lease_acquired = true;

    bridge->cli_vcp = furi_record_open(RECORD_CLI_VCP);
    if(!bridge->cli_vcp) {
        FURI_LOG_E(TAG, "CLI VCP service is unavailable");
        goto failed;
    }
    bridge->cli_record_open = true;
    bridge->cli_disabled = cli_vcp_disable(bridge->cli_vcp);

    if(!furi_hal_usb_set_config_for_owner(
           s_usb_owner, &usb_cdc_single, NULL)) {
        FURI_LOG_E(TAG, "Could not assign USB CDC to the bridge");
        goto failed;
    }
    if(!furi_hal_usb_composite_install_for_owner(s_usb_owner, 0, 0, NULL, NULL)) {
        FURI_LOG_E(TAG, "Could not install USB CDC composite");
        bridge->usb_composite_installed = furi_hal_usb_composite_is_installed();
        goto failed;
    }
    bridge->usb_composite_installed = true;
    furi_hal_usb_reinit_for_owner(s_usb_owner);

    if(!usb_uart_acquire_uart(bridge)) goto failed;

    furi_hal_cdc_set_callbacks(
        USB_UART_CDC_IF, (CdcCallbacks*)&usb_uart_cdc_callbacks, bridge);
    bridge->callbacks_registered = true;

    bridge->worker =
        furi_thread_alloc_ex("UsbUartWorker", 3072, usb_uart_worker, bridge);
    if(!bridge->worker) {
        FURI_LOG_E(TAG, "Could not allocate bridge worker");
        goto failed;
    }
    furi_thread_start(bridge->worker);
    return bridge;

failed:
    if(!usb_uart_cleanup(bridge)) {
        s_pending_cleanup = bridge;
    } else {
        furi_mutex_free(bridge->mutex);
        free(bridge);
    }
    atomic_store_explicit(&s_bridge_active, false, memory_order_release);
    return NULL;
#else
    UNUSED(cfg);
    FURI_LOG_E(TAG, "USB UART bridge requires an ESP32-S3 board with an external UART mapping");
    return NULL;
#endif
}

bool usb_uart_disable(UsbUartBridge* bridge) {
    furi_assert(bridge);
    if(!usb_uart_cleanup(bridge)) {
        s_pending_cleanup = bridge;
        FURI_LOG_E(TAG, "Bridge cleanup incomplete; affected resources remain reserved");
        return false;
    }
    if(s_pending_cleanup == bridge) s_pending_cleanup = NULL;
    furi_mutex_free(bridge->mutex);
    free(bridge);
    atomic_store_explicit(&s_bridge_active, false, memory_order_release);
    return true;
}

void usb_uart_set_config(UsbUartBridge* bridge, UsbUartConfig* cfg) {
    furi_assert(bridge);
    furi_assert(cfg);
    if(cfg->vcp_ch || cfg->uart_ch || cfg->flow_pins || cfg->software_de_re) {
        FURI_LOG_E(TAG, "Ignoring unsupported USB UART channel or control-pin selection");
        return;
    }
    furi_check(furi_mutex_acquire(bridge->mutex, FuriWaitForever) == FuriStatusOk);
    bridge->cfg = *cfg;
    furi_check(furi_mutex_release(bridge->mutex) == FuriStatusOk);
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
    state->tx_cnt = atomic_load_explicit(&bridge->tx_cnt, memory_order_acquire);
    state->rx_cnt = atomic_load_explicit(&bridge->rx_cnt, memory_order_acquire);
    state->baudrate_cur = atomic_load_explicit(&bridge->baudrate_cur, memory_order_acquire);
    state->active = atomic_load_explicit(&bridge->active, memory_order_acquire);
    state->error = atomic_load_explicit(&bridge->error, memory_order_acquire);
}
