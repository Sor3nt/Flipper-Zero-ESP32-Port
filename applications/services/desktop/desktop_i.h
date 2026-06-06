#pragma once

#include "desktop.h"
#include "desktop_settings.h"

#include "animations/animation_manager.h"
#include "views/desktop_view_pin_timeout.h"
#include "views/desktop_view_pin_input.h"
#include "views/desktop_view_locked.h"
#include "views/desktop_view_main.h"
#include "views/desktop_view_lock_menu.h"
#include "views/desktop_view_usb_storage.h"
#include "views/desktop_view_mesh_clients.h"
#include "views/desktop_view_mesh_action.h"
#include "views/desktop_view_mesh_device.h"
#include "views/desktop_view_mesh_wifi.h"
#include "views/desktop_view_mesh_handshake.h"
#include "views/desktop_view_debug.h"
#include "views/desktop_view_slideshow.h"
#include "helpers/mesh_config.h"
#include "helpers/mesh_service.h"

#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_stack.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/popup.h>
#include <gui/modules/dialog_ex.h>
#include <gui/scene_manager.h>

#include <loader/loader.h>
#include <notification/notification_app.h>
#include <notification/notification_messages.h>

#define STATUS_BAR_Y_SHIFT 13

typedef enum {
    DesktopViewIdMain,
    DesktopViewIdLockMenu,
    DesktopViewIdUsbStorage,
    DesktopViewIdMeshClients,
    DesktopViewIdMeshAction,
    DesktopViewIdMeshDevice,
    DesktopViewIdMeshWifi,
    DesktopViewIdMeshHandshake,
    DesktopViewIdMeshPair,
    DesktopViewIdLocked,
    DesktopViewIdDebug,
    DesktopViewIdPopup,
    DesktopViewIdPinInput,
    DesktopViewIdPinTimeout,
    DesktopViewIdSlideshow,
    DesktopViewIdTotal,
} DesktopViewId;

typedef struct {
    uint8_t hour;
    uint8_t minute;
    bool format_12; // 1 - 12 hour, 0 - 24H
} DesktopClock;

struct Desktop {
    FuriThread* scene_thread;

    Gui* gui;
    ViewDispatcher* view_dispatcher;
    SceneManager* scene_manager;

    Popup* popup;
    DialogEx* mesh_pair_dialog;
    DesktopLockMenuView* lock_menu;
    DesktopUsbStorageView* usb_storage_view;
    DesktopMeshClientsView* mesh_clients_view;
    DesktopMeshActionView* mesh_action_view;
    DesktopMeshDeviceView* mesh_device_view;
    DesktopMeshWifiView* mesh_wifi_view;
    DesktopMeshHandshakeView* mesh_handshake_view;
    DesktopDebugView* debug_view;
    DesktopViewLocked* locked_view;
    DesktopMainView* main_view;
    DesktopViewPinTimeout* pin_timeout_view;
    DesktopSlideshowView* slideshow_view;
    DesktopViewPinInput* pin_input_view;

    ViewStack* main_view_stack;
    ViewStack* locked_view_stack;

    ViewPort* lock_icon_viewport;
    ViewPort* dummy_mode_icon_viewport;
    ViewPort* clock_viewport;
    ViewPort* stealth_mode_icon_viewport;

    Loader* loader;
    Storage* storage;
    NotificationApp* notification;

    FuriPubSub* status_pubsub;
    FuriPubSub* input_events_pubsub;
    FuriPubSubSubscription* input_events_subscription;

    FuriTimer* auto_lock_timer;
    FuriTimer* update_clock_timer;

    AnimationManager* animation_manager;
    FuriSemaphore* animation_semaphore;

    DesktopClock clock;
    DesktopSettings settings;

    bool in_transition;
    bool app_running;
    bool locked;

    /* Phase-1 Mesh-State. mesh_mode wird beim Boot aus /ext/mesh/mode.txt
     * geladen und vom Lock-Menü-Toggle aktualisiert. mesh_pending hält die
     * Daten zum letzten Pair/Disconnect-Event, das der Background-Service ans
     * Main-Scene-Custom-Event-Handling reicht (single-shot — der Handler
     * verarbeitet und ignoriert weitere Requests bis er fertig ist). */
    MeshMode mesh_mode;
    MeshEventData mesh_pending;

    /* Master: gewählter Client für die Action-Scene; handoff-Flag hält den
     * Mesh-Service beim Wechsel Clients→Action am Leben. Der laufende Status der
     * Clients wird NICHT hier gecached — er kommt live vom Buddy. */
    MeshPeer mesh_action_client;
    bool mesh_keep_service;
    /* Vorab bekannte Feature-Liste des gewählten Clients (aus der Clients-Scene),
     * damit die Action-Scene sofort anzeigt statt erneut auf eine Antwort zu
     * warten. feature_count==0 = nichts bekannt → Action-Scene fragt selbst ab. */
    MeshFeature mesh_action_features[MESH_FEATURES_MAX];
    uint8_t mesh_action_feature_count;
    uint32_t mesh_action_running_mask;
    uint8_t mesh_action_channel; /* bekannter Kanal des gewählten Buddys (0 = unbekannt) */

    /* Result-Overlay ("Handshake received"): 3-s-Timer + Dedup gegen wiederholte
     * Result-Frames (Buddy sendet bis zum Ack erneut). */
    FuriTimer* mesh_overlay_timer;
    uint8_t mesh_last_result_mac[MESH_MAC_LEN];
    uint8_t mesh_last_result_id;
    bool mesh_last_result_valid;
};

void desktop_lock(Desktop* desktop);
void desktop_unlock(Desktop* desktop);
void desktop_set_dummy_mode_state(Desktop* desktop, bool enabled);
void desktop_set_stealth_mode_state(Desktop* desktop, bool enabled);

/* Mesh-Callback (impl in desktop.c): packt das Event in desktop->mesh_pending
 * und feuert DesktopMeshEventClient{PairRequest,Disconnect} via
 * view_dispatcher_send_custom_event. Aus dem Mesh-Service-Worker-Task sicher
 * aufrufbar (view_dispatcher hat eigene message queue). */
void desktop_mesh_event_cb(const MeshEventData* ev, void* ctx);
