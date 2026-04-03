/**
 * @file bt_i.h
 * BT service internal header — ESP32 version
 *
 * Based on STM32 bt_i.h, with RPC/Power conditionally excluded.
 */

#pragma once

#include "btshim.h"

#include <furi.h>
#include <furi_hal_bt.h>
#include <api_lock.h>

#include <gui/gui.h>
#include <gui/view_port.h>
#include <gui/view.h>

#include <notification/notification.h>
#include <storage/storage.h>
#include <rpc/rpc.h>

#include "bt_settings.h"
#include "bt_keys_storage.h"
#include "bt_keys_filename.h"

#define BT_KEYS_STORAGE_PATH INT_PATH(BT_KEYS_STORAGE_FILE_NAME)

typedef enum {
    BtMessageTypeUpdateStatus,
    BtMessageTypeUpdateBatteryLevel,
    BtMessageTypeUpdatePowerState,
    BtMessageTypePinCodeShow,
    BtMessageTypeKeysStorageUpdated,
    BtMessageTypeSetProfile,
    BtMessageTypeDisconnect,
    BtMessageTypeForgetBondedDevices,
    BtMessageTypeGetSettings,
    BtMessageTypeSetSettings,
    BtMessageTypeReloadKeysSettings,
    BtMessageTypeStopStack,
    BtMessageTypeStartStack,
} BtMessageType;

typedef struct {
    uint8_t* start_address;
    uint16_t size;
} BtKeyStorageUpdateData;

typedef union {
    uint32_t pin_code;
    uint8_t battery_level;
    bool power_state_charging;
    struct {
        const FuriHalBleProfileTemplate* template;
        FuriHalBleProfileParams params;
    } profile;
    FuriHalBleProfileParams profile_params;
    BtKeyStorageUpdateData key_storage_data;
    BtSettings* settings;
    const BtSettings* csettings;
} BtMessageData;

typedef struct {
    FuriApiLock lock;
    BtMessageType type;
    BtMessageData data;
    bool* result;
    FuriHalBleProfileBase** profile_instance;
} BtMessage;

struct Bt {
    uint8_t* bt_keys_addr_start;
    uint16_t bt_keys_size;
    uint16_t max_packet_size;
    BtSettings bt_settings;
    BtKeysStorage* keys_storage;
    BtStatus status;
    bool beacon_active;
    FuriHalBleProfileBase* current_profile;
    FuriMessageQueue* message_queue;
    NotificationApp* notification;
    Gui* gui;
    ViewPort* statusbar_view_port;
    ViewPort* pin_code_view_port;
    uint32_t pin_code;
    Rpc* rpc;
    RpcSession* rpc_session;
    FuriEventFlag* rpc_event;
    BtStatusChangedCallback status_changed_cb;
    void* status_changed_ctx;
    uint32_t pin;
    bool suppress_pin_screen;

    /* Intermediate RX buffer: GATTS callback writes here (non-blocking),
     * dedicated feeder thread drains into RPC session (can block safely). */
    FuriStreamBuffer* rx_stream;
    FuriThread* rx_thread;
};
