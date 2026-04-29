#include "../nrf24_app.h"
#include "../nrf24_hw.h"
#include "../helpers/nrf24_mj_core.h"

#include <furi.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <string.h>

#define TAG "Nrf24MjScan"

#define MJ_SCAN_CH_MIN     2
#define MJ_SCAN_CH_MAX     84
#define MJ_SCAN_TRIES_PER_CH 6
#define MJ_SCAN_DWELL_US   500

typedef struct {
    Nrf24App* app;
    FuriThread* worker;
    volatile bool stop;
} Nrf24MjScanCtx;

static Nrf24MjScanCtx* g_ctx = NULL;

static int32_t mj_scan_worker(void* context) {
    Nrf24MjScanCtx* ctx = context;
    Nrf24App* app = ctx->app;

    nrf24_hw_init();

    nrf24_hw_acquire();
    bool ok = nrf24_hw_probe();
    nrf24_hw_release();

    with_view_model(
        app->mj_scan_view, Nrf24MjScanModel * model, { model->hardware_ok = ok; }, true);

    if(!ok) {
        FURI_LOG_W(TAG, "NRF24 probe failed");
        nrf24_hw_deinit();
        return 0;
    }

    /* Start promiscuous RX on the first channel. */
    nrf24_hw_acquire();
    nrf24_hw_rx_start_promiscuous(MJ_SCAN_CH_MIN);
    nrf24_hw_release();

    uint16_t sweep = 0;
    uint8_t prev_count = 0;

    while(!ctx->stop) {
        for(uint8_t ch = MJ_SCAN_CH_MIN; ch <= MJ_SCAN_CH_MAX && !ctx->stop; ch++) {
            nrf24_hw_acquire();
            nrf24_hw_ce(false);
            nrf24_hw_set_channel(ch);
            nrf24_hw_flush_rx();
            nrf24_hw_ce(true);

            for(int t = 0; t < MJ_SCAN_TRIES_PER_CH && !ctx->stop; t++) {
                esp_rom_delay_us(MJ_SCAN_DWELL_US);
                if(nrf24_hw_rx_data_ready()) {
                    uint8_t buf[32];
                    nrf24_hw_rx_read_payload(buf, sizeof(buf));
                    int idx = mj_fingerprint(
                        buf, sizeof(buf), ch, app->mj_targets, &app->mj_target_count);
                    if(idx >= 0) {
                        char label[24];
                        mj_format_target(&app->mj_targets[idx], label, sizeof(label));
                        with_view_model(
                            app->mj_scan_view,
                            Nrf24MjScanModel * model,
                            {
                                model->target_count = app->mj_target_count;
                                strncpy(
                                    model->last_target_label,
                                    label,
                                    sizeof(model->last_target_label) - 1);
                                model->last_target_label
                                    [sizeof(model->last_target_label) - 1] = '\0';
                            },
                            true);
                    }
                }
            }
            nrf24_hw_release();

            /* Light UI update every 4 channels. */
            if((ch & 0x03) == 0 || app->mj_target_count != prev_count) {
                prev_count = app->mj_target_count;
                with_view_model(
                    app->mj_scan_view,
                    Nrf24MjScanModel * model,
                    {
                        model->current_channel = ch;
                        model->sweep_count = sweep;
                    },
                    true);
            }
        }
        sweep++;
        furi_delay_ms(2);
    }

    nrf24_hw_acquire();
    nrf24_hw_rx_stop();
    nrf24_hw_power_down();
    nrf24_hw_release();
    nrf24_hw_deinit();
    return 0;
}

void nrf24_app_scene_mj_scan_on_enter(void* context) {
    Nrf24App* app = context;

    Nrf24MjScanCtx* ctx = malloc(sizeof(Nrf24MjScanCtx));
    ctx->app = app;
    ctx->stop = false;
    g_ctx = ctx;

    with_view_model(
        app->mj_scan_view,
        Nrf24MjScanModel * model,
        {
            model->current_channel = MJ_SCAN_CH_MIN;
            model->target_count = 0;
            model->sweep_count = 0;
            model->running = true;
            model->hardware_ok = true;
            model->last_target_label[0] = '\0';
        },
        true);

    view_dispatcher_switch_to_view(app->view_dispatcher, Nrf24ViewMjScan);

    ctx->worker = furi_thread_alloc_ex("Nrf24MjScan", 4096, mj_scan_worker, ctx);
    furi_thread_start(ctx->worker);
}

bool nrf24_app_scene_mj_scan_on_event(void* context, SceneManagerEvent event) {
    Nrf24App* app = context;
    if(event.type != SceneManagerEventTypeCustom || !g_ctx) return false;

    if(event.event == Nrf24MjScanEventStop) {
        if(app->mj_target_count > 0) {
            scene_manager_next_scene(app->scene_manager, Nrf24AppSceneMjTargetList);
        } else {
            /* No targets — signal worker to stop, flash a message, return to menu. */
            g_ctx->stop = true;
            with_view_model(
                app->mj_scan_view,
                Nrf24MjScanModel * model,
                { model->no_targets_msg = true; },
                true);
            furi_delay_ms(1500);
            scene_manager_previous_scene(app->scene_manager);
        }
        return true;
    }
    return false;
}

void nrf24_app_scene_mj_scan_on_exit(void* context) {
    UNUSED(context);
    if(!g_ctx) return;
    g_ctx->stop = true;
    furi_thread_join(g_ctx->worker);
    furi_thread_free(g_ctx->worker);
    free(g_ctx);
    g_ctx = NULL;
}
