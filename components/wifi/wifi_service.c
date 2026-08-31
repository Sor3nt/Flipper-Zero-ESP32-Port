#include "wifi.h"
#include "wifi_settings.h"
#include "wlan_hal.h"
#include "wlan_passwords.h"

#include <furi.h>
#include <api_lock.h>
#include <btshim.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <storage/storage.h>
#include <string.h>

#define TAG "WifiSrv"

typedef enum {
    WifiMsgGetSettings,
    WifiMsgEnable,
    WifiMsgDisable,
    WifiMsgMarkConnected,
} WifiMsgType;

typedef struct {
    WifiMsgType type;
    FuriApiLock lock;
    union {
        WifiSettings* out_settings;
        const char* ssid;
    };
} WifiMessage;

struct Wifi {
    FuriMessageQueue* mq;
    WifiSettings settings;
    /* Spiegel von settings.enabled für lock-freies wifi_is_enabled() (das
     * Lock-Menü fragt das beim Label-Bauen ab, ohne den Service-Roundtrip). */
    volatile bool enabled_flag;

    /* Statusbar-Icon (WiFi verbunden), analog zum BT-Icon. Ein Timer pollt
     * wlan_hal_is_connected() und blendet den ViewPort ein/aus. */
    Gui* gui;
    ViewPort* statusbar_vp;
    FuriTimer* icon_timer;
    bool icon_enabled;   // Spiegel: WiFi global an (ViewPort sichtbar)
    bool icon_connected; // Spiegel: STA verbunden (Glyph voll vs. idle)
};

/* ---- Statusbar-Icon ---- */

/* WiFi-Icon, direkt mit Canvas-Dots gezeichnet (kein WiFi-Asset im Standard-Set).
 * Zwei Zustände analog zum BT-Icon (Idle/Connected):
 *   full = voller „Fächer" (3 Bögen + Punkt) → verbunden
 *   idle = nur innerer Bogen + Punkt         → an, aber (noch) nicht verbunden */
typedef struct {
    uint8_t x, y;
} WifiIconPx;

static const WifiIconPx wifi_icon_full[] = {
    {2, 0}, {3, 0}, {4, 0}, {5, 0}, // äußerer Bogen (oben)
    {1, 1}, {6, 1},                 // äußerer Bogen (Enden)
    {2, 2}, {3, 2}, {4, 2}, {5, 2}, // mittlerer Bogen
    {3, 4}, {4, 4},                 // innerer Bogen
    {3, 6}, {4, 6},                 // Punkt
};

static const WifiIconPx wifi_icon_idle[] = {
    {3, 4}, {4, 4}, // innerer Bogen
    {3, 6}, {4, 6}, // Punkt
};

static void wifi_draw_statusbar_callback(Canvas* canvas, void* context) {
    UNUSED(context);
    const WifiIconPx* px;
    size_t n;
    if(wlan_hal_is_connected()) {
        px = wifi_icon_full;
        n = sizeof(wifi_icon_full) / sizeof(wifi_icon_full[0]);
    } else {
        px = wifi_icon_idle;
        n = sizeof(wifi_icon_idle) / sizeof(wifi_icon_idle[0]);
    }
    for(size_t i = 0; i < n; i++) {
        canvas_draw_dot(canvas, px[i].x, px[i].y);
    }
}

static void wifi_icon_timer_cb(void* context) {
    Wifi* wifi = context;
    /* Sichtbar solange WiFi global an ist; Glyph wechselt mit dem Verbindungs-
     * zustand. Nur bei echten Änderungen den ViewPort anfassen/neu zeichnen. */
    bool enabled = wifi->enabled_flag;
    bool connected = wlan_hal_is_connected();
    if(enabled != wifi->icon_enabled) {
        wifi->icon_enabled = enabled;
        view_port_enabled_set(wifi->statusbar_vp, enabled);
    }
    if(connected != wifi->icon_connected) {
        wifi->icon_connected = connected;
        if(wifi->icon_enabled) view_port_update(wifi->statusbar_vp);
    }
}

/* ---- interne Helfer (laufen alle im Service-Task) ---- */

/* Schaltet den BLE-Stack ab und persistiert BtSettings.enabled=false. Damit
 * ist das Radio frei (~100KB) und das Lock-Menü-Label trackt die Realität.
 * bt_set_settings alleine stoppt nur Advertising — der echte RAM wird erst von
 * bt_stop_stack frei (idempotent). */
static void wifi_shutdown_bt(void) {
    Bt* bt = furi_record_open(RECORD_BT);
    BtSettings bs;
    bt_get_settings(bt, &bs);
    if(bs.enabled) {
        bs.enabled = false;
        bt_set_settings(bt, &bs);
    }
    bt_stop_stack(bt);
    furi_record_close(RECORD_BT);
}

static void wifi_try_reconnect(Wifi* wifi) {
    if(!wifi->settings.last_ssid[0]) return;
    char pw[65] = {0};
    bool has = wlan_password_read(wifi->settings.last_ssid, pw, sizeof(pw));
    FURI_LOG_I(TAG, "Auto-reconnect to '%s' (saved_pw=%d)", wifi->settings.last_ssid, has);
    wlan_hal_connect(wifi->settings.last_ssid, has ? pw : NULL, NULL, 0);
}

static void wifi_do_enable(Wifi* wifi) {
    wifi_shutdown_bt();
    if(!wlan_hal_start()) {
        FURI_LOG_E(TAG, "wlan_hal_start failed");
        return;
    }
    /* „sticky": beim nächsten wlan_hal_stop KEIN BT-Restore (BT bleibt bewusst
     * aus, bis der User es wieder einschaltet). */
    wlan_hal_set_bt_restore(false);
    wifi->settings.enabled = true;
    wifi->enabled_flag = true;
    wifi_settings_save(&wifi->settings);
    wifi_try_reconnect(wifi);
}

static void wifi_do_disable(Wifi* wifi) {
    wlan_hal_stop();
    wifi->settings.enabled = false;
    wifi->enabled_flag = false;
    wifi_settings_save(&wifi->settings);

    /* BT ist der Default-Funk: ohne aktives WiFi wieder einschalten (Radio ist
     * jetzt frei). bt_start_stack ist idempotent; bt_set_settings persistiert +
     * startet Advertising am nun laufenden Stack. */
    Bt* bt = furi_record_open(RECORD_BT);
    bt_start_stack(bt);
    BtSettings bs;
    bt_get_settings(bt, &bs);
    if(!bs.enabled) {
        bs.enabled = true;
        bt_set_settings(bt, &bs);
    }
    furi_record_close(RECORD_BT);
}

static void wifi_do_mark_connected(Wifi* wifi, const char* ssid) {
    if(ssid && ssid[0]) {
        strncpy(wifi->settings.last_ssid, ssid, sizeof(wifi->settings.last_ssid) - 1);
        wifi->settings.last_ssid[sizeof(wifi->settings.last_ssid) - 1] = '\0';
    }
    /* Die Verbindung kam über den App-Connect-Flow (wlan_hal_start lief bereits,
     * hat BT ggf. nur transient abgeschaltet). Jetzt sticky machen: */
    wifi_shutdown_bt();
    wlan_hal_set_bt_restore(false);
    wifi->settings.enabled = true;
    wifi->enabled_flag = true;
    wifi_settings_save(&wifi->settings);
}

/* ---- öffentliche API ---- */

bool wifi_is_enabled(Wifi* wifi) {
    furi_assert(wifi);
    return wifi->enabled_flag;
}

bool wifi_is_connected(Wifi* wifi) {
    UNUSED(wifi);
    return wlan_hal_is_connected();
}

void wifi_get_settings(Wifi* wifi, WifiSettings* out) {
    furi_assert(wifi);
    furi_assert(out);
    WifiMessage msg = {
        .type = WifiMsgGetSettings,
        .lock = api_lock_alloc_locked(),
        .out_settings = out,
    };
    furi_check(furi_message_queue_put(wifi->mq, &msg, FuriWaitForever) == FuriStatusOk);
    api_lock_wait_unlock_and_free(msg.lock);
}

void wifi_enable(Wifi* wifi) {
    furi_assert(wifi);
    WifiMessage msg = {.type = WifiMsgEnable, .lock = api_lock_alloc_locked()};
    furi_check(furi_message_queue_put(wifi->mq, &msg, FuriWaitForever) == FuriStatusOk);
    api_lock_wait_unlock_and_free(msg.lock);
}

void wifi_disable(Wifi* wifi) {
    furi_assert(wifi);
    WifiMessage msg = {.type = WifiMsgDisable, .lock = api_lock_alloc_locked()};
    furi_check(furi_message_queue_put(wifi->mq, &msg, FuriWaitForever) == FuriStatusOk);
    api_lock_wait_unlock_and_free(msg.lock);
}

void wifi_mark_connected(Wifi* wifi, const char* ssid) {
    furi_assert(wifi);
    WifiMessage msg = {
        .type = WifiMsgMarkConnected,
        .lock = api_lock_alloc_locked(),
        .ssid = ssid,
    };
    furi_check(furi_message_queue_put(wifi->mq, &msg, FuriWaitForever) == FuriStatusOk);
    api_lock_wait_unlock_and_free(msg.lock);
}

/* ---- Service main ---- */

int32_t wifi_srv(void* p) {
    UNUSED(p);
    Wifi* wifi = calloc(1, sizeof(Wifi));
    wifi->mq = furi_message_queue_alloc(8, sizeof(WifiMessage));

    /* Record + Icon FRÜH anlegen (mit sicheren Defaults), damit Lock-Menü/Apps
     * nicht auf den SD-Wait unten blocken. */
    memset(&wifi->settings, 0, sizeof(wifi->settings));
    wifi->enabled_flag = false;
    furi_record_create(RECORD_WIFI, wifi);

    /* Statusbar-Icon + Poll-Timer registrieren. */
    wifi->gui = furi_record_open(RECORD_GUI);
    wifi->statusbar_vp = view_port_alloc();
    view_port_set_width(wifi->statusbar_vp, 8);
    view_port_draw_callback_set(wifi->statusbar_vp, wifi_draw_statusbar_callback, wifi);
    view_port_enabled_set(wifi->statusbar_vp, false);
    gui_add_view_port(wifi->gui, wifi->statusbar_vp, GuiLayerStatusBarLeft);
    wifi->icon_connected = false;
    wifi->icon_timer = furi_timer_alloc(wifi_icon_timer_cb, FuriTimerTypePeriodic, wifi);
    furi_timer_start(wifi->icon_timer, furi_ms_to_ticks(1000));

    /* Settings-Load bis nach dem SD-Mount aufschieben: dieser Service startet vor
     * StorageSrv, und der Runtime-Save landet in der SD-Datei (/ext → SD). Läden
     * wir vor dem Mount, bekämen wir nur die stale NVS-Kopie → WiFi bliebe fälsch-
     * lich „disabled" und würde nicht reconnecten. Bounded warten (~5s), dann
     * laden (SD-Datei falls gemountet, sonst NVS-Fallback). */
    Storage* storage = furi_record_open(RECORD_STORAGE);
    for(int i = 0; i < 50 && storage_sd_status(storage) != FSE_OK; i++) {
        furi_delay_ms(100);
    }
    furi_record_close(RECORD_STORAGE);

    wifi_settings_load(&wifi->settings);
    wifi->enabled_flag = wifi->settings.enabled;
    FURI_LOG_I(TAG, "WifiSrv ready (enabled=%d, last_ssid='%s')",
        wifi->settings.enabled, wifi->settings.last_ssid);

    /* Boot-Bringup: WiFi war beim letzten Mal global an → Radio hoch + Reconnect.
     * (BtSettings.enabled ist dann false, bt_srv hat den BLE-Stack gar nicht
     * gestartet, also ist das RAM frei.) */
    if(wifi->settings.enabled) {
        wifi_do_enable(wifi);
    }

    WifiMessage msg;
    while(1) {
        furi_check(furi_message_queue_get(wifi->mq, &msg, FuriWaitForever) == FuriStatusOk);
        switch(msg.type) {
        case WifiMsgGetSettings:
            *msg.out_settings = wifi->settings;
            break;
        case WifiMsgEnable:
            wifi_do_enable(wifi);
            break;
        case WifiMsgDisable:
            wifi_do_disable(wifi);
            break;
        case WifiMsgMarkConnected:
            wifi_do_mark_connected(wifi, msg.ssid);
            break;
        }
        if(msg.lock) api_lock_unlock(msg.lock);
    }

    return 0;
}
