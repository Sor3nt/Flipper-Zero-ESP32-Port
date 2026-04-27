#include "../nrf24_app.h"
#include "../nrf24_hw.h"

#include <furi.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <string.h>

#define TAG "Nrf24WifiJam"

/* WiFi 2.4 GHz channel center frequency in MHz */
static uint16_t wifi_channel_center_mhz(uint8_t wifi_ch) {
    if(wifi_ch == 14) return 2484;
    if(wifi_ch < 1) wifi_ch = 1;
    if(wifi_ch > 13) wifi_ch = 13;
    return 2412 + (wifi_ch - 1) * 5;
}

typedef struct {
    Nrf24App* app;
    FuriThread* worker;
    FuriMutex* lock;
    volatile bool stop;
    volatile bool desired_running;
    bool active;
    uint8_t nrf_ch_min;
    uint8_t nrf_ch_max;
} WifiJamCtx;

static WifiJamCtx* g_ctx = NULL;

static int32_t wifi_jam_worker(void* context) {
    WifiJamCtx* ctx = context;
    Nrf24App* app = ctx->app;

    nrf24_hw_init();

    nrf24_hw_acquire();
    bool ok = nrf24_hw_probe();
    nrf24_hw_release();

    with_view_model(
        app->wifi_jam_view, Nrf24WifiJamModel * model, { model->hardware_ok = ok; }, true);

    if(!ok) {
        FURI_LOG_W(TAG, "NRF24 probe failed");
        nrf24_hw_deinit();
        return 0;
    }

    uint8_t cur_ch = ctx->nrf_ch_min;
    uint32_t hops = 0;

    while(!ctx->stop) {
        bool want = ctx->desired_running;

        if(want && !ctx->active) {
            nrf24_hw_acquire();
            nrf24_hw_jammer_start(ctx->nrf_ch_min);
            nrf24_hw_release();
            ctx->active = true;
            cur_ch = ctx->nrf_ch_min;
        } else if(!want && ctx->active) {
            nrf24_hw_acquire();
            nrf24_hw_jammer_stop();
            nrf24_hw_release();
            ctx->active = false;
        }

        if(ctx->active) {
            /* Sweep across the WiFi channel bandwidth in 50 ms batches */
            nrf24_hw_acquire();
            uint32_t batch_end = furi_get_tick() + pdMS_TO_TICKS(50);
            while(!ctx->stop && ctx->desired_running && furi_get_tick() < batch_end) {
                cur_ch++;
                if(cur_ch > ctx->nrf_ch_max) cur_ch = ctx->nrf_ch_min;
                nrf24_hw_jammer_set_channel(cur_ch);
                hops++;
                esp_rom_delay_us(200);
            }
            nrf24_hw_release();

            with_view_model(
                app->wifi_jam_view,
                Nrf24WifiJamModel * model,
                {
                    model->current_nrf_ch = cur_ch;
                    model->hop_count = hops;
                },
                true);
        }

        furi_delay_ms(10);
    }

    if(ctx->active) {
        nrf24_hw_acquire();
        nrf24_hw_jammer_stop();
        nrf24_hw_release();
        ctx->active = false;
    }

    nrf24_hw_deinit();
    return 0;
}

void nrf24_app_scene_wifi_jam_on_enter(void* context) {
    Nrf24App* app = context;

    /* Convert WiFi channel -> NRF24 sweep range covering the 20 MHz band */
    uint16_t center = wifi_channel_center_mhz(app->selected_wifi_channel);
    int min_mhz = (int)center - 10;
    int max_mhz = (int)center + 10;
    if(min_mhz < 2400) min_mhz = 2400;
    if(max_mhz > 2524) max_mhz = 2524;
    uint8_t nrf_min = (uint8_t)(min_mhz - 2400);
    uint8_t nrf_max = (uint8_t)(max_mhz - 2400);

    WifiJamCtx* ctx = malloc(sizeof(WifiJamCtx));
    ctx->app = app;
    ctx->stop = false;
    ctx->desired_running = false;
    ctx->active = false;
    ctx->nrf_ch_min = nrf_min;
    ctx->nrf_ch_max = nrf_max;
    g_ctx = ctx;

    with_view_model(
        app->wifi_jam_view,
        Nrf24WifiJamModel * model,
        {
            strncpy(model->ssid, app->selected_wifi_ssid, sizeof(model->ssid) - 1);
            model->ssid[sizeof(model->ssid) - 1] = '\0';
            model->wifi_channel = app->selected_wifi_channel;
            model->nrf_ch_min = nrf_min;
            model->nrf_ch_max = nrf_max;
            model->current_nrf_ch = nrf_min;
            model->hop_count = 0;
            model->running = false;
            model->hardware_ok = true;
        },
        true);

    view_dispatcher_switch_to_view(app->view_dispatcher, Nrf24ViewWifiJam);

    ctx->worker = furi_thread_alloc_ex("Nrf24WifiJam", 4096, wifi_jam_worker, ctx);
    furi_thread_start(ctx->worker);
}

bool nrf24_app_scene_wifi_jam_on_event(void* context, SceneManagerEvent event) {
    Nrf24App* app = context;

    if(event.type == SceneManagerEventTypeBack) {
        /* Skip the wifi-scan scene on the way out -- once the user has
         * picked an AP, "back" should land in the main menu. */
        scene_manager_search_and_switch_to_previous_scene(
            app->scene_manager, Nrf24AppSceneMenu);
        return true;
    }

    if(event.type != SceneManagerEventTypeCustom || !g_ctx) return false;

    if(event.event == Nrf24WifiJamEventToggle) {
        bool new_run = !g_ctx->desired_running;
        g_ctx->desired_running = new_run;
        with_view_model(
            app->wifi_jam_view, Nrf24WifiJamModel * model, { model->running = new_run; }, true);
        return true;
    }

    return false;
}

void nrf24_app_scene_wifi_jam_on_exit(void* context) {
    UNUSED(context);
    if(!g_ctx) return;

    g_ctx->stop = true;
    furi_thread_join(g_ctx->worker);
    furi_thread_free(g_ctx->worker);
    free(g_ctx);
    g_ctx = NULL;
}
