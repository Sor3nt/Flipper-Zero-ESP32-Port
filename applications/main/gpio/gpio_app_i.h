#pragma once

#include <furi.h>
#include "gpio_app.h"
#include "gpio_items.h"
#include "scenes/gpio_scene.h"
#include "gpio_custom_event.h"
#include "usb_uart_bridge.h"
#include <power/power_service/power.h>

#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/submenu.h>
#include <notification/notification_messages.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/widget.h>
#include <gui/modules/dialog_ex.h>
#include "views/gpio_test.h"
#include "views/gpio_usb_uart.h"
#include <assets_icons.h>
/* The upstream GPIO app disables the Expansion service (it shares the UART
 * pads) while running. This ESP32 port does not build that service, so there is
 * nothing to disable and the coupling is removed. */

struct GpioApp {
    Gui* gui;
    NotificationApp* notifications;
    ViewDispatcher* view_dispatcher;
    SceneManager* scene_manager;
    Widget* widget;
    DialogEx* dialog;
    Power* power;

    VariableItemList* var_item_list;
    GpioTest* gpio_test;
    GpioUsbUart* gpio_usb_uart;
    GPIOItems* gpio_items;
    UsbUartBridge* usb_uart_bridge;
    UsbUartConfig* usb_uart_cfg;
};

typedef enum {
    GpioAppViewVarItemList,
    GpioAppViewGpioTest,
    GpioAppViewUsbUart,
    GpioAppViewUsbUartCfg,
    GpioAppViewUsbUartCloseRpc,
    GpioAppViewExitConfirm,
} GpioAppView;
