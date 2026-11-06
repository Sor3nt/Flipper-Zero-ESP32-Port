#include "../gpio_app_i.h"
#include "../usb_uart_bridge.h"

typedef struct {
    UsbUartConfig cfg;
    UsbUartState state;
} SceneUsbUartBridge;

static SceneUsbUartBridge* scene_usb_uart = NULL;

void gpio_scene_usb_uart_callback(GpioCustomEvent event, void* context) {
    furi_assert(context);
    GpioApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, event);
}

void gpio_scene_usb_uart_dialog_callback(DialogExResult result, void* context) {
    GpioApp* app = context;
    if(!app->usb_uart_bridge) {
        free(scene_usb_uart);
        scene_usb_uart = NULL;
        scene_manager_search_and_switch_to_previous_scene(app->scene_manager, GpioSceneStart);
        return;
    }

    if(result == DialogExResultLeft) {
        if(!usb_uart_disable(app->usb_uart_bridge)) {
            dialog_ex_set_left_button_text(app->dialog, "Retry");
            dialog_ex_set_right_button_text(app->dialog, "Stay");
            dialog_ex_set_header(
                app->dialog, "Cleanup failed", 64, 12, AlignCenter, AlignCenter);
            dialog_ex_set_text(
                app->dialog,
                "USB/UART is still reserved.\nRetry or restart the device.",
                64,
                32,
                AlignCenter,
                AlignCenter);
            view_dispatcher_switch_to_view(app->view_dispatcher, GpioAppViewExitConfirm);
            return;
        }
        app->usb_uart_bridge = NULL;
        free(scene_usb_uart);
        scene_usb_uart = NULL;
        scene_manager_search_and_switch_to_previous_scene(app->scene_manager, GpioSceneStart);
    } else {
        view_dispatcher_switch_to_view(app->view_dispatcher, GpioAppViewUsbUart);
    }
}

void gpio_scene_usb_uart_on_enter(void* context) {
    GpioApp* app = context;
    if(!scene_usb_uart) {
        scene_usb_uart = malloc(sizeof(SceneUsbUartBridge));
        if(!scene_usb_uart) {
            FURI_LOG_E("GpioUsbUart", "Could not allocate scene state");
            scene_manager_search_and_switch_to_previous_scene(
                app->scene_manager, GpioSceneStart);
            return;
        }
        scene_usb_uart->cfg.vcp_ch = 0;
        scene_usb_uart->cfg.uart_ch = 0;
        scene_usb_uart->cfg.flow_pins = 0;
        scene_usb_uart->cfg.baudrate_mode = 0;
        scene_usb_uart->cfg.baudrate = 0;
        app->usb_uart_bridge = usb_uart_enable(&scene_usb_uart->cfg);
        if(!app->usb_uart_bridge) {
            dialog_ex_set_left_button_text(app->dialog, "Back");
            dialog_ex_set_right_button_text(app->dialog, "Dismiss");
            dialog_ex_set_header(
                app->dialog, "USB-UART unavailable", 64, 12, AlignCenter, AlignCenter);
            dialog_ex_set_text(
                app->dialog,
                "Check board support and USB/UART ownership.",
                64,
                32,
                AlignCenter,
                AlignCenter);
            dialog_ex_set_result_callback(app->dialog, gpio_scene_usb_uart_dialog_callback);
            dialog_ex_set_context(app->dialog, app);
            view_dispatcher_switch_to_view(app->view_dispatcher, GpioAppViewExitConfirm);
            return;
        }
    }

    usb_uart_get_config(app->usb_uart_bridge, &scene_usb_uart->cfg);
    usb_uart_get_state(app->usb_uart_bridge, &scene_usb_uart->state);

    gpio_usb_uart_set_callback(app->gpio_usb_uart, gpio_scene_usb_uart_callback, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, GpioAppViewUsbUart);
    notification_message(app->notifications, &sequence_display_backlight_enforce_on);
}

bool gpio_scene_usb_uart_on_event(void* context, SceneManagerEvent event) {
    GpioApp* app = context;
    if(event.type == SceneManagerEventTypeCustom) {
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
        if(!app->usb_uart_bridge || !scene_usb_uart) return true;
        uint32_t tx_cnt_last = scene_usb_uart->state.tx_cnt;
        uint32_t rx_cnt_last = scene_usb_uart->state.rx_cnt;
        usb_uart_get_state(app->usb_uart_bridge, &scene_usb_uart->state);
        gpio_usb_uart_update_state(
            app->gpio_usb_uart, &scene_usb_uart->cfg, &scene_usb_uart->state);
        if(tx_cnt_last != scene_usb_uart->state.tx_cnt)
            notification_message(app->notifications, &sequence_blink_blue_10);
        if(rx_cnt_last != scene_usb_uart->state.rx_cnt)
            notification_message(app->notifications, &sequence_blink_green_10);
    }
    return false;
}

void gpio_scene_usb_uart_on_exit(void* context) {
    GpioApp* app = context;
    notification_message(app->notifications, &sequence_display_backlight_enforce_auto);
    if(!app->usb_uart_bridge && scene_usb_uart) {
        free(scene_usb_uart);
        scene_usb_uart = NULL;
    }
}
