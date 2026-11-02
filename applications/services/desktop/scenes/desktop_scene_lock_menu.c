#include <furi.h>
#include <gui/scene_manager.h>

#include <btshim.h>
#include <wifi.h>

#include "../desktop_i.h"
#include "../views/desktop_view_lock_menu.h"
#include "../helpers/qflipper_bridge.h"
#include "desktop_scene.h"

#include "sdkconfig.h"

/* qFlipper / USB-Storage need USB-OTG (ESP32-S3 / S2 only). */
#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32S2
#define LOCK_MENU_USB_AVAILABLE true
#else
#define LOCK_MENU_USB_AVAILABLE false
#endif

void desktop_scene_lock_menu_callback(DesktopEvent event, void* context) {
    Desktop* desktop = (Desktop*)context;
    view_dispatcher_send_custom_event(desktop->view_dispatcher, event);
}

static bool desktop_lock_menu_bt_enabled(void) {
    Bt* bt = furi_record_open(RECORD_BT);
    BtSettings settings;
    bt_get_settings(bt, &settings);
    furi_record_close(RECORD_BT);
    return settings.enabled;
}

static bool desktop_lock_menu_wifi_enabled(void) {
    Wifi* wifi = furi_record_open(RECORD_WIFI);
    bool enabled = wifi_is_enabled(wifi);
    furi_record_close(RECORD_WIFI);
    return enabled;
}

/* Rebuild the menu from the live toggle states (used on enter and after a
 * toggle, so the Enable/Disable labels track reality). */
void desktop_scene_lock_menu_refresh(Desktop* desktop) {
    desktop_lock_menu_set_states(
        desktop->lock_menu,
        LOCK_MENU_USB_AVAILABLE,
        qflipper_bridge_is_active(),
        desktop_lock_menu_bt_enabled(),
        desktop_lock_menu_wifi_enabled());
}

void desktop_scene_lock_menu_on_enter(void* context) {
    Desktop* desktop = (Desktop*)context;

    desktop_lock_menu_set_callback(desktop->lock_menu, desktop_scene_lock_menu_callback, desktop);
    desktop_scene_lock_menu_refresh(desktop);

    view_dispatcher_switch_to_view(desktop->view_dispatcher, DesktopViewIdLockMenu);
}

bool desktop_scene_lock_menu_on_event(void* context, SceneManagerEvent event) {
    Desktop* desktop = (Desktop*)context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case DesktopLockMenuEventQflipperToggle:
            if(!desktop_system_task_is_pending(desktop)) {
                desktop_system_task_start_qflipper(desktop, !qflipper_bridge_is_active());
            }
            consumed = true;
            break;

        case DesktopLockMenuEventUsbStorage:
            if(desktop_system_task_is_pending(desktop)) {
                consumed = true;
                break;
            }
            /* The USB-Storage scene stops the qFlipper bridge itself (shared
             * composite / mutual exclusion). */
            scene_manager_next_scene(desktop->scene_manager, DesktopSceneUsbStorage);
            consumed = true;
            break;

        case DesktopLockMenuEventBluetoothToggle: {
            if(desktop_system_task_is_pending(desktop)) {
                consumed = true;
                break;
            }
            bool want_on = !desktop_lock_menu_bt_enabled();
            desktop_system_task_toggle_bluetooth(desktop, want_on);
            consumed = true;
            break;
        }

        case DesktopLockMenuEventWifiToggle: {
            if(desktop_system_task_is_pending(desktop)) {
                consumed = true;
                break;
            }
            /* WiFi global toggeln. Enable schaltet BLE aus + persistiert, zieht das
             * Radio hoch und reconnectet zum zuletzt genutzten Netz (siehe
             * wifi_enable). Disable baut das Radio ab; BLE bleibt bewusst aus. */
            Wifi* wifi = furi_record_open(RECORD_WIFI);
            bool want_on = !wifi_is_enabled(wifi);
            furi_record_close(RECORD_WIFI);
            desktop_system_task_toggle_wifi(desktop, want_on);
            consumed = true;
            break;
        }

        case DesktopLockMenuEventMeshClients:
            /* T-Embed ist immer Master; der Master-Mesh-Service läuft on-demand in
             * der Mesh-Clients-Scene. */
            scene_manager_next_scene(desktop->scene_manager, DesktopSceneMeshClients);
            consumed = true;
            break;

        case DesktopLockMenuEventWebFs:
            /* Web-Filesystem lives in the WiFi app; launch it into that flow. */
            loader_start_detached_with_gui_error(desktop->loader, "wlan", "webfs");
            consumed = true;
            break;

        default:
            break;
        }
    }

    return consumed;
}

void desktop_scene_lock_menu_on_exit(void* context) {
    UNUSED(context);
}
