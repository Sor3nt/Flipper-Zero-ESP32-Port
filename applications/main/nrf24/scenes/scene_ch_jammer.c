#include "../nrf24_app.h"
#include "../nrf24_hw.h"

#include <furi.h>

#define TAG "Nrf24ChJammer"

#define CH_JAMMER_MIN 1
#define CH_JAMMER_MAX 125
#define CH_JAMMER_DEFAULT 50

typedef struct {
    Nrf24App* app;
    FuriThread* worker;
    FuriMutex* lock;
    volatile bool stop;
    volatile bool channel_dirty;
    volatile bool desired_running;
    volatile uint8_t desired_channel;
    bool active;
} Nrf24ChJammerCtx;

static Nrf24ChJammerCtx* g_ctx = NULL;

static int32_t nrf24_ch_jammer_worker(void* context) {
    Nrf24ChJammerCtx* ctx = context;
    Nrf24App* app = ctx->app;

    nrf24_hw_init();

    nrf24_hw_acquire();
    bool ok = nrf24_hw_probe();
    nrf24_hw_release();

    with_view_model(
        app->ch_jammer_view, Nrf24ChJammerModel * model, { model->hardware_ok = ok; }, true);

    if(!ok) {
        FURI_LOG_W(TAG, "NRF24 probe failed");
        nrf24_hw_deinit();
        return 0;
    }

    while(!ctx->stop) {
        bool want_run = ctx->desired_running;
        uint8_t want_ch = ctx->desired_channel;
        bool ch_dirty = ctx->channel_dirty;
        ctx->channel_dirty = false;

        if(want_run && !ctx->active) {
            nrf24_hw_acquire();
            nrf24_hw_jammer_start(want_ch);
            nrf24_hw_release();
            ctx->active = true;
        } else if(!want_run && ctx->active) {
            nrf24_hw_acquire();
            nrf24_hw_jammer_stop();
            nrf24_hw_release();
            ctx->active = false;
        } else if(want_run && ch_dirty) {
            nrf24_hw_acquire();
            nrf24_hw_jammer_set_channel(want_ch);
            nrf24_hw_release();
        }

        furi_delay_ms(20);
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

void nrf24_app_scene_ch_jammer_on_enter(void* context) {
    Nrf24App* app = context;

    Nrf24ChJammerCtx* ctx = malloc(sizeof(Nrf24ChJammerCtx));
    ctx->app = app;
    ctx->stop = false;
    ctx->desired_running = false;
    ctx->desired_channel = CH_JAMMER_DEFAULT;
    ctx->channel_dirty = false;
    ctx->active = false;
    g_ctx = ctx;

    with_view_model(
        app->ch_jammer_view,
        Nrf24ChJammerModel * model,
        {
            model->channel = CH_JAMMER_DEFAULT;
            model->min_channel = CH_JAMMER_MIN;
            model->max_channel = CH_JAMMER_MAX;
            model->running = false;
            model->hardware_ok = true;
        },
        true);

    view_dispatcher_switch_to_view(app->view_dispatcher, Nrf24ViewChJammer);

    ctx->worker = furi_thread_alloc_ex("Nrf24ChJam", 4096, nrf24_ch_jammer_worker, ctx);
    furi_thread_start(ctx->worker);
}

bool nrf24_app_scene_ch_jammer_on_event(void* context, SceneManagerEvent event) {
    Nrf24App* app = context;
    if(event.type != SceneManagerEventTypeCustom || !g_ctx) return false;

    switch(event.event) {
    case Nrf24ChJammerEventToggle: {
        bool new_running = !g_ctx->desired_running;
        g_ctx->desired_running = new_running;
        with_view_model(
            app->ch_jammer_view,
            Nrf24ChJammerModel * model,
            { model->running = new_running; },
            true);
        return true;
    }
    case Nrf24ChJammerEventChannelUp: {
        with_view_model(
            app->ch_jammer_view,
            Nrf24ChJammerModel * model,
            {
                if(model->channel < model->max_channel) model->channel++;
                g_ctx->desired_channel = model->channel;
            },
            true);
        g_ctx->channel_dirty = true;
        return true;
    }
    case Nrf24ChJammerEventChannelDown: {
        with_view_model(
            app->ch_jammer_view,
            Nrf24ChJammerModel * model,
            {
                if(model->channel > model->min_channel) model->channel--;
                g_ctx->desired_channel = model->channel;
            },
            true);
        g_ctx->channel_dirty = true;
        return true;
    }
    default:
        return false;
    }
}

void nrf24_app_scene_ch_jammer_on_exit(void* context) {
    UNUSED(context);
    if(!g_ctx) return;

    g_ctx->stop = true;
    furi_thread_join(g_ctx->worker);
    furi_thread_free(g_ctx->worker);
    free(g_ctx);
    g_ctx = NULL;
}
