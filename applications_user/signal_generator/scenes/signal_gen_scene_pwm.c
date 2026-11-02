#include "../signal_gen_app_i.h"

#define DEFAULT_FREQ 1000
#define DEFAULT_DUTY 50

// Called from the view's input handler, outside the model lock. The output is
// switched right here (not from a queued event) so a quick double pin change can
// never leave the first pin running.
static void
    signal_gen_pwm_callback(uint8_t channel_id, uint32_t freq, uint8_t duty, void* context) {
    SignalGenApp* app = context;
    const FuriHalPwmOutputId channel = signal_gen_pwm_ids[channel_id];

    app->pwm_freq = freq;
    app->pwm_duty = duty;

    if(app->pwm_ch != channel) {
        if(app->pwm_ch != FuriHalPwmOutputIdNone) {
            furi_hal_pwm_stop(app->pwm_ch);
        }
        app->pwm_ch = channel;
    }

    // Starting a running output retunes it; a pin that was busy gets another try.
    // The HAL leaves a pin idle when it is reserved (e.g. IR external TX) or driven
    // by someone else, so show "busy" instead of a signal that isn't there.
    furi_hal_pwm_start(channel, freq, duty);
    signal_gen_pwm_set_busy(app->pwm_view, !furi_hal_pwm_is_running(channel));
}

void signal_gen_scene_pwm_on_enter(void* context) {
    SignalGenApp* app = context;

    app->pwm_ch = FuriHalPwmOutputIdNone;

    view_dispatcher_switch_to_view(app->view_dispatcher, SignalGenViewPwm);

    signal_gen_pwm_set_callback(app->pwm_view, signal_gen_pwm_callback, app);

    signal_gen_pwm_set_params(app->pwm_view, 0, DEFAULT_FREQ, DEFAULT_DUTY);
}

bool signal_gen_scene_pwm_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void signal_gen_scene_pwm_on_exit(void* context) {
    SignalGenApp* app = context;
    variable_item_list_reset(app->var_item_list);

    furi_hal_pwm_stop(app->pwm_ch);
    app->pwm_ch = FuriHalPwmOutputIdNone;
}
