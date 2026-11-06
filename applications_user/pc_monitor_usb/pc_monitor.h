#pragma once

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_usb.h>
#include <furi_hal_usb_cdc.h>
#include <gui/gui.h>
#include <gui/elements.h>
#include <notification/notification_messages.h>
#include <input/input.h>
#include <storage/storage.h>
#include <cli/cli_vcp.h>

#include <stdatomic.h>
#include <string.h>

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
    UsbStateBusy, // ESP32 port: the USB port is leased to another feature (or USB is locked)
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
    // Guards usb_state / data / lines_count: the main thread writes them, the GUI thread
    // reads (and, in draw_bars_view, updates) them.
    FuriMutex* mutex;

    UsbState usb_state;
    DataStruct data;
    uint32_t last_packet;
    uint8_t lines_count;

    // Reassembly of the 16-byte packets: a USB read may return part of one or several.
    uint8_t rx_buf[sizeof(DataStruct)];
    uint8_t rx_len;
    uint32_t rx_partial_tick;

    // ESP32 port: what this app has taken over and must give back, in the order it was
    // taken (see pc_monitor_usb_acquire / pc_monitor_usb_release in pc_monitor.c).
    FuriHalUsbInterface* usb_previous;
    CliVcp* cli_vcp;
    bool usb_lease; // we own the USB lease
    bool cli_record_open;
    bool cli_disabled; // we turned the CLI's USB console off
    bool usb_composite; // we created the USB composite (and routed the PHY to OTG)
    bool cdc_claimed; // CDC0 callbacks are ours
    bool sleep_inhibited;
} PcMonitorApp;
