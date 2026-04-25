/*
 * SPDX-FileCopyrightText: 2020-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_private/usb_phy.h"
#include "tinyusb.h"
#include "descriptors_control.h"
#include "tusb.h"
#include "tusb_tasks.h"

const static char *TAG = "TinyUSB";
static usb_phy_handle_t phy_hdl;

// For the tinyusb component without tusb_teardown() implementation
#ifndef tusb_teardown
#   define tusb_teardown()   (true)
#endif // tusb_teardown

esp_err_t tinyusb_driver_install(const tinyusb_config_t *config)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "Config can't be NULL");

    // Configure USB PHY
    usb_phy_config_t phy_conf = {
        .controller = USB_PHY_CTRL_OTG,
        .otg_mode = USB_OTG_MODE_DEVICE,
#if (USB_PHY_SUPPORTS_P4_OTG11)
        .otg_speed = (TUD_OPT_HIGH_SPEED) ? USB_PHY_SPEED_HIGH : USB_PHY_SPEED_FULL,
#else
#if (CONFIG_IDF_TARGET_ESP32P4 && CONFIG_TINYUSB_RHPORT_FS)
#error "USB PHY for OTG1.1 is not supported, please update your esp-idf."
#endif // IDF_TARGET_ESP32P4 && CONFIG_TINYUSB_RHPORT_FS
#endif // USB_PHY_SUPPORTS_P4_OTG11
    };

    /*
    Following ext. PHY IO configuration is here to provide compatibility with IDFv5.x releases,
    where ext. PHY IOs were mapped to predefined GPIOs.
    In reality, ESP32-S2 and ESP32-S3 can map ext. PHY IOs to any GPIOs.
    This option is implemented in esp_tinyusb v2.0.0 and later.
    */
    usb_phy_ext_io_conf_t ext_io_conf;
    // Use memset to be compatible with IDF < 5.4.1 where suspend_n_io_num and fs_edge_sel_io_num were added
    memset(&ext_io_conf, -1, sizeof(usb_phy_ext_io_conf_t));
#if CONFIG_IDF_TARGET_ESP32S2
    ext_io_conf.vp_io_num  = 33;
    ext_io_conf.vm_io_num  = 34;
    ext_io_conf.rcv_io_num = 35;
    ext_io_conf.oen_io_num = 36;
    ext_io_conf.vpo_io_num = 37;
    ext_io_conf.vmo_io_num = 38;
#elif CONFIG_IDF_TARGET_ESP32S3
    ext_io_conf.vp_io_num  = 42;
    ext_io_conf.vm_io_num  = 41;
    ext_io_conf.rcv_io_num = 21;
    ext_io_conf.oen_io_num = 40;
    ext_io_conf.vpo_io_num = 39;
    ext_io_conf.vmo_io_num = 38;
#endif // IDF_TARGET_ESP32S3

    if (config->external_phy) {
        phy_conf.target = USB_PHY_TARGET_EXT;
        phy_conf.ext_io_conf = &ext_io_conf;

        /*
        There is a bug in esp-idf that does not allow device speed selection
        when External PHY is used.
        Remove this when proper fix is implemented in IDF-11144
        */
        phy_conf.otg_speed = USB_PHY_SPEED_UNDEFINED;
    } else {
        phy_conf.target = USB_PHY_TARGET_INT;
    }

    // OTG IOs config
    const usb_phy_otg_io_conf_t otg_io_conf = USB_PHY_SELF_POWERED_DEVICE(config->vbus_monitor_io);
    if (config->self_powered) {
        phy_conf.otg_io_conf = &otg_io_conf;
    }
    ESP_RETURN_ON_ERROR(usb_new_phy(&phy_conf, &phy_hdl), TAG, "Install USB PHY failed");

    // Descriptors config
    ESP_RETURN_ON_ERROR(tinyusb_set_descriptors(config), TAG, "Descriptors config failed");

    // Init
#if !CONFIG_TINYUSB_INIT_IN_DEFAULT_TASK
    ESP_RETURN_ON_FALSE(tusb_init(), ESP_FAIL, TAG, "Init TinyUSB stack failed");
#endif
#if !CONFIG_TINYUSB_NO_DEFAULT_TASK
    ESP_RETURN_ON_ERROR(tusb_run_task(), TAG, "Run TinyUSB task failed");
#endif
    ESP_LOGI(TAG, "TinyUSB Driver installed");
    return ESP_OK;
}

esp_err_t tinyusb_driver_uninstall(void)
{
#if !CONFIG_TINYUSB_NO_DEFAULT_TASK
    ESP_RETURN_ON_ERROR(tusb_stop_task(), TAG, "Unable to stop TinyUSB task");
#endif // !CONFIG_TINYUSB_NO_DEFAULT_TASK
    ESP_RETURN_ON_FALSE(tusb_teardown(), ESP_ERR_NOT_FINISHED, TAG, "Unable to teardown TinyUSB");
    tinyusb_free_descriptors();
    ESP_RETURN_ON_ERROR(usb_del_phy(phy_hdl), TAG, "Unable to delete PHY");
    return ESP_OK;
}
