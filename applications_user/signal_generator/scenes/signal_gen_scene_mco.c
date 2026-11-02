#include "../signal_gen_app_i.h"

#include <string.h>

// Clock Generator. Upstream routes an STM32 oscillator to the MCO pin; the ESP32
// has no such output, so this is a 50% square wave from the LEDC timer (APB 80 MHz
// divided down) on a header pin. Every preset below is an exact LEDC divider except
// the 32.768 kHz one, which lands within ~0.06% of it.

typedef enum {
    LineIndexPin,
    LineIndexFrequency,
    LineIndexDivision,
} LineIndex;

#define CLOCK_DUTY 50

static const uint32_t clock_base_freqs[] = {
    32768UL,
    100000UL,
    200000UL,
    400000UL,
    800000UL,
    1000000UL,
    2000000UL,
    4000000UL,
    8000000UL,
    10000000UL,
    16000000UL,
    20000000UL,
    40000000UL,
};

static const char* const clock_divisor_names[] = {
    "1",
    "2",
    "4",
    "8",
    "16",
};

static const uint8_t clock_divisors[] = {1, 2, 4, 8, 16};

static uint32_t clock_output_freq(SignalGenApp* app) {
    return clock_base_freqs[app->clock_base] / clock_divisors[app->clock_div];
}

// "2.048kHz", "1.25MHz", "400kHz": up to 3 decimals, trailing zeros dropped
static void clock_format_freq(char* buf, size_t size, uint32_t hz) {
    uint32_t unit_hz = 1;
    const char* unit = "Hz";
    if(hz >= 1000000UL) {
        unit_hz = 1000000UL;
        unit = "MHz";
    } else if(hz >= 1000UL) {
        unit_hz = 1000UL;
        unit = "kHz";
    }

    const uint32_t whole = hz / unit_hz;
    const uint32_t frac = unit_hz > 1 ? (hz % unit_hz) / (unit_hz / 1000UL) : 0;
    if(frac == 0) {
        snprintf(buf, size, "%lu%s", whole, unit);
        return;
    }

    char digits[4];
    snprintf(digits, sizeof(digits), "%03lu", frac);
    for(size_t i = strlen(digits); i > 0 && digits[i - 1] == '0'; i--) {
        digits[i - 1] = '\0';
    }
    snprintf(buf, size, "%lu.%s%s", whole, digits, unit);
}

static void clock_update_freq_text(SignalGenApp* app) {
    char text[16];
    clock_format_freq(text, sizeof(text), clock_output_freq(app));
    variable_item_set_current_value_text(app->clock_freq_item, text);
}

// Starting a running output retunes it; a pin that was busy gets another try.
// The HAL leaves a pin idle when it is reserved (e.g. IR external TX) or driven by
// someone else, so the pin line then reads "busy" instead of a signal that isn't there.
static void clock_start_output(SignalGenApp* app) {
    const FuriHalPwmOutputId channel = signal_gen_pwm_ids[app->clock_pin];
    furi_hal_pwm_start(channel, clock_output_freq(app), CLOCK_DUTY);

    variable_item_set_current_value_text(
        app->clock_pin_item,
        furi_hal_pwm_is_running(channel) ? signal_gen_pin_names[app->clock_pin] : "busy");
}

static void clock_pin_list_change_callback(VariableItem* item) {
    SignalGenApp* app = variable_item_get_context(item);

    furi_hal_pwm_stop(signal_gen_pwm_ids[app->clock_pin]);
    app->clock_pin = variable_item_get_current_value_index(item);
    clock_start_output(app);
}

static void clock_source_list_change_callback(VariableItem* item) {
    SignalGenApp* app = variable_item_get_context(item);
    app->clock_base = variable_item_get_current_value_index(item);
    clock_update_freq_text(app);
    clock_start_output(app);
}

static void clock_divisor_list_change_callback(VariableItem* item) {
    SignalGenApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, clock_divisor_names[index]);

    app->clock_div = index;
    clock_update_freq_text(app);
    clock_start_output(app);
}

void signal_gen_scene_mco_on_enter(void* context) {
    SignalGenApp* app = context;
    VariableItemList* var_item_list = app->var_item_list;

    VariableItem* item;

    app->clock_pin = 0;
    app->clock_base = 0;
    app->clock_div = 0;

    item = variable_item_list_add(
        var_item_list,
        "GPIO Pin",
        COUNT_OF(signal_gen_pin_names),
        clock_pin_list_change_callback,
        app);
    variable_item_set_current_value_index(item, app->clock_pin);
    variable_item_set_current_value_text(item, signal_gen_pin_names[app->clock_pin]);
    app->clock_pin_item = item;

    // Shows the resulting output (base frequency / divider)
    item = variable_item_list_add(
        var_item_list,
        "Frequency",
        COUNT_OF(clock_base_freqs),
        clock_source_list_change_callback,
        app);
    variable_item_set_current_value_index(item, app->clock_base);
    app->clock_freq_item = item;
    clock_update_freq_text(app);

    item = variable_item_list_add(
        var_item_list,
        "Freq. divider",
        COUNT_OF(clock_divisor_names),
        clock_divisor_list_change_callback,
        app);
    variable_item_set_current_value_index(item, app->clock_div);
    variable_item_set_current_value_text(item, clock_divisor_names[app->clock_div]);

    variable_item_list_set_selected_item(var_item_list, LineIndexFrequency);

    view_dispatcher_switch_to_view(app->view_dispatcher, SignalGenViewVarItemList);

    clock_start_output(app);
}

bool signal_gen_scene_mco_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void signal_gen_scene_mco_on_exit(void* context) {
    SignalGenApp* app = context;
    variable_item_list_reset(app->var_item_list);

    furi_hal_pwm_stop(signal_gen_pwm_ids[app->clock_pin]);
}
