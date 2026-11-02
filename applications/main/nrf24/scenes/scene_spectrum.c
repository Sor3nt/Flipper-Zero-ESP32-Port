#include "../nrf24_app.h"
#include "../nrf24_hw.h"

#include <furi.h>
#include <esp_rom_sys.h>

#define TAG "Nrf24Spectrum"
#define NRF24_SPECTRUM_DWELL_US NRF24_RPD_SCAN_DWELL_US
#define NRF24_SPECTRUM_DIAGNOSTIC_SWEEPS 100
#define NRF24_SPECTRUM_UPDATE_CHANNELS 16

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
    if(ok) ok = nrf24_hw_rpd_setup();
    nrf24_hw_release();

    with_view_model(
        app->spectrum_view,
        Nrf24SpectrumModel * model,
        {
            model->hardware_ok = ok;
            model->running = ok;
            model->sample_errors = 0;
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
    uint16_t diagnostic_hits = 0;
    uint16_t diagnostic_errors = 0;
    uint32_t diagnostic_sweeps = 0;
    uint32_t sample_errors = 0;

    while(!ctx->stop) {
        uint8_t sweep_hits = 0;
        uint8_t sweep_errors = 0;
        uint8_t last_hit_channel = 0;
        for(uint8_t ch = 0; ch < NRF24_SPECTRUM_CHANNELS && !ctx->stop; ch++) {
            nrf24_hw_acquire();
            bool sample_ok = nrf24_hw_listen_rpd_begin(ch);
            nrf24_hw_release();

                /* CE holds the radio in RX while the shared SPI bus is available to
                 * the LCD. end() samples RPD in RX before lowering CE. */
            uint8_t rpd = 0;
            if(sample_ok) {
                esp_rom_delay_us(NRF24_SPECTRUM_DWELL_US);
                nrf24_hw_acquire();
                sample_ok = nrf24_hw_listen_rpd_end(&rpd);
                nrf24_hw_release();
            }

            if(sample_ok) {
                if(rpd) {
                    sweep_hits++;
                    last_hit_channel = ch;
                }
                local_levels[ch] =
                    (uint8_t)((local_levels[ch] * 3 + rpd * 125) / 4);
            } else {
                sweep_errors++;
                sample_errors++;
            }

            if((ch + 1) % NRF24_SPECTRUM_UPDATE_CHANNELS == 0 ||
               ch + 1 == NRF24_SPECTRUM_CHANNELS) {
                with_view_model(
                    app->spectrum_view,
                    Nrf24SpectrumModel * model,
                    {
                        memcpy(model->levels, local_levels, NRF24_SPECTRUM_CHANNELS);
                        model->sample_errors = sample_errors;
                        if(ch + 1 == NRF24_SPECTRUM_CHANNELS) model->sweep_count++;
                    },
                    true);
            }
        }

        if(diagnostic_sweeps == 0) {
            FURI_LOG_I(
                TAG,
                "RPD first sweep: hits=%u valid=%u errors=%u last_hit_ch=%u",
                (unsigned)sweep_hits,
                (unsigned)(NRF24_SPECTRUM_CHANNELS - sweep_errors),
                (unsigned)sweep_errors,
                (unsigned)last_hit_channel);
        }
        diagnostic_hits += sweep_hits;
        diagnostic_errors += sweep_errors;
        if(++diagnostic_sweeps == NRF24_SPECTRUM_DIAGNOSTIC_SWEEPS) {
            FURI_LOG_I(
                TAG,
                "RPD diagnostic: hits=%u valid=%u errors=%u last_sweep_hits=%u last_hit_ch=%u",
                (unsigned)diagnostic_hits,
                (unsigned)(NRF24_SPECTRUM_DIAGNOSTIC_SWEEPS * NRF24_SPECTRUM_CHANNELS -
                           diagnostic_errors),
                (unsigned)diagnostic_errors,
                (unsigned)sweep_hits,
                (unsigned)last_hit_channel);
            diagnostic_sweeps = 0;
            diagnostic_hits = 0;
            diagnostic_errors = 0;
        }

        furi_delay_ms(1);
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
    if(!ctx) {
        FURI_LOG_E(TAG, "No memory for spectrum context");
        return;
    }
    ctx->app = app;
    ctx->stop = false;
    g_ctx = ctx;

    with_view_model(
        app->spectrum_view,
        Nrf24SpectrumModel * model,
        {
            model->hardware_ok = true;
            model->running = false;
            model->sample_errors = 0;
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
