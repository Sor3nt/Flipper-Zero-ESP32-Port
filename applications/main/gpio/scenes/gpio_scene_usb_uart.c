#include "../gpio_app_i.h"
#include "../usb_uart_bridge.h"

void gpio_scene_usb_uart_callback(GpioCustomEvent event, void* context) {
    furi_assert(context);
    GpioApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, event);
}

void gpio_scene_usb_uart_dialog_callback(DialogExResult result, void* context) {
    GpioApp* app = context;
    if(result == DialogExResultLeft) {
        if(app->usb_uart_bridge) {
            if(!usb_uart_disable(app->usb_uart_bridge)) {
                app->usb_uart_state.status = UsbUartStatusCleanupError;
                gpio_usb_uart_update_state(
                    app->gpio_usb_uart,
                    &app->usb_uart_state_config,
                    &app->usb_uart_state);
                view_dispatcher_switch_to_view(app->view_dispatcher, GpioAppViewUsbUart);
                return;
            }
            app->usb_uart_bridge = NULL;
        }
        scene_manager_search_and_switch_to_previous_scene(app->scene_manager, GpioSceneStart);
    } else {
        view_dispatcher_switch_to_view(app->view_dispatcher, GpioAppViewUsbUart);
    }
}

void gpio_scene_usb_uart_on_enter(void* context) {
    GpioApp* app = context;
    if(!app->usb_uart_bridge) {
        usb_uart_config_init(&app->usb_uart_state_config);
        app->usb_uart_bridge = usb_uart_enable(&app->usb_uart_state_config);
    }

    gpio_usb_uart_set_callback(app->gpio_usb_uart, gpio_scene_usb_uart_callback, app);
    if(app->usb_uart_bridge) {
        usb_uart_get_config(app->usb_uart_bridge, &app->usb_uart_state_config);
        usb_uart_get_state(app->usb_uart_bridge, &app->usb_uart_state);
    } else {
        app->usb_uart_state = (UsbUartState){.status = UsbUartStatusMemoryError};
    }
    gpio_usb_uart_update_state(
        app->gpio_usb_uart, &app->usb_uart_state_config, &app->usb_uart_state);
    view_dispatcher_switch_to_view(app->view_dispatcher, GpioAppViewUsbUart);
    notification_message(app->notifications, &sequence_display_backlight_enforce_on);
}

bool gpio_scene_usb_uart_on_event(void* context, SceneManagerEvent event) {
    GpioApp* app = context;
    if(event.type == SceneManagerEventTypeCustom) {
        if(!app->usb_uart_bridge) return true;
        usb_uart_get_state(app->usb_uart_bridge, &app->usb_uart_state);
        if(app->usb_uart_state.status != UsbUartStatusActive) return true;
        scene_manager_next_scene(app->scene_manager, GpioSceneUsbUartCfg);
        return true;
    } else if(event.type == SceneManagerEventTypeBack) {
        DialogEx* dialog = app->dialog;
        dialog_ex_set_context(dialog, app);
        dialog_ex_set_left_button_text(dialog, "Exit");
        dialog_ex_set_right_button_text(dialog, "Stay");
        dialog_ex_set_header(dialog, "Exit USB-UART?", 22, 12, AlignLeft, AlignTop);
        dialog_ex_set_result_callback(dialog, gpio_scene_usb_uart_dialog_callback);
        view_dispatcher_switch_to_view(app->view_dispatcher, GpioAppViewExitConfirm);
        return true;
    } else if(event.type == SceneManagerEventTypeTick) {
        if(!app->usb_uart_bridge) return false;
        uint32_t tx_cnt_last = app->usb_uart_state.tx_cnt;
        uint32_t rx_cnt_last = app->usb_uart_state.rx_cnt;
        usb_uart_get_state(app->usb_uart_bridge, &app->usb_uart_state);
        usb_uart_get_config(app->usb_uart_bridge, &app->usb_uart_state_config);
        if(app->usb_uart_state.status != UsbUartStatusActive) {
            if(usb_uart_disable(app->usb_uart_bridge)) {
                app->usb_uart_bridge = NULL;
            } else {
                app->usb_uart_state.status = UsbUartStatusCleanupError;
            }
        }
        gpio_usb_uart_update_state(
            app->gpio_usb_uart, &app->usb_uart_state_config, &app->usb_uart_state);
        if(tx_cnt_last != app->usb_uart_state.tx_cnt)
            notification_message(app->notifications, &sequence_blink_blue_10);
        if(rx_cnt_last != app->usb_uart_state.rx_cnt)
            notification_message(app->notifications, &sequence_blink_green_10);
    }
    return false;
}

void gpio_scene_usb_uart_on_exit(void* context) {
    GpioApp* app = context;
    notification_message(app->notifications, &sequence_display_backlight_enforce_auto);
}
