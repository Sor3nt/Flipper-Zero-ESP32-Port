#pragma once

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_usb.h>
#include <furi_hal_usb_cdc.h>
#include <gui/gui.h>
#include <gui/elements.h>
#include <notification/notification_messages.h>
#include <input/input.h>
#include <input/input_canonical.h>
#include <storage/storage.h>

#include "views/bars_view.h"
#include "views/connect_view.h"
#include "views/status_view.h"

#define TAG "PCMonitorUSB"

#define SCREEN_HEIGHT 64
#define LINE_HEIGHT   11

#define BAR_X     30
#define BAR_WIDTH 97

typedef enum {
    UsbStateWaiting,
    UsbStateActive,
    UsbStateInactive,
    UsbStateLost,
    UsbStateBusy, // ESP32 port: the only USB serial port belongs to qFlipper / the CLI / another app
    UsbStateError, // ESP32 port: the USB serial port could not be started
} UsbState;

#pragma pack(push, 1)
typedef struct {
    uint8_t cpu_usage;
    uint16_t ram_max;
    uint8_t ram_usage;
    char ram_unit[4];
    uint8_t gpu_usage;
    uint16_t vram_max;
    uint8_t vram_usage;
    char vram_unit[4];
} DataStruct;
#pragma pack(pop)

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* event_queue;
    NotificationApp* notification;

    UsbState usb_state;
    DataStruct data;
    uint32_t last_packet;
    uint8_t lines_count;
    volatile bool new_data_received;

    // ESP32 port: what this app has taken over and must give back
    bool cdc_claimed; // CDC0 callbacks are ours
    bool composite_installed_by_us; // we created the USB composite (and routed the PHY to OTG)
    bool sleep_inhibited;
    InputCanonical canon;
} PcMonitorApp;
