#include "../nrf24_app.h"
#include "../nrf24_hw.h"

#include <furi.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <string.h>

#define TAG "Nrf24SmartJam"

#define SMART_JAM_CH_MIN     0
#define SMART_JAM_CH_MAX     79
#define SMART_JAM_CH_COUNT   (SMART_JAM_CH_MAX - SMART_JAM_CH_MIN + 1)
#define SMART_JAM_SCAN_MS    10000
#define SMART_JAM_SCAN_DELAY 5 /* ms between sweeps */

typedef struct {
    Nrf24App* app;
    FuriThread* worker;
    volatile bool stop;
    volatile bool desired_running;
    volatile bool wide_mode;
    volatile bool mode_dirty;
    bool active;
    uint16_t hits[SMART_JAM_CH_COUNT];
    uint8_t targets[NRF24_SMART_JAM_TARGETS];
    uint16_t target_hits[NRF24_SMART_JAM_TARGETS];
    uint8_t target_count;
} Nrf24SmartJamCtx;

static Nrf24SmartJamCtx* g_ctx = NULL;

/* Pick top-N channels by hit count. Channels with zero hits are excluded. */
static uint8_t pick_top_targets(
    const uint16_t* hits,
    uint8_t* out_targets,
    uint16_t* out_hits,
    uint8_t max_n) {
    bool used[SMART_JAM_CH_COUNT] = {0};
    uint8_t n = 0;

    for(uint8_t k = 0; k < max_n; k++) {
        int best_ch = -1;
        uint16_t best_h = 0;
        for(uint8_t ch = 0; ch < SMART_JAM_CH_COUNT; ch++) {
            if(used[ch]) continue;
            if(hits[ch] > best_h) {
                best_h = hits[ch];
                best_ch = ch;
            }
        }
        if(best_ch < 0 || best_h == 0) break;
        used[best_ch] = true;
        out_targets[n] = (uint8_t)(SMART_JAM_CH_MIN + best_ch);
        out_hits[n] = best_h;
        n++;
    }
    return n;
}

static int32_t smart_jam_worker(void* context) {
    Nrf24SmartJamCtx* ctx = context;
    Nrf24App* app = ctx->app;

    nrf24_hw_init();

    nrf24_hw_acquire();
    bool ok = nrf24_hw_probe();
    nrf24_hw_release();

    with_view_model(
        app->smart_jam_view, Nrf24SmartJamModel * model, { model->hardware_ok = ok; }, true);

    if(!ok) {
        FURI_LOG_W(TAG, "NRF24 probe failed");
        nrf24_hw_deinit();
        return 0;
    }

    /* ---- Phase 1: Scan ---- */
    memset(ctx->hits, 0, sizeof(ctx->hits));
    uint32_t scan_start = furi_get_tick();
    uint32_t scan_end = scan_start + pdMS_TO_TICKS(SMART_JAM_SCAN_MS);
    uint16_t sweep_count = 0;

    while(!ctx->stop && furi_get_tick() < scan_end) {
        nrf24_hw_acquire();
        for(uint8_t ch = 0; ch < SMART_JAM_CH_COUNT && !ctx->stop; ch++) {
            uint8_t rpd = nrf24_hw_listen_rpd((uint8_t)(SMART_JAM_CH_MIN + ch));
            if(rpd) ctx->hits[ch]++;
        }
        nrf24_hw_release();
        sweep_count++;

        uint32_t elapsed = furi_get_tick() - scan_start;
        uint8_t progress = (uint8_t)((elapsed * 100u) / pdMS_TO_TICKS(SMART_JAM_SCAN_MS));
        if(progress > 100) progress = 100;

        with_view_model(
            app->smart_jam_view,
            Nrf24SmartJamModel * model,
            {
                model->scan_progress = progress;
                model->sweep_count = sweep_count;
            },
            true);

        furi_delay_ms(SMART_JAM_SCAN_DELAY);
    }

    if(ctx->stop) {
        nrf24_hw_acquire();
        nrf24_hw_power_down();
        nrf24_hw_release();
        nrf24_hw_deinit();
        return 0;
    }

    /* ---- Compute targets ---- */
    ctx->target_count = pick_top_targets(
        ctx->hits, ctx->targets, ctx->target_hits, NRF24_SMART_JAM_TARGETS);

    /* If no active channels were detected, default to Wide mode so the user
     * can still jam something. */
    if(ctx->target_count == 0) ctx->wide_mode = true;

    with_view_model(
        app->smart_jam_view,
        Nrf24SmartJamModel * model,
        {
            model->phase = SmartJamPhaseJam;
            model->target_count = ctx->target_count;
            memcpy(model->targets, ctx->targets, ctx->target_count);
            memcpy(
                model->target_hits, ctx->target_hits, ctx->target_count * sizeof(uint16_t));
            model->current_target_idx = 0;
            model->current_channel = 0;
            model->hop_count = 0;
            model->running = false;
            model->wide_mode = ctx->wide_mode;
        },
        true);

    /* ---- Phase 2: Jam loop ---- */
    uint8_t cur_idx = 0;
    uint8_t cur_channel = 0;
    uint32_t hops = 0;

    while(!ctx->stop) {
        bool want = ctx->desired_running;
        bool wide = ctx->wide_mode;

        if(ctx->mode_dirty) {
            ctx->mode_dirty = false;
            cur_idx = 0;
            cur_channel = wide ? SMART_JAM_CH_MIN : (ctx->target_count > 0 ? ctx->targets[0] : 0);
            if(ctx->active) {
                nrf24_hw_acquire();
                nrf24_hw_jammer_set_channel(cur_channel);
                nrf24_hw_release();
            }
        }

        if(want && !ctx->active) {
            cur_idx = 0;
            cur_channel = wide ? SMART_JAM_CH_MIN : (ctx->target_count > 0 ? ctx->targets[0] : 0);
            nrf24_hw_acquire();
            nrf24_hw_jammer_start(cur_channel);
            nrf24_hw_release();
            ctx->active = true;
        } else if(!want && ctx->active) {
            nrf24_hw_acquire();
            nrf24_hw_jammer_stop();
            nrf24_hw_release();
            ctx->active = false;
        }

        if(ctx->active) {
            /* Sweep target list (or full range in Wide mode) in 50 ms batches,
             * 200 µs per hop. */
            nrf24_hw_acquire();
            uint32_t batch_end = furi_get_tick() + pdMS_TO_TICKS(50);
            while(!ctx->stop && ctx->desired_running && !ctx->mode_dirty &&
                  furi_get_tick() < batch_end) {
                if(wide) {
                    cur_idx++;
                    if(cur_idx > SMART_JAM_CH_MAX) cur_idx = SMART_JAM_CH_MIN;
                    cur_channel = cur_idx;
                } else {
                    cur_idx++;
                    if(cur_idx >= ctx->target_count) cur_idx = 0;
                    cur_channel = ctx->targets[cur_idx];
                }
                nrf24_hw_jammer_set_channel(cur_channel);
                hops++;
                esp_rom_delay_us(200);
            }
            nrf24_hw_release();

            with_view_model(
                app->smart_jam_view,
                Nrf24SmartJamModel * model,
                {
                    model->current_target_idx = wide ? 0 : cur_idx;
                    model->current_channel = cur_channel;
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

void nrf24_app_scene_smart_jam_on_enter(void* context) {
    Nrf24App* app = context;

    Nrf24SmartJamCtx* ctx = malloc(sizeof(Nrf24SmartJamCtx));
    ctx->app = app;
    ctx->stop = false;
    ctx->desired_running = false;
    ctx->wide_mode = false;
    ctx->mode_dirty = false;
    ctx->active = false;
    ctx->target_count = 0;
    g_ctx = ctx;

    with_view_model(
        app->smart_jam_view,
        Nrf24SmartJamModel * model,
        {
            model->phase = SmartJamPhaseScan;
            model->scan_progress = 0;
            model->sweep_count = 0;
            model->target_count = 0;
            model->current_target_idx = 0;
            model->current_channel = 0;
            model->hop_count = 0;
            model->running = false;
            model->wide_mode = false;
            model->hardware_ok = true;
        },
        true);

    view_dispatcher_switch_to_view(app->view_dispatcher, Nrf24ViewSmartJam);

    ctx->worker = furi_thread_alloc_ex("Nrf24SmartJam", 4096, smart_jam_worker, ctx);
    furi_thread_start(ctx->worker);
}

bool nrf24_app_scene_smart_jam_on_event(void* context, SceneManagerEvent event) {
    Nrf24App* app = context;
    if(event.type != SceneManagerEventTypeCustom || !g_ctx) return false;

    switch(event.event) {
    case Nrf24SmartJamEventToggle: {
        /* In Smart mode without targets: nothing to jam */
        if(!g_ctx->wide_mode && g_ctx->target_count == 0) return true;
        bool new_run = !g_ctx->desired_running;
        g_ctx->desired_running = new_run;
        with_view_model(
            app->smart_jam_view,
            Nrf24SmartJamModel * model,
            {
                if(model->phase == SmartJamPhaseJam) model->running = new_run;
            },
            true);
        return true;
    }
    case Nrf24SmartJamEventToggleMode: {
        bool new_wide = !g_ctx->wide_mode;
        /* Don't switch back to Smart if there are no targets */
        if(!new_wide && g_ctx->target_count == 0) return true;
        g_ctx->wide_mode = new_wide;
        g_ctx->mode_dirty = true;
        with_view_model(
            app->smart_jam_view,
            Nrf24SmartJamModel * model,
            {
                if(model->phase == SmartJamPhaseJam) model->wide_mode = new_wide;
            },
            true);
        return true;
    }
    default:
        return false;
    }
}

void nrf24_app_scene_smart_jam_on_exit(void* context) {
    UNUSED(context);
    if(!g_ctx) return;

    g_ctx->stop = true;
    furi_thread_join(g_ctx->worker);
    furi_thread_free(g_ctx->worker);
    free(g_ctx);
    g_ctx = NULL;
}
