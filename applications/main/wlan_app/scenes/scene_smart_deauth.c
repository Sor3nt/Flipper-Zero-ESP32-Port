#include "../wlan_app.h"
#include "../wlan_smart_deauth.h"
#include <gui/view.h>
#include <esp_attr.h>

void wlan_app_scene_smart_deauth_on_enter(void* context) {
    WlanApp* app = context;

    with_view_model(
        app->view_smart_deauth, SdViewModel* m,
        { m->scan_cursor = 0; }, true);

    wlan_smart_deauth_start();
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewSmartDeauth);
}

bool wlan_app_scene_smart_deauth_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    // Back button intercept
    if(event.type == SceneManagerEventTypeBack) {
        SdViewMode mode = wlan_smart_deauth_get_mode();
        if(mode == SdViewModeAttack) {
            wlan_smart_deauth_back_to_scan();
            return true;
        }
        if(mode == SdViewModeMonitor) {
            wlan_smart_deauth_monitor_exit();
            return true;
        }
        return false;   // scan → exit scene
    }

    if(event.type == SceneManagerEventTypeTick) {
        // SdViewModel kini ~1.1 KB (channels pakai SdChannelView ramping). static
        // agar tidak membebani stack & tanpa churn; aman karena hanya diakses dari
        // thread view_dispatcher.
        static EXT_RAM_BSS_ATTR SdViewModel vm;
        wlan_smart_deauth_get_view_model(&vm);

        with_view_model(
            app->view_smart_deauth, SdViewModel* m,
            {
                uint8_t cursor = m->scan_cursor;
                *m = vm;
                // Daftar scan kini hanya AP terdeteksi; jaga cursor tetap valid saat
                // jumlahnya menyusut setelah rescan.
                if(m->detected_count == 0)
                    cursor = 0;
                else if(cursor >= m->detected_count)
                    cursor = m->detected_count - 1;
                m->scan_cursor = cursor;
            }, true);
        consumed = true;

    } else if(event.type == SceneManagerEventTypeCustom) {
        SdViewMode mode = wlan_smart_deauth_get_mode();

        switch(event.event) {
        case WlanAppCustomEventSmartDeauthUp:
            if(mode == SdViewModeScan) {
                with_view_model(
                    app->view_smart_deauth, SdViewModel* m,
                    { if(m->scan_cursor > 0) m->scan_cursor--; }, true);
            }
            consumed = true;
            break;

        case WlanAppCustomEventSmartDeauthDown:
            if(mode == SdViewModeScan) {
                with_view_model(
                    app->view_smart_deauth, SdViewModel* m,
                    {
                        if(m->detected_count > 0 && m->scan_cursor + 1 < m->detected_count)
                            m->scan_cursor++;
                    }, true);
            }
            consumed = true;
            break;

        case WlanAppCustomEventSmartDeauthOk:
            if(mode == SdViewModeScan) {
                int16_t real = -1;
                with_view_model(
                    app->view_smart_deauth, SdViewModel* m,
                    {
                        // Petakan posisi cursor (daftar terfilter) → index kanal asli.
                        if(m->detected_count > 0 && m->scan_cursor < m->detected_count)
                            real = (int16_t)m->detected_map[m->scan_cursor];
                    }, false);
                if(real >= 0) wlan_smart_deauth_select_channel((uint8_t)real);
            } else if(mode == SdViewModeAttack) {
                wlan_smart_deauth_attack_toggle();
            }
            consumed = true;
            break;

        case WlanAppCustomEventSmartDeauthLeft:
            if(mode == SdViewModeScan) {
                wlan_smart_deauth_scan_now();
            } else if(mode == SdViewModeAttack) {
                wlan_smart_deauth_back_to_scan();
            } else if(mode == SdViewModeMonitor) {
                wlan_smart_deauth_monitor_exit();
            }
            consumed = true;
            break;

        case WlanAppCustomEventSmartDeauthRight:
            if(mode == SdViewModeAttack) {
                wlan_smart_deauth_monitor_enter();
            }
            consumed = true;
            break;
        }
    }
    return consumed;
}

void wlan_app_scene_smart_deauth_on_exit(void* context) {
    UNUSED(context);
    wlan_smart_deauth_stop();
}
