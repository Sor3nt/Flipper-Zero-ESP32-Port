#include "../gpio_app_i.h"
#include <furi_hal_power.h>
#include <furi_hal_usb.h>
#include <dolphin/dolphin.h>

enum GpioItem {
    GpioItemUsbUart,
    GpioItemTest,
    GpioItemOtg,
};

enum GpioOtg {
    GpioOtgOff,
    GpioOtgOn,
    GpioOtgSettingsNum,
};

const char* const gpio_otg_text[GpioOtgSettingsNum] = {
    "OFF",
    "ON",
};

/* List position -> GpioItem. "GPIO Manual Control" is listed only on boards with an
 * external GPIO header (Paradoxt), so positions and GpioItem values can differ. */
static uint8_t gpio_start_items[3];
static uint8_t gpio_start_items_count;

static uint32_t gpio_scene_start_item_at(uint32_t position) {
    return position < gpio_start_items_count ? gpio_start_items[position] : UINT32_MAX;
}

static uint32_t gpio_scene_start_position_of(uint32_t item) {
    for(uint8_t i = 0; i < gpio_start_items_count; i++) {
        if(gpio_start_items[i] == item) return i;
    }
    return 0;
}

static void gpio_scene_start_var_list_enter_callback(void* context, uint32_t position) {
    furi_assert(context);
    GpioApp* app = context;
    const uint32_t index = gpio_scene_start_item_at(position);
    if(index == GpioItemTest) {
        view_dispatcher_send_custom_event(app->view_dispatcher, GpioStartEventManualControl);
    } else if(index == GpioItemUsbUart) {
        view_dispatcher_send_custom_event(app->view_dispatcher, GpioStartEventUsbUart);
    }
}

static void gpio_scene_start_var_list_change_callback(VariableItem* item) {
    GpioApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);

    variable_item_set_current_value_text(item, gpio_otg_text[index]);
    if(index == GpioOtgOff) {
        view_dispatcher_send_custom_event(app->view_dispatcher, GpioStartEventOtgOff);
    } else if(index == GpioOtgOn) {
        view_dispatcher_send_custom_event(app->view_dispatcher, GpioStartEventOtgOn);
    }
}

void gpio_scene_start_on_enter(void* context) {
    GpioApp* app = context;
    VariableItemList* var_item_list = app->var_item_list;

    VariableItem* item;
    variable_item_list_set_enter_callback(
        var_item_list, gpio_scene_start_var_list_enter_callback, app);

    gpio_start_items_count = 0;
    variable_item_list_add(var_item_list, "USB-UART Bridge", 0, NULL, NULL);
    gpio_start_items[gpio_start_items_count++] = GpioItemUsbUart;

    if(gpio_items_manual_control_supported()) {
        variable_item_list_add(var_item_list, "GPIO Manual Control", 0, NULL, NULL);
        gpio_start_items[gpio_start_items_count++] = GpioItemTest;
    }

    /* On ESP32-S3 boards this is the only place to switch 5V (VBUS / OTG): the Infrared
     * app no longer offers it there. */
    gpio_start_items[gpio_start_items_count++] = GpioItemOtg;
    item = variable_item_list_add(
        var_item_list,
        "5V on GPIO",
        GpioOtgSettingsNum,
        gpio_scene_start_var_list_change_callback,
        app);
    if(power_is_otg_enabled(app->power)) {
        variable_item_set_current_value_index(item, GpioOtgOn);
        variable_item_set_current_value_text(item, gpio_otg_text[GpioOtgOn]);
    } else {
        variable_item_set_current_value_index(item, GpioOtgOff);
        variable_item_set_current_value_text(item, gpio_otg_text[GpioOtgOff]);
    }

    variable_item_list_set_selected_item(
        var_item_list,
        gpio_scene_start_position_of(
            scene_manager_get_scene_state(app->scene_manager, GpioSceneStart)));

    view_dispatcher_switch_to_view(app->view_dispatcher, GpioAppViewVarItemList);
}

bool gpio_scene_start_on_event(void* context, SceneManagerEvent event) {
    GpioApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == GpioStartEventOtgOn) {
            power_enable_otg(app->power, true);
        } else if(event.event == GpioStartEventOtgOff) {
            power_enable_otg(app->power, false);
        } else if(event.event == GpioStartEventManualControl) {
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
        }
        consumed = true;
    }
    return consumed;
}

void gpio_scene_start_on_exit(void* context) {
    GpioApp* app = context;
    variable_item_list_reset(app->var_item_list);
}
