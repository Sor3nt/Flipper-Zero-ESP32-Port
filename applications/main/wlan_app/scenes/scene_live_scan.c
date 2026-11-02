#include "../wlan_app.h"
#include "../wlan_live_scanner.h"
#include <gui/view.h>
#include <esp_attr.h>

void wlan_app_scene_live_scan_on_enter(void* context) {
    WlanApp* app = context;

    with_view_model(
        app->view_live_scan, WlanLiveScanModel* m,
        {
            m->cursor = 0;
            m->count = 0;
            m->detail_mode = false;
            m->sort = LiveSortRssi;
            m->filter = LiveFilterAll;
        }, true);

    wlan_live_scan_set_sort(LiveSortRssi);
    wlan_live_scan_set_filter(LiveFilterAll);
    wlan_live_scan_start();
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewLiveScan);
}

bool wlan_app_scene_live_scan_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    // Back: im Detail zurück zur Liste, sonst Scene verlassen (Default).
    if(event.type == SceneManagerEventTypeBack) {
        bool in_detail = false;
        with_view_model(
            app->view_live_scan, WlanLiveScanModel* m,
            { in_detail = m->detail_mode; }, false);
        if(in_detail) {
            with_view_model(
                app->view_live_scan, WlanLiveScanModel* m,
                { m->detail_mode = false; }, true);
            return true;
        }
        return false;
    }

    if(event.type == SceneManagerEventTypeTick) {
        // static: ~5.8 KB, zu groß für den Task-Stack (vgl. SmartDeauth-Fix).
        static EXT_RAM_BSS_ATTR LiveApEntry snap[LIVE_SCAN_MAX_APS];
        uint16_t n = wlan_live_scan_snapshot(snap, LIVE_SCAN_MAX_APS);

        with_view_model(
            app->view_live_scan, WlanLiveScanModel* m,
            {
                // BSSID unter dem Cursor merken, damit die Auswahl beim Re-Sort
                // (RSSI schwankt → Reihenfolge ändert sich) auf demselben AP bleibt.
                uint8_t sel_bssid[6];
                bool have_sel = (m->count > 0 && m->cursor < m->count);
                if(have_sel) memcpy(sel_bssid, m->aps[m->cursor].bssid, 6);

                if(n > 0) memcpy(m->aps, snap, sizeof(LiveApEntry) * n);
                m->count = n;
                m->running = wlan_live_scan_is_running();
                m->hopping = wlan_live_scan_is_hopping();
                m->current_channel = wlan_live_scan_get_channel();
                m->sort = wlan_live_scan_get_sort();
                m->filter = wlan_live_scan_get_filter();

                if(n == 0) {
                    m->cursor = 0;
                } else {
                    if(have_sel) {
                        for(uint16_t i = 0; i < n; i++) {
                            if(memcmp(m->aps[i].bssid, sel_bssid, 6) == 0) {
                                m->cursor = i;
                                break;
                            }
                        }
                    }
                    if(m->cursor >= n) m->cursor = n - 1;
                }
            }, true);
        consumed = true;

    } else if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case WlanAppCustomEventLiveScanUp:
            with_view_model(
                app->view_live_scan, WlanLiveScanModel* m,
                {
                    if(m->count > 0 && !m->detail_mode) {
                        if(m->cursor > 0) m->cursor--;
                        else m->cursor = m->count - 1;   // wrap
                    }
                }, true);
            consumed = true;
            break;

        case WlanAppCustomEventLiveScanDown:
            with_view_model(
                app->view_live_scan, WlanLiveScanModel* m,
                {
                    if(m->count > 0 && !m->detail_mode) {
                        if(m->cursor + 1 < m->count) m->cursor++;
                        else m->cursor = 0;              // wrap
                    }
                }, true);
            consumed = true;
            break;

        case WlanAppCustomEventLiveScanOk:
            with_view_model(
                app->view_live_scan, WlanLiveScanModel* m,
                { if(m->count > 0) m->detail_mode = !m->detail_mode; }, true);
            consumed = true;
            break;

        case WlanAppCustomEventLiveScanLeft: {
            // Detail: zurück zur Liste (ohne zu löschen). Liste: Scan zurücksetzen.
            bool in_detail = false;
            with_view_model(
                app->view_live_scan, WlanLiveScanModel* m,
                { in_detail = m->detail_mode; }, false);
            if(in_detail) {
                with_view_model(
                    app->view_live_scan, WlanLiveScanModel* m,
                    { m->detail_mode = false; }, true);
            } else {
                wlan_live_scan_clear();
                with_view_model(
                    app->view_live_scan, WlanLiveScanModel* m,
                    { m->cursor = 0; m->count = 0; }, true);
            }
            consumed = true;
            break;
        }

        case WlanAppCustomEventLiveScanRight:
            wlan_live_scan_set_hop(!wlan_live_scan_is_hopping());
            consumed = true;
            break;
        }
    }

    return consumed;
}

void wlan_app_scene_live_scan_on_exit(void* context) {
    UNUSED(context);
    wlan_live_scan_stop();
}
