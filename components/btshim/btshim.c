/**
 * @file btshim.c
 * BT service — ESP32 implementation
 *
 * Based on STM32 bt.c + bt_api.c. Uses the same message-queue architecture
 * and GapEvent callback system. RPC and Power integration are excluded
 * until those services are ported.
 */

#include "bt_i.h"

#include <furi_hal_bt.h>
#include <gui/elements.h>
#include <assets_icons.h>
#include <notification/notification_messages.h>
#include <ble_profile/extra_profiles/hid_profile.h>
#include <ble_profile/extra_profiles/serial_profile.h>
#include <rpc/rpc.h>
#include <esp_log.h>
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_gap_ble_api.h>
#include <ble_serial.h>

#define BT_RPC_EVENT_BUFF_SENT    (1UL << 0)
#define BT_RPC_EVENT_DISCONNECTED (1UL << 1)
#define BT_RPC_EVENT_ALL          (BT_RPC_EVENT_BUFF_SENT | BT_RPC_EVENT_DISCONNECTED)

/* RX feeder thread event flags */
#define BT_RX_EVENT_DATA      (1UL << 0)
#define BT_RX_EVENT_STOP      (1UL << 1)
#define BT_RX_EVENT_ALL       (BT_RX_EVENT_DATA | BT_RX_EVENT_STOP)

#define BT_RX_STREAM_SIZE     (8192)

/* Use ESP_LOG for BLE/RPC debug since FURI_LOG may not work in BTC task context */
#define BT_LOG_I(fmt, ...) ESP_LOGI("BtSrv", fmt, ##__VA_ARGS__)
#define BT_LOG_E(fmt, ...) ESP_LOGE("BtSrv", fmt, ##__VA_ARGS__)
#define BT_LOG_W(fmt, ...) ESP_LOGW("BtSrv", fmt, ##__VA_ARGS__)
#define BT_LOG_D(fmt, ...) ESP_LOGD("BtSrv", fmt, ##__VA_ARGS__)

#define TAG "BtSrv"

#define ICON_SPACER          2
#define BT_DEFAULT_MTU       BLE_PROFILE_SERIAL_PACKET_SIZE_MAX

/* ---- Statusbar ---- */

static void bt_draw_statusbar_callback(Canvas* canvas, void* context) {
    furi_assert(context);

    Bt* bt = context;
    uint8_t draw_offset = 0;
    if(bt->beacon_active) {
        canvas_draw_icon(canvas, 0, 0, &I_BLE_beacon_7x8);
        draw_offset += icon_get_width(&I_BLE_beacon_7x8) + ICON_SPACER;
    }
    if(bt->status == BtStatusAdvertising) {
        canvas_draw_icon(canvas, draw_offset, 0, &I_Bluetooth_Idle_5x8);
    } else if(bt->status == BtStatusConnected) {
        canvas_draw_icon(canvas, draw_offset, 0, &I_Bluetooth_Connected_16x8);
    }
}

static ViewPort* bt_statusbar_view_port_alloc(Bt* bt) {
    ViewPort* statusbar_view_port = view_port_alloc();
    view_port_set_width(statusbar_view_port, 5);
    view_port_draw_callback_set(statusbar_view_port, bt_draw_statusbar_callback, bt);
    view_port_enabled_set(statusbar_view_port, false);
    return statusbar_view_port;
}

/* ---- PIN code display ---- */

static void bt_pin_code_view_port_draw_callback(Canvas* canvas, void* context) {
    furi_assert(context);
    Bt* bt = context;
    char pin_code_info[24];
    canvas_draw_icon(canvas, 0, 0, &I_BLE_Pairing_128x64);
    snprintf(pin_code_info, sizeof(pin_code_info), "Pairing code\n%06lu", bt->pin_code);
    elements_multiline_text_aligned(canvas, 64, 4, AlignCenter, AlignTop, pin_code_info);
    elements_button_left(canvas, "Quit");
}

static void bt_pin_code_view_port_input_callback(InputEvent* event, void* context) {
    furi_assert(context);
    Bt* bt = context;
    if(event->type == InputTypeShort) {
        if(event->key == InputKeyLeft || event->key == InputKeyBack) {
            view_port_enabled_set(bt->pin_code_view_port, false);
        }
    }
}

static ViewPort* bt_pin_code_view_port_alloc(Bt* bt) {
    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, bt_pin_code_view_port_draw_callback, bt);
    view_port_input_callback_set(view_port, bt_pin_code_view_port_input_callback, bt);
    view_port_enabled_set(view_port, false);
    return view_port;
}

static void bt_pin_code_show(Bt* bt, uint32_t pin_code) {
    bt->pin_code = pin_code;
    if(!bt->pin_code_view_port) {
        bt->pin_code_view_port = bt_pin_code_view_port_alloc(bt);
        gui_add_view_port(bt->gui, bt->pin_code_view_port, GuiLayerFullscreen);
    }
    notification_message(bt->notification, &sequence_display_backlight_on);
    if(bt->suppress_pin_screen) return;

    gui_view_port_send_to_front(bt->gui, bt->pin_code_view_port);
    view_port_enabled_set(bt->pin_code_view_port, true);
}

static void bt_pin_code_hide(Bt* bt) {
    bt->pin_code = 0;
    if(bt->pin_code_view_port && view_port_is_enabled(bt->pin_code_view_port)) {
        view_port_enabled_set(bt->pin_code_view_port, false);
    }
}

/* ---- Storage callback for key reload ---- */

static void bt_storage_callback(const void* message, void* context) {
    furi_assert(context);
    Bt* bt = context;
    const StorageEvent* event = message;

    if(event->type == StorageEventTypeCardMount) {
        const BtMessage msg = {
            .type = BtMessageTypeReloadKeysSettings,
        };

        furi_check(
            furi_message_queue_put(bt->message_queue, &msg, FuriWaitForever) == FuriStatusOk);
    }
}

/* ---- Allocation ---- */

static Bt* bt_alloc(void) {
    Bt* bt = malloc(sizeof(Bt));

    bt->max_packet_size = BT_DEFAULT_MTU;
    bt->current_profile = NULL;

    /* Keys storage (stub on ESP32) */
    bt->keys_storage = bt_keys_storage_alloc(BT_KEYS_STORAGE_PATH);

    /* Message queue */
    bt->message_queue = furi_message_queue_alloc(8, sizeof(BtMessage));

    /* Statusbar view port */
    bt->statusbar_view_port = bt_statusbar_view_port_alloc(bt);

    /* Notification */
    bt->notification = furi_record_open(RECORD_NOTIFICATION);

    /* GUI */
    bt->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(bt->gui, bt->statusbar_view_port, GuiLayerStatusBarLeft);

    /* RPC — initialize here (STM32 does this in a separate rpc_srv thread) */
    rpc_init();
    bt->rpc = furi_record_open(RECORD_RPC);
    bt->rpc_event = furi_event_flag_alloc();

    /* Intermediate RX buffer between GATTS callback and RPC */
    bt->rx_stream = furi_stream_buffer_alloc(BT_RX_STREAM_SIZE, 1);

    bt->pin = 0;

    return bt;
}

/* ---- RX feeder thread: drains intermediate buffer into RPC ---- */

static int32_t bt_rx_feeder_thread(void* context) {
    Bt* bt = context;
    uint8_t buf[512];

    BT_LOG_I("RX feeder thread started");

    while(true) {
        uint32_t flags = furi_thread_flags_wait(BT_RX_EVENT_ALL, FuriFlagWaitAny, FuriWaitForever);

        if(flags & BT_RX_EVENT_STOP) {
            break;
        }

        if(flags & BT_RX_EVENT_DATA) {
            /* Drain all available data from intermediate buffer into RPC */
            while(!furi_stream_buffer_is_empty(bt->rx_stream)) {
                size_t available = furi_stream_buffer_bytes_available(bt->rx_stream);
                size_t to_read = (available > sizeof(buf)) ? sizeof(buf) : available;
                size_t got = furi_stream_buffer_receive(bt->rx_stream, buf, to_read, 0);
                if(got == 0) break;

                /* Feed to RPC — this CAN block, which is fine on this thread */
                size_t fed = 0;
                while(fed < got) {
                    size_t n = rpc_session_feed(
                        bt->rpc_session, buf + fed, got - fed, 5000);
                    if(n == 0) {
                        BT_LOG_E("RPC feed stalled, dropping %zu bytes", got - fed);
                        break;
                    }
                    fed += n;
                }
            }
        }
    }

    BT_LOG_I("RX feeder thread stopped");
    return 0;
}

/* ---- Serial service event callback (called from BLE GATTS task) ---- */

static uint16_t bt_serial_event_callback(SerialServiceEvent event, void* context) {
    furi_assert(context);
    Bt* bt = context;
    uint16_t ret = 0;

    if(event.event == SerialServiceEventTypeDataReceived) {
        /* Non-blocking write to intermediate buffer — MUST NOT block GATTS task */
        size_t sent = furi_stream_buffer_send(
            bt->rx_stream, event.data.buffer, event.data.size, 0);
        if(sent != event.data.size) {
            BT_LOG_E("RX overflow: %zu/%u bytes buffered", sent, event.data.size);
        }
        /* Wake the feeder thread */
        furi_thread_flags_set(furi_thread_get_id(bt->rx_thread), BT_RX_EVENT_DATA);
        ret = furi_stream_buffer_spaces_available(bt->rx_stream);
    } else if(event.event == SerialServiceEventTypeDataSent) {
        BT_LOG_I("TX confirmed");
        furi_event_flag_set(bt->rpc_event, BT_RPC_EVENT_BUFF_SENT);
    } else if(event.event == SerialServiceEventTypesBleResetRequest) {
        FURI_LOG_I(TAG, "BLE restart request received");
    }
    return ret;
}

static void bt_rpc_send_bytes_callback(void* context, uint8_t* bytes, size_t bytes_len) {
    furi_assert(context);
    Bt* bt = context;

    BT_LOG_I("TX send %zu bytes, max_pkt=%u", bytes_len, bt->max_packet_size);

    if(furi_event_flag_get(bt->rpc_event) & BT_RPC_EVENT_DISCONNECTED) {
        BT_LOG_W("TX aborted: disconnected");
        return;
    }
    furi_event_flag_clear(bt->rpc_event, BT_RPC_EVENT_ALL & (~BT_RPC_EVENT_DISCONNECTED));
    size_t bytes_sent = 0;
    while(bytes_sent < bytes_len) {
        size_t bytes_remain = bytes_len - bytes_sent;
        size_t chunk = (bytes_remain > bt->max_packet_size) ? bt->max_packet_size : bytes_remain;
        bool ok = ble_profile_serial_tx(bt->current_profile, &bytes[bytes_sent], chunk);
        if(!ok) {
            BT_LOG_E("TX chunk failed at offset %zu/%zu", bytes_sent, bytes_len);
            break;
        }
        bytes_sent += chunk;

        uint32_t event_flag = furi_event_flag_wait(
            bt->rpc_event, BT_RPC_EVENT_ALL, FuriFlagWaitAny | FuriFlagNoClear, 3000);
        if(event_flag & BT_RPC_EVENT_DISCONNECTED) {
            BT_LOG_W("TX aborted: disconnected during send");
            break;
        } else if(event_flag == (uint32_t)FuriStatusErrorTimeout) {
            BT_LOG_E("TX timeout waiting for confirmation");
            break;
        } else {
            furi_event_flag_clear(bt->rpc_event, BT_RPC_EVENT_ALL & (~BT_RPC_EVENT_DISCONNECTED));
        }
    }
    BT_LOG_I("TX done, sent %zu/%zu", bytes_sent, bytes_len);
}

static void bt_serial_buffer_is_empty_callback(void* context) {
    furi_assert(context);
    Bt* bt = context;
    if(furi_hal_bt_check_profile_type(bt->current_profile, ble_profile_serial)) {
        ble_profile_serial_notify_buffer_is_empty(bt->current_profile);
    }
}

static void bt_open_rpc_connection(Bt* bt) {
    BT_LOG_I("bt_open_rpc_connection: session=%p status=%d profile=%p",
        (void*)bt->rpc_session, bt->status, (void*)bt->current_profile);
    if(!bt->rpc_session && bt->status == BtStatusConnected) {
        /* Open RPC record lazily on first connection */
        if(!bt->rpc) {
            BT_LOG_I("Opening RECORD_RPC...");
            bt->rpc = furi_record_open(RECORD_RPC);
            BT_LOG_I("RECORD_RPC opened: %p", (void*)bt->rpc);
        }
        furi_event_flag_clear(bt->rpc_event, BT_RPC_EVENT_DISCONNECTED);
        bool is_serial = furi_hal_bt_check_profile_type(bt->current_profile, ble_profile_serial);
        BT_LOG_I("Profile is_serial=%d", is_serial);
        if(is_serial) {
            bt->rpc_session = rpc_session_open(bt->rpc, RpcOwnerBle);
            if(bt->rpc_session) {
                BT_LOG_I("RPC session opened OK");
                rpc_session_set_send_bytes_callback(bt->rpc_session, bt_rpc_send_bytes_callback);
                rpc_session_set_buffer_is_empty_callback(
                    bt->rpc_session, bt_serial_buffer_is_empty_callback);
                rpc_session_set_context(bt->rpc_session, bt);
                /* Start RX feeder thread */
                furi_stream_buffer_reset(bt->rx_stream);
                bt->rx_thread = furi_thread_alloc_ex(
                    "BtRxFeeder", 4096, bt_rx_feeder_thread, bt);
                furi_thread_start(bt->rx_thread);
                ble_profile_serial_set_event_callback(
                    bt->current_profile, BT_RX_STREAM_SIZE, bt_serial_event_callback, bt);
                ble_profile_serial_set_rpc_active(
                    bt->current_profile, FuriHalBtSerialRpcStatusActive);
                BT_LOG_I("RPC over BLE fully initialized");
            } else {
                BT_LOG_W("RPC is busy, failed to open new session");
            }
        }
    }
}

static void bt_close_rpc_connection(Bt* bt) {
    if(furi_hal_bt_check_profile_type(bt->current_profile, ble_profile_serial) &&
       bt->rpc_session) {
        FURI_LOG_I(TAG, "Close RPC connection");
        ble_profile_serial_set_rpc_active(
            bt->current_profile, FuriHalBtSerialRpcStatusNotActive);
        furi_event_flag_set(bt->rpc_event, BT_RPC_EVENT_DISCONNECTED);
        /* Stop RX feeder thread */
        if(bt->rx_thread) {
            furi_thread_flags_set(furi_thread_get_id(bt->rx_thread), BT_RX_EVENT_STOP);
            furi_thread_join(bt->rx_thread);
            furi_thread_free(bt->rx_thread);
            bt->rx_thread = NULL;
        }
        rpc_session_close(bt->rpc_session);
        ble_profile_serial_set_event_callback(bt->current_profile, 0, NULL, NULL);
        bt->rpc_session = NULL;
    }
}

/* ---- GAP event callback (called from furi_hal_bt bridge) ---- */

static bool bt_on_gap_event_callback(GapEvent event, void* context) {
    furi_assert(context);
    Bt* bt = context;
    bool ret = false;
    bt->pin = 0;
    bool do_update_status = false;

    if(event.type == GapEventTypeConnected) {
        bt->status = BtStatusConnected;
        do_update_status = true;
        bt_open_rpc_connection(bt);
        ret = true;
    } else if(event.type == GapEventTypeDisconnected) {
        bt_close_rpc_connection(bt);
        bt->status = BtStatusOff;
        do_update_status = true;
        ret = true;
    } else if(event.type == GapEventTypeStartAdvertising) {
        bt->status = BtStatusAdvertising;
        do_update_status = true;
        ret = true;
    } else if(event.type == GapEventTypeStopAdvertising) {
        bt->status = BtStatusOff;
        do_update_status = true;
        ret = true;
    } else if(event.type == GapEventTypePinCodeShow) {
        bt->pin = event.data.pin_code;
        BtMessage message = {
            .type = BtMessageTypePinCodeShow, .data.pin_code = event.data.pin_code};
        furi_check(
            furi_message_queue_put(bt->message_queue, &message, FuriWaitForever) == FuriStatusOk);
        ret = true;
    } else if(event.type == GapEventTypeUpdateMTU) {
        bt->max_packet_size = event.data.max_packet_size;
        ret = true;
    } else if(event.type == GapEventTypeBeaconStart) {
        bt->beacon_active = true;
        do_update_status = true;
        ret = true;
    } else if(event.type == GapEventTypeBeaconStop) {
        bt->beacon_active = false;
        do_update_status = true;
        ret = true;
    }

    if(do_update_status) {
        BtMessage message = {.type = BtMessageTypeUpdateStatus};
        furi_check(
            furi_message_queue_put(bt->message_queue, &message, FuriWaitForever) == FuriStatusOk);
    }
    return ret;
}

static void bt_on_key_storage_change_callback(uint8_t* addr, uint16_t size, void* context) {
    furi_assert(context);
    Bt* bt = context;
    BtMessage message = {
        .type = BtMessageTypeKeysStorageUpdated,
        .data.key_storage_data.start_address = addr,
        .data.key_storage_data.size = size};
    furi_check(
        furi_message_queue_put(bt->message_queue, &message, FuriWaitForever) == FuriStatusOk);
}

/* ---- Statusbar update ---- */

static void bt_statusbar_update(Bt* bt) {
    uint8_t active_icon_width = 0;
    if(bt->beacon_active) {
        active_icon_width = icon_get_width(&I_BLE_beacon_7x8) + ICON_SPACER;
    }
    if(bt->status == BtStatusAdvertising) {
        active_icon_width += icon_get_width(&I_Bluetooth_Idle_5x8);
    } else if(bt->status == BtStatusConnected) {
        active_icon_width += icon_get_width(&I_Bluetooth_Connected_16x8);
    }

    if(active_icon_width > 0) {
        view_port_set_width(bt->statusbar_view_port, active_icon_width);
        view_port_enabled_set(bt->statusbar_view_port, true);
    } else {
        view_port_enabled_set(bt->statusbar_view_port, false);
    }
}

/* ---- Profile management ---- */

static void bt_change_profile(Bt* bt, BtMessage* message) {
    if(furi_hal_bt_is_gatt_gap_supported()) {
        bt_settings_load(&bt->bt_settings);

        bt_keys_storage_load(bt->keys_storage);

        bt->current_profile = furi_hal_bt_change_app(
            message->data.profile.template,
            message->data.profile.params,
            bt_keys_storage_get_root_keys(bt->keys_storage),
            bt_on_gap_event_callback,
            bt);
        if(bt->current_profile) {
            FURI_LOG_I(TAG, "Bt App started");
            if(bt->bt_settings.enabled) {
                furi_hal_bt_start_advertising();
            }
            furi_hal_bt_set_key_storage_change_callback(bt_on_key_storage_change_callback, bt);
        } else {
            FURI_LOG_E(TAG, "Failed to start Bt App");
        }
        if(message->profile_instance) {
            *message->profile_instance = bt->current_profile;
        }
        if(message->result) {
            *message->result = bt->current_profile != NULL;
        }

    } else {
        FURI_LOG_E(TAG, "Radio stack doesn't support this app");
        if(message->result) {
            *message->result = false;
        }
        if(message->profile_instance) {
            *message->profile_instance = NULL;
        }
    }
}

static void bt_close_connection(Bt* bt) {
    bt_close_rpc_connection(bt);
    furi_hal_bt_stop_advertising();
}

static void bt_apply_settings(Bt* bt) {
    FURI_LOG_I(TAG, "bt_apply_settings: enabled=%d", bt->bt_settings.enabled);
    if(bt->bt_settings.enabled) {
        furi_hal_bt_start_advertising();
    } else {
        furi_hal_bt_stop_advertising();
    }
}

static void bt_load_keys(Bt* bt) {
    if(!furi_hal_bt_is_gatt_gap_supported()) {
        FURI_LOG_W(TAG, "Unsupported radio stack");
        bt->status = BtStatusUnavailable;
        return;

    } else if(bt_keys_storage_is_changed(bt->keys_storage)) {
        FURI_LOG_I(TAG, "Loading new keys");
        bt_keys_storage_load(bt->keys_storage);
        bt->current_profile = NULL;
    } else {
        FURI_LOG_I(TAG, "Keys unchanged");
    }
}

static void bt_start_application(Bt* bt) {
    FURI_LOG_I(TAG, "bt_start_application: current_profile=%p", (void*)bt->current_profile);
    if(!bt->current_profile) {
        FURI_LOG_I(TAG, "Starting ble_profile_serial...");
        /* Use serial profile as default (same as STM32) */
        bt->current_profile = furi_hal_bt_change_app(
            ble_profile_serial,
            NULL,
            bt_keys_storage_get_root_keys(bt->keys_storage),
            bt_on_gap_event_callback,
            bt);

        if(!bt->current_profile) {
            FURI_LOG_E(TAG, "BLE App start failed");
            bt->status = BtStatusUnavailable;
        } else {
            FURI_LOG_I(TAG, "BLE App started OK, profile=%p", (void*)bt->current_profile);
        }
    }
}

static void bt_load_settings(Bt* bt) {
    bt_settings_load(&bt->bt_settings);
    bt_apply_settings(bt);
}

static void bt_handle_get_settings(Bt* bt, BtMessage* message) {
    *message->data.settings = bt->bt_settings;
}

static void bt_handle_set_settings(Bt* bt, BtMessage* message) {
    bt->bt_settings = *message->data.csettings;
    bt_apply_settings(bt);
    bt_settings_save(&bt->bt_settings);
}

static void bt_handle_reload_keys_settings(Bt* bt) {
    bt_load_keys(bt);
    bt_start_application(bt);
    bt_load_settings(bt);
}

static void bt_handle_stop_stack(Bt* bt) {
    FURI_LOG_I(TAG, "Stopping BLE stack...");

    if(bt->status == BtStatusOff) {
        FURI_LOG_I(TAG, "BLE stack already off");
        return;
    }

    bt_close_rpc_connection(bt);
    furi_hal_bt_stop_advertising();

    if(bt->current_profile) {
        bt->current_profile->config->stop(bt->current_profile);
        bt->current_profile = NULL;
    }

    // Only deinit if stack was actually initialized
    if(esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_UNINITIALIZED) {
        esp_bluedroid_disable();
        furi_delay_ms(50);
        esp_bluedroid_deinit();
        furi_delay_ms(50);
    }

    if(esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_IDLE) {
        esp_bt_controller_disable();
        furi_delay_ms(50);
    }
    if(esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
        esp_bt_controller_deinit();
        furi_delay_ms(50);
    }

    ble_serial_reset_initialized();
    furi_hal_bt_reinit(); // clear stale profile pointer in HAL

    bt->status = BtStatusOff;
    FURI_LOG_I(TAG, "BLE stack stopped");
}

static void bt_handle_start_stack(Bt* bt) {
    FURI_LOG_I(TAG, "Starting BLE stack...");

    if(furi_hal_bt_start_radio_stack()) {
        bt_start_application(bt);
        if(bt->bt_settings.enabled) {
            furi_hal_bt_start_advertising();
        }
    } else {
        FURI_LOG_E(TAG, "Radio stack start failed");
        bt->status = BtStatusUnavailable;
    }

    FURI_LOG_I(TAG, "BLE stack started");
}

static void bt_init_keys_settings(Bt* bt) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    furi_pubsub_subscribe(storage_get_pubsub(storage), bt_storage_callback, bt);

    if(storage_sd_status(storage) != FSE_OK) {
        FURI_LOG_D(TAG, "SD Card not ready, skipping settings");
        /* Start the BLE application without loading keys or settings */
        bt_start_application(bt);
        /* Still load settings (from internal storage) */
        bt_load_settings(bt);
        return;
    }

    bt_handle_reload_keys_settings(bt);
}

/* ---- Public API (bt_api.c equivalent) ---- */

FuriHalBleProfileBase* bt_profile_start(
    Bt* bt,
    const FuriHalBleProfileTemplate* profile_template,
    FuriHalBleProfileParams params) {
    furi_check(bt);

    FuriHalBleProfileBase* profile_instance = NULL;

    BtMessage message = {
        .lock = api_lock_alloc_locked(),
        .type = BtMessageTypeSetProfile,
        .profile_instance = &profile_instance,
        .data.profile.params = params,
        .data.profile.template = profile_template,
    };
    furi_check(
        furi_message_queue_put(bt->message_queue, &message, FuriWaitForever) == FuriStatusOk);
    api_lock_wait_unlock_and_free(message.lock);

    bt->current_profile = profile_instance;
    return profile_instance;
}

bool bt_profile_restore_default(Bt* bt) {
    /* Restore the serial profile (same as STM32) */
    bt->current_profile = bt_profile_start(bt, ble_profile_serial, NULL);
    return bt->current_profile != NULL;
}

void bt_disconnect(Bt* bt) {
    furi_check(bt);

    BtMessage message = {.lock = api_lock_alloc_locked(), .type = BtMessageTypeDisconnect};
    furi_check(
        furi_message_queue_put(bt->message_queue, &message, FuriWaitForever) == FuriStatusOk);
    api_lock_wait_unlock_and_free(message.lock);
}

void bt_set_status_changed_callback(Bt* bt, BtStatusChangedCallback callback, void* context) {
    furi_check(bt);

    bt->status_changed_cb = callback;
    bt->status_changed_ctx = context;
}

void bt_forget_bonded_devices(Bt* bt) {
    furi_check(bt);
    BtMessage message = {.type = BtMessageTypeForgetBondedDevices};
    furi_check(
        furi_message_queue_put(bt->message_queue, &message, FuriWaitForever) == FuriStatusOk);
}

void bt_keys_storage_set_storage_path(Bt* bt, const char* keys_storage_path) {
    furi_check(bt);
    furi_check(bt->keys_storage);
    furi_check(keys_storage_path);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    FuriString* path = furi_string_alloc_set(keys_storage_path);
    storage_common_resolve_path_and_ensure_app_directory(storage, path);

    bt_keys_storage_set_file_path(bt->keys_storage, furi_string_get_cstr(path));

    furi_string_free(path);
    furi_record_close(RECORD_STORAGE);
}

void bt_keys_storage_set_default_path(Bt* bt) {
    furi_check(bt);
    furi_check(bt->keys_storage);

    bt_keys_storage_set_file_path(bt->keys_storage, BT_KEYS_STORAGE_PATH);
}

void bt_get_settings(Bt* bt, BtSettings* settings) {
    furi_assert(bt);
    furi_assert(settings);

    BtMessage message = {
        .lock = api_lock_alloc_locked(),
        .type = BtMessageTypeGetSettings,
        .data.settings = settings,
    };

    furi_check(
        furi_message_queue_put(bt->message_queue, &message, FuriWaitForever) == FuriStatusOk);

    api_lock_wait_unlock_and_free(message.lock);
}

bool bt_is_enabled(Bt* bt) {
    furi_assert(bt);
    BtSettings settings;
    bt_get_settings(bt, &settings);
    return settings.enabled;
}

void bt_set_settings(Bt* bt, const BtSettings* settings) {
    furi_assert(bt);
    furi_assert(settings);

    BtMessage message = {
        .lock = api_lock_alloc_locked(),
        .type = BtMessageTypeSetSettings,
        .data.csettings = settings,
    };

    furi_check(
        furi_message_queue_put(bt->message_queue, &message, FuriWaitForever) == FuriStatusOk);

    api_lock_wait_unlock_and_free(message.lock);
}

void bt_stop_stack(Bt* bt) {
    furi_assert(bt);
    BtMessage message = {
        .lock = api_lock_alloc_locked(),
        .type = BtMessageTypeStopStack,
    };
    furi_check(
        furi_message_queue_put(bt->message_queue, &message, FuriWaitForever) == FuriStatusOk);
    api_lock_wait_unlock_and_free(message.lock);
}

void bt_start_stack(Bt* bt) {
    furi_assert(bt);
    BtMessage message = {
        .lock = api_lock_alloc_locked(),
        .type = BtMessageTypeStartStack,
    };
    furi_check(
        furi_message_queue_put(bt->message_queue, &message, FuriWaitForever) == FuriStatusOk);
    api_lock_wait_unlock_and_free(message.lock);
}

/* ---- Service main loop ---- */

int32_t bt_srv(void* p) {
    UNUSED(p);
    Bt* bt = bt_alloc();

    /* Load settings FIRST to know if BLE should be started */
    bt_settings_load(&bt->bt_settings);

    if(bt->bt_settings.enabled) {
        /* Start the BLE stack and default profile */
        if(furi_hal_bt_start_radio_stack()) {
            bt_init_keys_settings(bt);
            furi_hal_bt_set_key_storage_change_callback(bt_on_key_storage_change_callback, bt);
        } else {
            FURI_LOG_E(TAG, "Radio stack start failed");
        }
    } else {
        FURI_LOG_I(TAG, "BT disabled in settings, skipping BLE stack init");
    }

    furi_record_create(RECORD_BT, bt);

    FURI_LOG_I(TAG, "BtSrv ready (enabled=%d)", bt->bt_settings.enabled);

    BtMessage message;

    while(1) {
        furi_check(
            furi_message_queue_get(bt->message_queue, &message, FuriWaitForever) == FuriStatusOk);
        FURI_LOG_D(
            TAG,
            "msg %d, lock 0x%p, result 0x%p",
            message.type,
            (void*)message.lock,
            (void*)message.result);

        if(message.type == BtMessageTypeUpdateStatus) {
            bt_statusbar_update(bt);
            bt_pin_code_hide(bt);
            if(bt->status_changed_cb) {
                bt->status_changed_cb(bt->status, bt->status_changed_ctx);
            }
        } else if(message.type == BtMessageTypeUpdateBatteryLevel) {
            furi_hal_bt_update_battery_level(message.data.battery_level);
        } else if(message.type == BtMessageTypeUpdatePowerState) {
            furi_hal_bt_update_power_state(message.data.power_state_charging);
        } else if(message.type == BtMessageTypePinCodeShow) {
            bt_pin_code_show(bt, message.data.pin_code);
        } else if(message.type == BtMessageTypeKeysStorageUpdated) {
            bt_keys_storage_update(
                bt->keys_storage,
                message.data.key_storage_data.start_address,
                message.data.key_storage_data.size);
        } else if(message.type == BtMessageTypeSetProfile) {
            bt_change_profile(bt, &message);
        } else if(message.type == BtMessageTypeDisconnect) {
            bt_close_connection(bt);
        } else if(message.type == BtMessageTypeForgetBondedDevices) {
            bt_keys_storage_delete(bt->keys_storage);
        } else if(message.type == BtMessageTypeGetSettings) {
            bt_handle_get_settings(bt, &message);
        } else if(message.type == BtMessageTypeSetSettings) {
            bt_handle_set_settings(bt, &message);
        } else if(message.type == BtMessageTypeReloadKeysSettings) {
            bt_handle_reload_keys_settings(bt);
        } else if(message.type == BtMessageTypeStopStack) {
            bt_handle_stop_stack(bt);
        } else if(message.type == BtMessageTypeStartStack) {
            bt_handle_start_stack(bt);
        }

        if(message.lock) api_lock_unlock(message.lock);
    }

    return 0;
}
