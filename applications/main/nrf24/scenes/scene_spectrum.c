#include "../nrf24_app.h"
#include "../nrf24_hw.h"

#include <furi.h>

#define TAG "Nrf24Spectrum"

typedef struct {
    Nrf24App* app;
    FuriThread* worker;
    volatile bool stop;
} Nrf24SpectrumCtx;

static Nrf24SpectrumCtx* g_ctx = NULL;

static int32_t nrf24_spectrum_worker(void* context) {
    Nrf24SpectrumCtx* ctx = context;
    Nrf24App* app = ctx->app;

    nrf24_hw_init();

    nrf24_hw_acquire();
    bool ok = nrf24_hw_probe();
    nrf24_hw_release();

    with_view_model(
        app->spectrum_view,
        Nrf24SpectrumModel * model,
        {
            model->hardware_ok = ok;
            model->running = ok;
            for(int i = 0; i < NRF24_SPECTRUM_CHANNELS; i++) model->levels[i] = 0;
            model->sweep_count = 0;
        },
        true);

    if(!ok) {
        FURI_LOG_W(TAG, "NRF24 probe failed");
        nrf24_hw_deinit();
        return 0;
    }

    uint8_t local_levels[NRF24_SPECTRUM_CHANNELS] = {0};

    while(!ctx->stop) {
        nrf24_hw_acquire();

        for(uint8_t ch = 0; ch < NRF24_SPECTRUM_CHANNELS && !ctx->stop; ch++) {
            uint8_t rpd = nrf24_hw_listen_rpd(ch);
            local_levels[ch] = (uint8_t)((local_levels[ch] * 3 + rpd * 125) / 4);
        }

        nrf24_hw_release();

        with_view_model(
            app->spectrum_view,
            Nrf24SpectrumModel * model,
            {
                memcpy(model->levels, local_levels, NRF24_SPECTRUM_CHANNELS);
                model->sweep_count++;
            },
            true);

        furi_delay_ms(20);
    }

    nrf24_hw_acquire();
    nrf24_hw_power_down();
    nrf24_hw_release();
    nrf24_hw_deinit();

    return 0;
}

void nrf24_app_scene_spectrum_on_enter(void* context) {
    Nrf24App* app = context;

    Nrf24SpectrumCtx* ctx = malloc(sizeof(Nrf24SpectrumCtx));
    ctx->app = app;
    ctx->stop = false;
    g_ctx = ctx;

    with_view_model(
        app->spectrum_view,
        Nrf24SpectrumModel * model,
        {
            model->hardware_ok = true;
            model->running = false;
            model->sweep_count = 0;
            for(int i = 0; i < NRF24_SPECTRUM_CHANNELS; i++) model->levels[i] = 0;
        },
        true);

    view_dispatcher_switch_to_view(app->view_dispatcher, Nrf24ViewSpectrum);

    ctx->worker = furi_thread_alloc_ex("Nrf24Spectrum", 4096, nrf24_spectrum_worker, ctx);
    furi_thread_start(ctx->worker);
}

bool nrf24_app_scene_spectrum_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void nrf24_app_scene_spectrum_on_exit(void* context) {
    UNUSED(context);
    if(!g_ctx) return;

    g_ctx->stop = true;
    furi_thread_join(g_ctx->worker);
    furi_thread_free(g_ctx->worker);
    free(g_ctx);
    g_ctx = NULL;
}
