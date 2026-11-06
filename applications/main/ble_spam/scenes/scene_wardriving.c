#include "../ble_spam_app.h"
#include "../views/ble_wardriving_view.h"
#include <gui/view.h>
#include <esp_attr.h>
#include <storage/storage.h>
#include <furi_hal_rtc.h>
#include <boards/board.h>

void ble_spam_scene_wardriving_on_enter(void* context) {
    BleSpamApp* app = context;

    with_view_model(
        app->view_wardriving, BleWardrivingModel* m,
        {
            m->cursor = 0;
            m->count = 0;
            m->total = 0;
            m->detail_mode = false;
            m->exit_pending = false;
            m->detail_valid = false;
        }, true);

    // start() non-blocking: bring-up BT jalan di worker thread (tak freeze UI).
    wlan_ble_scan_start();
    view_dispatcher_switch_to_view(app->view_dispatcher, BleSpamViewWardriving);
}

bool ble_spam_scene_wardriving_on_event(void* context, SceneManagerEvent event) {
    BleSpamApp* app = context;
    bool consumed = false;

    // Back always exits the scanner; Left returns from details to the list.
    if(event.type == SceneManagerEventTypeBack) {
        with_view_model(
            app->view_wardriving, BleWardrivingModel* m,
            { m->exit_pending = true; }, true);
        wlan_ble_scan_stop();
        return true;
    }

    if(event.type == SceneManagerEventTypeTick) {
        bool exit_pending = false;
        with_view_model(
            app->view_wardriving, BleWardrivingModel* m,
            { exit_pending = m->exit_pending; }, false);
        if(exit_pending) {
            if(wlan_ble_scan_is_stopped()) scene_manager_previous_scene(app->scene_manager);
            return true;
        }

        static EXT_RAM_BSS_ATTR BleDeviceView snap[BLE_SCAN_VIEW_ENTRIES];
        uint16_t n = wlan_ble_scan_snapshot_view(snap, BLE_SCAN_VIEW_ENTRIES);

        with_view_model(
            app->view_wardriving, BleWardrivingModel* m,
            {
                // Pertahankan highlight pada MAC yang sama saat urutan RSSI berubah.
                uint8_t sel_mac[6];
                bool have_sel = (m->count > 0 && m->cursor < m->count);
                if(have_sel) memcpy(sel_mac, m->devices[m->cursor].mac, 6);

                if(n > 0) memcpy(m->devices, snap, sizeof(BleDeviceView) * n);
                m->count = n;
                m->total = wlan_ble_scan_get_count();
                m->running = wlan_ble_scan_is_running();

                if(n == 0) {
                    m->cursor = 0;
                } else {
                    if(have_sel) {
                        for(uint16_t i = 0; i < n; i++) {
                            if(memcmp(m->devices[i].mac, sel_mac, 6) == 0) {
                                m->cursor = i;
                                break;
                            }
                        }
                    }
                    if(m->cursor >= n) m->cursor = n - 1;
                }

                // Segarkan detail dari store (device bisa hilang → detail_valid=false).
                if(m->detail_mode && m->count > 0) {
                    m->detail_valid =
                        wlan_ble_scan_get_full_by_mac(m->devices[m->cursor].mac, &m->detail);
                } else if(m->detail_mode) {
                    m->detail_valid = false;
                }
            }, true);
        consumed = true;

    } else if(event.type == SceneManagerEventTypeCustom) {
        uint32_t ev = event.event;
#if defined(BOARD_HAS_ENCODER) && BOARD_HAS_ENCODER
        // Encoder boards (T-Embed, Thorchimera): a plain knob turn emits Up/Down,
        // while Left/Right require the awkward hold-knob-and-turn gesture. In the device
        // detail view there is no list to scroll, so remap the knob onto the Left/Right
        // actions — Up = back to list, Down = export CSV. The list view keeps Up/Down
        // for scrolling. Button boards are unaffected (macro undefined/0).
        {
            bool in_detail = false;
            with_view_model(
                app->view_wardriving, BleWardrivingModel* m,
                { in_detail = m->detail_mode; }, false);
            if(in_detail) {
                if(ev == BleSpamWardrivingUp) ev = BleSpamWardrivingLeft;
                else if(ev == BleSpamWardrivingDown) ev = BleSpamWardrivingRight;
            }
        }
#endif
        switch(ev) {
        case BleSpamWardrivingUp:
            with_view_model(
                app->view_wardriving, BleWardrivingModel* m,
                {
                    if(m->count > 0 && !m->detail_mode) {
                        if(m->cursor > 0) m->cursor--;
                        else m->cursor = m->count - 1;
                    }
                }, true);
            consumed = true;
            break;

        case BleSpamWardrivingDown:
            with_view_model(
                app->view_wardriving, BleWardrivingModel* m,
                {
                    if(m->count > 0 && !m->detail_mode) {
                        if(m->cursor + 1 < m->count) m->cursor++;
                        else m->cursor = 0;
                    }
                }, true);
            consumed = true;
            break;

        case BleSpamWardrivingOk:
            with_view_model(
                app->view_wardriving, BleWardrivingModel* m,
                { if(m->count > 0) m->detail_mode = !m->detail_mode; }, true);
            consumed = true;
            break;

        case BleSpamWardrivingLeft: {
            bool in_detail = false;
            with_view_model(
                app->view_wardriving, BleWardrivingModel* m,
                { in_detail = m->detail_mode; }, false);
            if(in_detail) {
                with_view_model(
                    app->view_wardriving, BleWardrivingModel* m,
                    { m->detail_mode = false; }, true);
            } else {
                wlan_ble_scan_clear();
                with_view_model(
                    app->view_wardriving, BleWardrivingModel* m,
                    { m->cursor = 0; m->count = 0; }, true);
            }
            consumed = true;
            break;
        }

        case BleSpamWardrivingRight: {
            // Export CSV ke /ext/apps_data/wlan dengan nama berstempel waktu unik.
            Storage* storage = furi_record_open(RECORD_STORAGE);
            char path[96];
            DateTime dt;
            furi_hal_rtc_get_datetime(&dt);
            for(int attempt = 0; attempt < 10; attempt++) {
                if(attempt == 0) {
                    snprintf(path, sizeof(path),
                             "/ext/apps_data/wlan/ble_%04u%02u%02u_%02u%02u%02u.csv",
                             dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
                } else {
                    snprintf(path, sizeof(path),
                             "/ext/apps_data/wlan/ble_%04u%02u%02u_%02u%02u%02u_%d.csv",
                             dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second, attempt);
                }
                if(!storage_common_exists(storage, path)) break;
            }
            furi_record_close(RECORD_STORAGE);

            uint16_t count = 0;
            if(wlan_ble_scan_export_csv(path, &count)) {
                FURI_LOG_I("Wardriving", "Exported %u BLE devices", count);
            } else {
                FURI_LOG_E("Wardriving", "CSV export failed");
            }
            consumed = true;
            break;
        }
        }
    }

    return consumed;
}

void ble_spam_scene_wardriving_on_exit(void* context) {
    UNUSED(context);
    wlan_ble_scan_stop();
}
