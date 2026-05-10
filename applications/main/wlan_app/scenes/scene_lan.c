#include "../wlan_app.h"
#include "../wlan_netcut.h"
#include <assets_icons.h>

// Action user_ids im Custom-View. Devices nutzen ihren Index direkt
// (0..WLAN_APP_MAX_DEVICES-1).
#define LAN_ACTION_BASE         1000
#define LAN_ACTION_SELECT_ALL   (LAN_ACTION_BASE + 0)
#define LAN_ACTION_ATTACK_NET   (LAN_ACTION_BASE + 1)
#define LAN_ACTION_ATTACK_TGT   (LAN_ACTION_BASE + 2)
#define LAN_ACTION_RESCAN       (LAN_ACTION_BASE + 3)

// Menu-Item user_ids (Long-OK Modal).
#define LAN_MENU_BLOCK    1
#define LAN_MENU_THROTTLE 2
#define LAN_MENU_PORTSCAN 3
#define LAN_MENU_SNIFFER  4

#define LAN_THROTTLE_DEFAULT_KBPS 64
#define LAN_RESTORING_TICKS 12 // 12 * 250 ms ≈ 3 s

static uint16_t s_lan_restoring_ticks;

// Zeigt die letzten zwei Octets der IP (Network-Byte-Order) als "Subnet.Host".
static void lan_format_device_ip(const WlanDeviceRecord* d, char* out, size_t out_sz) {
    uint32_t ip = d->ip;
    snprintf(out, out_sz, "%u.%u",
        (unsigned)((ip >> 16) & 0xFF), (unsigned)((ip >> 24) & 0xFF));
}

// Liefert Anzeige-Wert eines Devices: Block/Throttle haben Vorrang, dann
// TARGET (als Icon), sonst "VIP" (Default).
static void lan_compute_device_value(
    const WlanDeviceRecord* d,
    const char** out_label,
    const Icon** out_icon,
    bool* out_is_default) {
    *out_label = NULL;
    *out_icon = NULL;
    *out_is_default = false;

    if(d->block_internet) {
        *out_label = "CUT";
    } else if(d->throttle_kbps) {
        *out_label = "Slow";
    } else if(d->active) {
        *out_icon = &I_Pin_star_7x7;
    } else {
        *out_label = "VIP";
        *out_is_default = true;
    }
}

static void lan_set_device_view_value(View* view, uint16_t user_id, const WlanDeviceRecord* d) {
    const char* label;
    const Icon* icon;
    bool is_default;
    lan_compute_device_value(d, &label, &icon, &is_default);
    wlan_lan_view_set_device_value_by_user_id(view, user_id, label, icon, is_default);
}

static bool lan_has_targets(const WlanApp* app) {
    for(uint16_t i = 0; i < app->device_count; ++i) {
        if(app->devices[i].active) return true;
    }
    return false;
}

static void lan_popup_done_cb(void* context) {
    WlanApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, WlanAppCustomEventLanPopupDone);
}

static void lan_show_popup(WlanApp* app, const char* text) {
    popup_reset(app->popup);
    popup_set_header(app->popup, "WiFi", 64, 10, AlignCenter, AlignTop);
    popup_set_text(app->popup, text, 64, 32, AlignCenter, AlignCenter);
    popup_set_context(app->popup, app);
    popup_set_callback(app->popup, lan_popup_done_cb);
    popup_set_timeout(app->popup, 1500);
    popup_enable_timeout(app->popup);
    app->lan_popup_active = true;
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewPopup);
}

static void lan_close_popup(WlanApp* app) {
    popup_reset(app->popup);
    app->lan_popup_active = false;
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewLan);
}

static void lan_show_restoring(WlanApp* app) {
    widget_reset(app->widget);
    // Gerundeter Rahmen mittig + Text drin.
    widget_add_rect_element(app->widget, 14, 22, 100, 20, 3, false);
    widget_add_string_element(
        app->widget, 64, 32, AlignCenter, AlignCenter, FontSecondary, "Restoring device...");
    s_lan_restoring_ticks = LAN_RESTORING_TICKS;
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewWidget);
}

static void lan_finish_restoring(WlanApp* app) {
    widget_reset(app->widget);
    s_lan_restoring_ticks = 0;
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewLan);
}

static void lan_rebuild(WlanApp* app) {
    View* v = app->view_lan;
    wlan_lan_view_clear(v);

    wlan_lan_view_add_action_centered(v, "Un/Select All", LAN_ACTION_SELECT_ALL);
    wlan_lan_view_add_separator(v);

    for(uint16_t i = 0; i < app->device_count; ++i) {
        char ip_buf[WLAN_LAN_VIEW_LABEL_MAX];
        lan_format_device_ip(&app->devices[i], ip_buf, sizeof(ip_buf));
        const char* host = app->devices[i].hostname[0] ? app->devices[i].hostname : "?";
        const char* val_label;
        const Icon* val_icon;
        bool is_default;
        lan_compute_device_value(&app->devices[i], &val_label, &val_icon, &is_default);
        wlan_lan_view_add_device(v, ip_buf, host, val_label, val_icon, is_default, i);
    }

    // Attack Targets + Separator — gleiches Spacing wie zwischen
    // "Un/Select All" und Device-Liste.
    wlan_lan_view_add_action_centered(v, "Attack Targets", LAN_ACTION_ATTACK_TGT);
    wlan_lan_view_add_separator(v);

    // Restliche Aktionen.
    wlan_lan_view_add_action_centered(v, "Attack Network", LAN_ACTION_ATTACK_NET);
    wlan_lan_view_add_action_centered(v, "Re-Scan", LAN_ACTION_RESCAN);

    uint8_t restore = scene_manager_get_scene_state(app->scene_manager, WlanAppSceneLan);
    wlan_lan_view_set_selected(v, restore);
}

static void lan_toggle_all(WlanApp* app) {
    bool any_idle = false;
    for(uint16_t i = 0; i < app->device_count; ++i) {
        if(!app->devices[i].active) {
            any_idle = true;
            break;
        }
    }
    for(uint16_t i = 0; i < app->device_count; ++i) {
        app->devices[i].active = any_idle;
    }
}

void wlan_app_scene_lan_on_enter(void* context) {
    WlanApp* app = context;
    lan_rebuild(app);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewLan);
}

bool wlan_app_scene_lan_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom &&
       event.event == WlanAppCustomEventLanItemOk) {
        uint8_t sel = wlan_lan_view_get_selected(app->view_lan);
        WlanLanItem it = wlan_lan_view_get_item(app->view_lan, sel);
        scene_manager_set_scene_state(app->scene_manager, WlanAppSceneLan, sel);

        if(it.kind == WlanLanItemKindDevice) {
            uint16_t dev_idx = it.user_id;
            if(dev_idx < app->device_count) {
                WlanDeviceRecord* d = &app->devices[dev_idx];
                bool was_active = d->block_internet || d->throttle_kbps;
                if(was_active) {
                    // Aktive Attacke beenden → zurück zu VIP.
                    d->block_internet = false;
                    d->throttle_kbps = 0;
                    d->active = false;
                } else {
                    // VIP ↔ TARGET togglen.
                    d->active = !d->active;
                }
                lan_set_device_view_value(app->view_lan, dev_idx, d);
                if(was_active) {
                    bool restoring = wlan_netcut_apply(
                        app->netcut, app->devices, app->device_count);
                    if(restoring) lan_show_restoring(app);
                }
            }
            consumed = true;
        } else {
            switch(it.user_id) {
            case LAN_ACTION_SELECT_ALL:
                lan_toggle_all(app);
                lan_rebuild(app);
                consumed = true;
                break;
            case LAN_ACTION_ATTACK_NET:
                scene_manager_next_scene(app->scene_manager, WlanAppSceneNetworkActions);
                consumed = true;
                break;
            case LAN_ACTION_ATTACK_TGT:
                if(!lan_has_targets(app)) {
                    lan_show_popup(app, "No Targets selected");
                } else {
                    scene_manager_next_scene(app->scene_manager, WlanAppSceneAttackTargets);
                }
                consumed = true;
                break;
            case LAN_ACTION_RESCAN:
                app->lan_scan_complete = false;
                scene_manager_next_scene(app->scene_manager, WlanAppSceneNetworkScanning);
                consumed = true;
                break;
            }
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        if(s_lan_restoring_ticks > 0) {
            s_lan_restoring_ticks--;
            if(s_lan_restoring_ticks == 0) lan_finish_restoring(app);
        }
    } else if(event.type == SceneManagerEventTypeCustom &&
              event.event == WlanAppCustomEventLanPopupDone) {
        lan_close_popup(app);
        consumed = true;
    } else if(event.type == SceneManagerEventTypeBack) {
        if(app->lan_popup_active) {
            lan_close_popup(app);
        } else {
            // Network-Scanning würde sofort wieder zu LAN leiten → direkt zu Main.
            scene_manager_search_and_switch_to_previous_scene(
                app->scene_manager, WlanAppSceneMain);
        }
        consumed = true;
    } else if(event.type == SceneManagerEventTypeCustom &&
              event.event == WlanAppCustomEventLanItemLongOk) {
        // Long-OK auf Device → Modal-Menu öffnen.
        uint8_t sel = wlan_lan_view_get_selected(app->view_lan);
        WlanLanItem it = wlan_lan_view_get_item(app->view_lan, sel);
        if(it.kind == WlanLanItemKindDevice && it.user_id < app->device_count) {
            app->lan_menu_device_idx = (int16_t)it.user_id;
            const WlanDeviceRecord* d = &app->devices[it.user_id];
            wlan_lan_view_clear_menu(app->view_lan);
            wlan_lan_view_add_menu_item(
                app->view_lan,
                d->block_internet ? "Unblock Inet" : "Block Inet",
                LAN_MENU_BLOCK);
            wlan_lan_view_add_menu_item(
                app->view_lan,
                d->throttle_kbps ? "Throttled" : "Throttle",
                LAN_MENU_THROTTLE);
            wlan_lan_view_add_menu_item(app->view_lan, "Ports", LAN_MENU_PORTSCAN);
            wlan_lan_view_add_menu_item(app->view_lan, "Sniffer", LAN_MENU_SNIFFER);
            wlan_lan_view_open_menu(app->view_lan);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeCustom &&
              event.event == WlanAppCustomEventLanMenuOk) {
        uint8_t mi = wlan_lan_view_get_menu_selected(app->view_lan);
        WlanLanMenuItem mit = wlan_lan_view_get_menu_item(app->view_lan, mi);
        int16_t devidx = app->lan_menu_device_idx;
        wlan_lan_view_close_menu(app->view_lan);

        if(devidx >= 0 && devidx < (int16_t)app->device_count) {
            WlanDeviceRecord* d = &app->devices[devidx];
            switch(mit.user_id) {
            case LAN_MENU_BLOCK:
                d->block_internet = !d->block_internet;
                if(d->block_internet) d->throttle_kbps = 0; // exklusiv
                lan_set_device_view_value(app->view_lan, (uint16_t)devidx, d);
                {
                    bool r = wlan_netcut_apply(app->netcut, app->devices, app->device_count);
                    if(r) lan_show_restoring(app);
                }
                consumed = true;
                break;
            case LAN_MENU_THROTTLE:
                if(d->throttle_kbps) {
                    d->throttle_kbps = 0;
                } else {
                    d->throttle_kbps = LAN_THROTTLE_DEFAULT_KBPS;
                    d->block_internet = false; // exklusiv
                }
                lan_set_device_view_value(app->view_lan, (uint16_t)devidx, d);
                {
                    bool r = wlan_netcut_apply(app->netcut, app->devices, app->device_count);
                    if(r) lan_show_restoring(app);
                }
                consumed = true;
                break;
            case LAN_MENU_PORTSCAN:
                for(uint16_t i = 0; i < app->device_count; ++i) {
                    app->devices[i].active = (i == (uint16_t)devidx);
                }
                scene_manager_next_scene(app->scene_manager, WlanAppScenePortScanner);
                consumed = true;
                break;
            case LAN_MENU_SNIFFER:
                for(uint16_t i = 0; i < app->device_count; ++i) {
                    app->devices[i].active = (i == (uint16_t)devidx);
                }
                scene_manager_next_scene(app->scene_manager, WlanAppScenePackageSniffer);
                consumed = true;
                break;
            }
        }
        app->lan_menu_device_idx = -1;
    }

    return consumed;
}

void wlan_app_scene_lan_on_exit(void* context) {
    WlanApp* app = context;
    wlan_lan_view_close_menu(app->view_lan);
    wlan_lan_view_clear_menu(app->view_lan);
    wlan_lan_view_clear(app->view_lan);
    popup_reset(app->popup);
    if(s_lan_restoring_ticks > 0) {
        widget_reset(app->widget);
        s_lan_restoring_ticks = 0;
    }
    app->lan_menu_device_idx = -1;
    app->lan_popup_active = false;
}
