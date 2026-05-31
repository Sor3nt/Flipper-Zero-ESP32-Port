#include "../nrf24_app.h"
#include "../nrf24_hw.h"
#include "../helpers/nrf24_jam_presets.h"

#include <furi.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <string.h>

#define TAG "Nrf24PresetJam"

typedef struct {
    Nrf24App* app;
    FuriThread* worker;
    volatile bool stop;
    volatile bool desired_running;
    volatile bool desired_flooding;
    volatile bool desired_low_rate;
    volatile uint16_t dwell_us;
    Nrf24JamPreset preset;
    bool active;
    bool active_flooding;
    bool active_low_rate;
} PresetJamCtx;

static PresetJamCtx* g_ctx = NULL;

/* Bring up the radio in the requested strategy on the given channel. */
static void preset_jam_setup(bool flooding, bool low_rate, uint8_t channel) {
    nrf24_hw_acquire();
    if(flooding) {
        nrf24_hw_flood_start(channel, low_rate);
    } else {
        nrf24_hw_jammer_start(channel);
    }
    nrf24_hw_release();
}

static void preset_jam_teardown(bool flooding) {
    nrf24_hw_acquire();
    if(flooding) {
        nrf24_hw_flood_stop();
    } else {
        nrf24_hw_jammer_stop();
    }
    nrf24_hw_release();
}

static int32_t preset_jam_worker(void* context) {
    PresetJamCtx* ctx = context;
    Nrf24App* app = ctx->app;

    nrf24_hw_init();

    nrf24_hw_acquire();
    bool ok = nrf24_hw_probe();
    nrf24_hw_release();

    with_view_model(
        app->preset_jam_view, Nrf24PresetJamModel * model, { model->hardware_ok = ok; }, true);

    if(!ok) {
        FURI_LOG_W(TAG, "NRF24 probe failed");
        nrf24_hw_deinit();
        return 0;
    }

    uint32_t hop_index = 0;
    uint32_t hops = 0;
    uint8_t cur_ch = 0;

    while(!ctx->stop) {
        bool want = ctx->desired_running;
        bool flood = ctx->desired_flooding;
        bool low_rate = ctx->desired_low_rate;

        /* (Re)configure on start, strategy switch, or flood rate change. */
        bool need_resetup = want && (!ctx->active || flood != ctx->active_flooding ||
                                     (flood && low_rate != ctx->active_low_rate));
        if(need_resetup) {
            if(ctx->active) preset_jam_teardown(ctx->active_flooding);
            hop_index = 0;
            hops = 0;
            cur_ch = nrf24_jam_preset_next_channel(ctx->preset, &hop_index);
            preset_jam_setup(flood, low_rate, cur_ch);
            ctx->active = true;
            ctx->active_flooding = flood;
            ctx->active_low_rate = low_rate;
        } else if(!want && ctx->active) {
            preset_jam_teardown(ctx->active_flooding);
            ctx->active = false;
        }

        if(ctx->active) {
            /* Hop through the preset's channel list in ~30 ms bursts so the
             * shared SPI bus stays free for LCD / SD between batches. */
            nrf24_hw_acquire();
            uint32_t batch_end = furi_get_tick() + pdMS_TO_TICKS(30);
            uint16_t dwell = ctx->dwell_us;
            while(!ctx->stop && ctx->desired_running &&
                  ctx->desired_flooding == ctx->active_flooding &&
                  ctx->desired_low_rate == ctx->active_low_rate &&
                  furi_get_tick() < batch_end) {
                cur_ch = nrf24_jam_preset_next_channel(ctx->preset, &hop_index);
                if(ctx->active_flooding) {
                    /* Burst long enough to fill the dwell window (each burst
                     * radiates ~500 µs of packets on this channel). */
                    uint32_t spent = 0;
                    do {
                        nrf24_hw_flood_channel(cur_ch);
                        spent += 500;
                    } while(spent < dwell);
                } else {
                    nrf24_hw_jammer_set_channel(cur_ch);
                    /* Dwell covers the ~130 µs PLL re-lock plus carrier time. */
                    esp_rom_delay_us(dwell);
                }
                hops++;
            }
            nrf24_hw_release();

            with_view_model(
                app->preset_jam_view,
                Nrf24PresetJamModel * model,
                {
                    model->channel = cur_ch;
                    model->hop_count = hops;
                    model->flooding = ctx->active_flooding;
                    model->low_rate = ctx->active_low_rate;
                },
                true);
        }

        furi_delay_ms(ctx->active ? 2 : 10);
    }

    if(ctx->active) {
        preset_jam_teardown(ctx->active_flooding);
        ctx->active = false;
    }

    nrf24_hw_deinit();
    return 0;
}

void nrf24_app_scene_preset_jam_on_enter(void* context) {
    Nrf24App* app = context;

    Nrf24JamPreset preset = (Nrf24JamPreset)app->selected_jam_preset;
    uint16_t dwell = nrf24_jam_preset_default_dwell_us(preset);

    PresetJamCtx* ctx = malloc(sizeof(PresetJamCtx));
    ctx->app = app;
    ctx->stop = false;
    ctx->desired_running = false;
    ctx->desired_flooding = false; /* default to constant carrier, like Bruce */
    ctx->desired_low_rate = false;
    ctx->dwell_us = dwell;
    ctx->preset = preset;
    ctx->active = false;
    ctx->active_flooding = false;
    ctx->active_low_rate = false;
    g_ctx = ctx;

    with_view_model(
        app->preset_jam_view,
        Nrf24PresetJamModel * model,
        {
            strncpy(
                model->preset_name,
                nrf24_jam_preset_short(preset),
                sizeof(model->preset_name) - 1);
            model->preset_name[sizeof(model->preset_name) - 1] = '\0';
            model->channel = 0;
            model->hop_count = 0;
            model->dwell_us = dwell;
            model->flooding = false;
            model->low_rate = false;
            model->running = false;
            model->hardware_ok = true;
        },
        true);

    view_dispatcher_switch_to_view(app->view_dispatcher, Nrf24ViewPresetJam);

    ctx->worker = furi_thread_alloc_ex("Nrf24PresetJam", 4096, preset_jam_worker, ctx);
    furi_thread_start(ctx->worker);
}

bool nrf24_app_scene_preset_jam_on_event(void* context, SceneManagerEvent event) {
    Nrf24App* app = context;
    if(event.type != SceneManagerEventTypeCustom || !g_ctx) return false;

    switch(event.event) {
    case Nrf24PresetJamEventToggle: {
        bool new_run = !g_ctx->desired_running;
        g_ctx->desired_running = new_run;
        with_view_model(
            app->preset_jam_view, Nrf24PresetJamModel * model, { model->running = new_run; }, true);
        return true;
    }
    case Nrf24PresetJamEventToggleStrategy: {
        bool new_flood = !g_ctx->desired_flooding;
        g_ctx->desired_flooding = new_flood;
        with_view_model(
            app->preset_jam_view,
            Nrf24PresetJamModel * model,
            { model->flooding = new_flood; },
            true);
        return true;
    }
    case Nrf24PresetJamEventToggleRate: {
        bool new_rate = !g_ctx->desired_low_rate;
        g_ctx->desired_low_rate = new_rate;
        with_view_model(
            app->preset_jam_view,
            Nrf24PresetJamModel * model,
            { model->low_rate = new_rate; },
            true);
        return true;
    }
    case Nrf24PresetJamEventDwellUp: {
        uint16_t d = g_ctx->dwell_us + NRF24_JAM_DWELL_STEP_US;
        if(d > NRF24_JAM_DWELL_MAX_US) d = NRF24_JAM_DWELL_MAX_US;
        g_ctx->dwell_us = d;
        with_view_model(
            app->preset_jam_view, Nrf24PresetJamModel * model, { model->dwell_us = d; }, true);
        return true;
    }
    case Nrf24PresetJamEventDwellDown: {
        uint16_t d = g_ctx->dwell_us;
        d = (d > NRF24_JAM_DWELL_MIN_US + NRF24_JAM_DWELL_STEP_US) ?
                (d - NRF24_JAM_DWELL_STEP_US) :
                NRF24_JAM_DWELL_MIN_US;
        g_ctx->dwell_us = d;
        with_view_model(
            app->preset_jam_view, Nrf24PresetJamModel * model, { model->dwell_us = d; }, true);
        return true;
    }
    default:
        return false;
    }
}

void nrf24_app_scene_preset_jam_on_exit(void* context) {
    UNUSED(context);
    if(!g_ctx) return;

    g_ctx->stop = true;
    furi_thread_join(g_ctx->worker);
    furi_thread_free(g_ctx->worker);
    free(g_ctx);
    g_ctx = NULL;
}
