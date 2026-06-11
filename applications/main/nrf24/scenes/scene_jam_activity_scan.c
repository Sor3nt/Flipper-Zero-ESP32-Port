#include "../nrf24_app.h"
#include "../nrf24_hw.h"
#include "../helpers/nrf24_channel_source.h"

#include <furi.h>
#include <freertos/FreeRTOS.h>
#include <string.h>

#define TAG "Nrf24ActScan"

#define ACT_SCAN_MS     10000
#define ACT_SCAN_DELAY  5 /* ms between sweeps */
#define ACT_CH_COUNT    (NRF24_ACTIVITY_CH_MAX - NRF24_ACTIVITY_CH_MIN + 1)
#define ACT_EVENT_DONE  1

typedef struct {
    Nrf24App* app;
    FuriThread* worker;
    volatile bool stop;
    uint16_t hits[ACT_CH_COUNT];
} ActScanCtx;

static ActScanCtx* g_ctx = NULL;

/* Greedy top-N by hit count; channels with zero hits are excluded. */
static uint8_t pick_top_targets(const uint16_t* hits, uint8_t* out_ch, uint16_t* out_hits) {
    bool used[ACT_CH_COUNT] = {0};
    uint8_t n = 0;
    for(uint8_t k = 0; k < NRF24_ACTIVITY_TARGETS; k++) {
        int best_ch = -1;
        uint16_t best_h = 0;
        for(uint8_t ch = 0; ch < ACT_CH_COUNT; ch++) {
            if(used[ch]) continue;
            if(hits[ch] > best_h) {
                best_h = hits[ch];
                best_ch = ch;
            }
        }
        if(best_ch < 0 || best_h == 0) break;
        used[best_ch] = true;
        out_ch[n] = (uint8_t)(NRF24_ACTIVITY_CH_MIN + best_ch);
        out_hits[n] = best_h;
        n++;
    }
    return n;
}

static int32_t act_scan_worker(void* context) {
    ActScanCtx* ctx = context;
    Nrf24App* app = ctx->app;

    nrf24_hw_init();

    nrf24_hw_acquire();
    bool ok = nrf24_hw_probe();
    nrf24_hw_release();

    with_view_model(app->scan_view, Nrf24ScanModel * model, { model->hardware_ok = ok; }, true);

    if(!ok) {
        FURI_LOG_W(TAG, "NRF24 probe failed");
        nrf24_hw_deinit();
        return 0;
    }

    memset(ctx->hits, 0, sizeof(ctx->hits));
    uint32_t scan_start = furi_get_tick();
    uint32_t scan_end = scan_start + pdMS_TO_TICKS(ACT_SCAN_MS);
    uint32_t sweeps = 0;

    while(!ctx->stop && furi_get_tick() < scan_end) {
        nrf24_hw_acquire();
        for(uint8_t ch = 0; ch < ACT_CH_COUNT && !ctx->stop; ch++) {
            uint8_t rpd = nrf24_hw_listen_rpd((uint8_t)(NRF24_ACTIVITY_CH_MIN + ch));
            if(rpd) ctx->hits[ch]++;
        }
        nrf24_hw_release();
        sweeps++;

        uint32_t elapsed = furi_get_tick() - scan_start;
        uint8_t progress = (uint8_t)((elapsed * 100u) / pdMS_TO_TICKS(ACT_SCAN_MS));
        if(progress > 100) progress = 100;

        with_view_model(
            app->scan_view,
            Nrf24ScanModel * model,
            {
                model->progress = progress;
                model->sweeps = sweeps;
            },
            true);

        furi_delay_ms(ACT_SCAN_DELAY);
    }

    nrf24_hw_acquire();
    nrf24_hw_power_down();
    nrf24_hw_release();
    nrf24_hw_deinit();

    if(ctx->stop) return 0; /* aborted — leave cache untouched */

    /* Commit results to the shared jam state. */
    app->jam.activity_count =
        pick_top_targets(ctx->hits, app->jam.activity_ch, app->jam.activity_hits);
    app->jam.activity_wide = (app->jam.activity_count == 0); /* fallback to Wide */
    app->jam.activity_scanned = true;

    view_dispatcher_send_custom_event(app->view_dispatcher, ACT_EVENT_DONE);
    return 0;
}

void nrf24_app_scene_jam_activity_scan_on_enter(void* context) {
    Nrf24App* app = context;

    ActScanCtx* ctx = malloc(sizeof(ActScanCtx));
    if(!ctx) return;
    ctx->app = app;
    ctx->stop = false;
    g_ctx = ctx;

    with_view_model(
        app->scan_view,
        Nrf24ScanModel * model,
        {
            model->progress = 0;
            model->sweeps = 0;
            model->hardware_ok = true;
        },
        true);

    view_dispatcher_switch_to_view(app->view_dispatcher, Nrf24ViewScan);

    ctx->worker = furi_thread_alloc_ex("Nrf24ActScan", 4096, act_scan_worker, ctx);
    furi_thread_start(ctx->worker);
}

bool nrf24_app_scene_jam_activity_scan_on_event(void* context, SceneManagerEvent event) {
    Nrf24App* app = context;
    if(event.type == SceneManagerEventTypeCustom && event.event == ACT_EVENT_DONE) {
        /* Scan finished — return to the jam engine (rebuilds from Top-N). */
        scene_manager_previous_scene(app->scene_manager);
        return true;
    }
    return false;
}

void nrf24_app_scene_jam_activity_scan_on_exit(void* context) {
    UNUSED(context);
    if(!g_ctx) return;

    g_ctx->stop = true;
    furi_thread_join(g_ctx->worker);
    furi_thread_free(g_ctx->worker);
    free(g_ctx);
    g_ctx = NULL;
}
