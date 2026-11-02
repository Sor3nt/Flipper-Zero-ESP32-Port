#include "../usb_uart_bridge.h"
#include "../gpio_app_i.h"
#include <furi_hal.h>

static void gpio_usb_uart_config_changed(VariableItem* item) {
    GpioApp* app = variable_item_get_context(item);
    furi_assert(app);
    furi_assert(app->usb_uart_cfg);
    usb_uart_set_config(app->usb_uart_bridge, app->usb_uart_cfg);
}

static void line_usb_channel_cb(VariableItem* item) {
    GpioApp* app = variable_item_get_context(item);
    app->usb_uart_cfg->usb_channel = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(
        item, app->usb_uart_cfg->usb_channel == 0 ? "0" : "1");
    gpio_usb_uart_config_changed(item);
}

static void line_baudrate_cb(VariableItem* item) {
    GpioApp* app = variable_item_get_context(item);
    UsbUartConfig* cfg = app->usb_uart_cfg;
    cfg->baudrate_mode = variable_item_get_current_value_index(item);
    cfg->baudrate = cfg->baudrate_mode == 0 ?
                        0 :
                        usb_uart_baudrate_presets[cfg->baudrate_mode - 1];

    char value[12];
    if(cfg->baudrate_mode == 0) {
        variable_item_set_current_value_text(item, "Host");
    } else {
        snprintf(value, sizeof(value), "%lu", (unsigned long)cfg->baudrate);
        variable_item_set_current_value_text(item, value);
    }
    gpio_usb_uart_config_changed(item);
}

static void line_uart_pins_cb(VariableItem* item) {
    GpioApp* app = variable_item_get_context(item);
    app->usb_uart_cfg->uart_pin_pair = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(
        item, usb_uart_get_uart_pin_pair_text(app->usb_uart_cfg->uart_pin_pair));
    gpio_usb_uart_config_changed(item);
}

static void line_control_pins_cb(VariableItem* item) {
    GpioApp* app = variable_item_get_context(item);
    app->usb_uart_cfg->control_pin_pair = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(
        item, usb_uart_get_control_pin_pair_text(app->usb_uart_cfg->control_pin_pair));
    gpio_usb_uart_config_changed(item);
}

static void line_de_re_pin_cb(VariableItem* item) {
    GpioApp* app = variable_item_get_context(item);
    app->usb_uart_cfg->de_re_pin = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(
        item, usb_uart_get_de_re_pin_text(app->usb_uart_cfg->de_re_pin));
    gpio_usb_uart_config_changed(item);
}

bool gpio_scene_usb_uart_cfg_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void gpio_scene_usb_uart_cfg_on_enter(void* context) {
    GpioApp* app = context;
    furi_assert(app);
    furi_assert(app->usb_uart_bridge);

    furi_assert(app->usb_uart_cfg == NULL);
    app->usb_uart_cfg = malloc(sizeof(UsbUartConfig));
    furi_check(app->usb_uart_cfg);
    usb_uart_get_config(app->usb_uart_bridge, app->usb_uart_cfg);

    VariableItem* item = variable_item_list_add(
        app->var_item_list,
        "USB Channel",
        usb_uart_get_usb_channel_count(),
        line_usb_channel_cb,
        app);
    variable_item_set_current_value_index(item, app->usb_uart_cfg->usb_channel);
    variable_item_set_current_value_text(
        item, app->usb_uart_cfg->usb_channel == 0 ? "0" : "1");

    item = variable_item_list_add(
        app->var_item_list,
        "Baudrate",
        USB_UART_BAUDRATE_PRESET_COUNT + 1,
        line_baudrate_cb,
        app);
    if(app->usb_uart_cfg->baudrate_mode > USB_UART_BAUDRATE_PRESET_COUNT) {
        app->usb_uart_cfg->baudrate_mode = 0;
        app->usb_uart_cfg->baudrate = 0;
    }
    variable_item_set_current_value_index(item, app->usb_uart_cfg->baudrate_mode);
    if(app->usb_uart_cfg->baudrate_mode == 0) {
        variable_item_set_current_value_text(item, "Host");
    } else {
        char value[12];
        snprintf(
            value,
            sizeof(value),
            "%lu",
            (unsigned long)usb_uart_baudrate_presets[app->usb_uart_cfg->baudrate_mode - 1]);
        variable_item_set_current_value_text(item, value);
    }

    item = variable_item_list_add(
        app->var_item_list,
        "UART Pins",
        usb_uart_get_uart_pin_pair_count(),
        line_uart_pins_cb,
        app);
    variable_item_set_current_value_index(item, app->usb_uart_cfg->uart_pin_pair);
    variable_item_set_current_value_text(
        item, usb_uart_get_uart_pin_pair_text(app->usb_uart_cfg->uart_pin_pair));

    item = variable_item_list_add(
        app->var_item_list,
        "RTS/DTR Pins",
        usb_uart_get_control_pin_pair_count(),
        line_control_pins_cb,
        app);
    variable_item_set_current_value_index(item, app->usb_uart_cfg->control_pin_pair);
    variable_item_set_current_value_text(
        item, usb_uart_get_control_pin_pair_text(app->usb_uart_cfg->control_pin_pair));

    item = variable_item_list_add(
        app->var_item_list,
        "DE/RE Pin",
        usb_uart_get_de_re_pin_count(),
        line_de_re_pin_cb,
        app);
    variable_item_set_current_value_index(item, app->usb_uart_cfg->de_re_pin);
    variable_item_set_current_value_text(
        item, usb_uart_get_de_re_pin_text(app->usb_uart_cfg->de_re_pin));

    variable_item_list_set_selected_item(
        app->var_item_list, scene_manager_get_scene_state(app->scene_manager, GpioSceneUsbUartCfg));
    view_dispatcher_switch_to_view(app->view_dispatcher, GpioAppViewUsbUartCfg);
}

void gpio_scene_usb_uart_cfg_on_exit(void* context) {
    GpioApp* app = context;
    scene_manager_set_scene_state(
        app->scene_manager,
        GpioSceneUsbUartCfg,
        variable_item_list_get_selected_item_index(app->var_item_list));
    variable_item_list_reset(app->var_item_list);
    free(app->usb_uart_cfg);
    app->usb_uart_cfg = NULL;
}
