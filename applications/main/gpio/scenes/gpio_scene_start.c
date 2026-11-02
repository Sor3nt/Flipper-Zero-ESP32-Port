#include "../gpio_app_i.h"
#include <furi_hal_usb.h>
#include <dolphin/dolphin.h>

enum GpioItem {
    GpioItemUsbUart,
    GpioItemTest,
    GpioItemVbusSettings,
};

static void gpio_scene_start_var_list_enter_callback(void* context, uint32_t index) {
    furi_assert(context);
    GpioApp* app = context;
    if(index == GpioItemTest) {
        view_dispatcher_send_custom_event(app->view_dispatcher, GpioStartEventManualControl);
    } else if(index == GpioItemUsbUart) {
        view_dispatcher_send_custom_event(app->view_dispatcher, GpioStartEventUsbUart);
    } else if(index == GpioItemVbusSettings) {
        view_dispatcher_send_custom_event(app->view_dispatcher, GpioStartEventVbusSettings);
    }
}

void gpio_scene_start_on_enter(void* context) {
    GpioApp* app = context;
    VariableItemList* var_item_list = app->var_item_list;

    variable_item_list_set_enter_callback(
        var_item_list, gpio_scene_start_var_list_enter_callback, app);

    variable_item_list_add(var_item_list, "USB-UART Bridge", 0, NULL, NULL);

    variable_item_list_add(var_item_list, "GPIO Manual Control", 0, NULL, NULL);

    variable_item_list_add(var_item_list, "VBUS Settings", 0, NULL, NULL);

    variable_item_list_set_selected_item(
        var_item_list, scene_manager_get_scene_state(app->scene_manager, GpioSceneStart));

    view_dispatcher_switch_to_view(app->view_dispatcher, GpioAppViewVarItemList);
}

bool gpio_scene_start_on_event(void* context, SceneManagerEvent event) {
    GpioApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == GpioStartEventManualControl) {
            scene_manager_set_scene_state(app->scene_manager, GpioSceneStart, GpioItemTest);
            scene_manager_next_scene(app->scene_manager, GpioSceneTest);
        } else if(event.event == GpioStartEventUsbUart) {
            scene_manager_set_scene_state(app->scene_manager, GpioSceneStart, GpioItemUsbUart);
            if(!furi_hal_usb_is_locked()) {
                dolphin_deed(DolphinDeedGpioUartBridge);
                scene_manager_next_scene(app->scene_manager, GpioSceneUsbUart);
            } else {
                scene_manager_next_scene(app->scene_manager, GpioSceneUsbUartCloseRpc);
            }
        } else if(event.event == GpioStartEventVbusSettings) {
            scene_manager_set_scene_state(app->scene_manager, GpioSceneStart, GpioItemVbusSettings);
            scene_manager_next_scene(app->scene_manager, GpioSceneVbusSettings);
        }
        consumed = true;
    }
    return consumed;
}

void gpio_scene_start_on_exit(void* context) {
    GpioApp* app = context;
    variable_item_list_reset(app->var_item_list);
}
