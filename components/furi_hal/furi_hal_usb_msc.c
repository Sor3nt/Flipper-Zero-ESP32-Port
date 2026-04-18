#include "furi_hal_usb_msc.h"

#include "furi_hal_sd.h"

#include <sdkconfig.h>
#include <string.h>

#include <esp_check.h>
#include <esp_log.h>
#include <sdmmc_cmd.h>

static const char* TAG = "FuriHalUsbMsc";

#if defined(CONFIG_TINYUSB_MSC_ENABLED) && CONFIG_TINYUSB_MSC_ENABLED
#include "tusb.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"

static tinyusb_msc_storage_handle_t s_msc_storage;
static bool s_tinyusb_installed;

enum {
    ITF_NUM_MSC = 0,
    ITF_NUM_TOTAL,
};

enum {
    EDPT_MSC_OUT = 0x01,
    EDPT_MSC_IN = 0x81,
};

#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

static tusb_desc_device_t s_device_desc = {
    .bLength = sizeof(s_device_desc),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,
    .idProduct = 0x4020,
    .bcdDevice = 0x100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

static uint8_t const s_fs_cfg_desc[] = {
    TUD_CONFIG_DESCRIPTOR(
        1,
        ITF_NUM_TOTAL,
        0,
        TUSB_DESC_TOTAL_LEN,
        TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP,
        100),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, EDPT_MSC_OUT, EDPT_MSC_IN, 64),
};

#if (TUD_OPT_HIGH_SPEED)
static tusb_desc_device_qualifier_t const s_device_qualifier = {
    .bLength = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 0x01,
    .bReserved = 0,
};

static uint8_t const s_hs_cfg_desc[] = {
    TUD_CONFIG_DESCRIPTOR(
        1,
        ITF_NUM_TOTAL,
        0,
        TUSB_DESC_TOTAL_LEN,
        TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP,
        100),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, EDPT_MSC_OUT, EDPT_MSC_IN, 512),
};
#endif

static char const* s_string_desc[] = {
    (const char[]){0x09, 0x04},
    "Flipper",
    "SD Card (USB)",
    "0",
};

static void msc_event_cb(tinyusb_msc_storage_handle_t handle, tinyusb_msc_event_t* event, void* arg) {
    (void)handle;
    (void)arg;
    if(!event) {
        return;
    }
    if(event->id == TINYUSB_MSC_EVENT_MOUNT_FAILED) {
        ESP_LOGE(TAG, "MSC mount failed");
    }
}

bool furi_hal_usb_msc_is_supported(void) {
    return true;
}

bool furi_hal_usb_msc_is_active(void) {
    return s_msc_storage != NULL;
}

esp_err_t furi_hal_usb_msc_start(void) {
    if(s_msc_storage != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    sdmmc_card_t* card = (sdmmc_card_t*)furi_hal_sd_get_mmc_card();
    ESP_RETURN_ON_FALSE(card != NULL, ESP_ERR_INVALID_STATE, TAG, "SD card not initialized");

    tinyusb_msc_storage_config_t storage_cfg = {
        .medium.card = card,
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
        .fat_fs =
            {
                .base_path = NULL,
                .config.max_files = 5,
                .do_not_format = true,
                .format_flags = 0,
            },
    };

    ESP_RETURN_ON_ERROR(tinyusb_msc_new_storage_sdmmc(&storage_cfg, &s_msc_storage), TAG, "msc storage");
    ESP_RETURN_ON_ERROR(tinyusb_msc_set_storage_callback(msc_event_cb, NULL), TAG, "msc cb");

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.device = &s_device_desc;
    tusb_cfg.descriptor.full_speed_config = s_fs_cfg_desc;
    tusb_cfg.descriptor.string = s_string_desc;
    tusb_cfg.descriptor.string_count = sizeof(s_string_desc) / sizeof(s_string_desc[0]);
#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.descriptor.high_speed_config = s_hs_cfg_desc;
    tusb_cfg.descriptor.qualifier = &s_device_qualifier;
#endif

    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb_cfg), TAG, "tinyusb_driver_install");
    s_tinyusb_installed = true;

    ESP_LOGI(TAG, "USB MSC started — SD visible to USB host");
    return ESP_OK;
}

esp_err_t furi_hal_usb_msc_stop(void) {
    if(s_msc_storage == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = tinyusb_msc_delete_storage(s_msc_storage);
    s_msc_storage = NULL;
    ESP_RETURN_ON_ERROR(err, TAG, "delete_storage");

    if(s_tinyusb_installed) {
        err = tinyusb_driver_uninstall();
        s_tinyusb_installed = false;
        ESP_RETURN_ON_ERROR(err, TAG, "tinyusb_driver_uninstall");
    }

    ESP_LOGI(TAG, "USB MSC stopped");
    return ESP_OK;
}

#else /* TinyUSB MSC disabled */

bool furi_hal_usb_msc_is_supported(void) {
    return false;
}

bool furi_hal_usb_msc_is_active(void) {
    return false;
}

esp_err_t furi_hal_usb_msc_start(void) {
    ESP_LOGW(TAG, "USB MSC not enabled in this firmware build");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t furi_hal_usb_msc_stop(void) {
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
