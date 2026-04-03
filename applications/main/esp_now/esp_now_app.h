#pragma once

#include <furi.h>
#include <gui/gui.h>
#include <gui/scene_manager.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/widget.h>

#include "esp_now_packet.h"
#include "scenes/scenes.h"

#define ESP_NOW_PACKETS_MAX 64

typedef enum {
    EspNowViewSubmenu,
    EspNowViewWidget,
} EspNowView;

typedef enum {
    EspNowCustomEventPacketsUpdated = 100,
} EspNowCustomEvent;

typedef struct {
    Gui* gui;
    SceneManager* scene_manager;
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    Widget* widget;

    EspNowPacket* packets;
    size_t packet_count;
    size_t packet_capacity;
    FuriMutex* mutex;

    size_t selected_index;
    size_t last_displayed_count;
    bool sniffing;

    FuriString* text_buf;
} EspNowApp;
