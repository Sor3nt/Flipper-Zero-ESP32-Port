#pragma once

#include "esp_flasher_app.h"
#include "scenes/esp_flasher_scene.h"
#include "esp_flasher_custom_event.h"
#include "esp_flasher_hw.h"
#include "esp_flasher_reset.h"
#include "esp_flasher_uart.h"

#include <esp_loader.h>

#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/text_box.h>
#include <gui/modules/submenu.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/widget.h>
#include <gui/modules/text_input.h>

#include <storage/storage.h>
#include <toolbox/path.h>
#include <dialogs/dialogs.h>
#include <notification/notification_messages.h>

#include <assets_icons.h>

#define ESP_FLASHER_TEXT_BOX_STORE_SIZE (4096)

#define ESP_APP_FOLDER_USER "apps_data/esp_flasher"
#define ESP_APP_FOLDER      EXT_PATH(ESP_APP_FOLDER_USER)
/* Quick-flash firmware lives on the SD card; absolute so worker threads resolve it too. */
#define ESP_APP_ASSETS(path) ESP_APP_FOLDER "/assets/" path

#define ESP_FLASHER_SETTINGS_PATH    ESP_APP_FOLDER "/.settings"
#define ESP_FLASHER_SETTINGS_MAGIC   (0xE5)
#define ESP_FLASHER_SETTINGS_VERSION (1)

#define ESP_FLASHER_PATH_SIZE (256)

/* Flash slots, in flashing order. */
typedef enum SelectedFlashOptions {
    SelectedFlashBoot,
    SelectedFlashPart,
    SelectedFlashNvs,
    SelectedFlashBootApp0,
    SelectedFlashAppA,
    SelectedFlashAppB,
    SelectedFlashCustom,
    NUM_FLASH_OPTIONS
} SelectedFlashOptions;

typedef enum {
    SwitchNotSet,
    SwitchToFirmwareA,
    SwitchToFirmwareB,
} SwitchFirmware;

typedef enum {
    EspFlasherFastBaud921600,
    EspFlasherFastBaud460800,
    EspFlasherFastBaud230400,
    EspFlasherFastBaudCount,
} EspFlasherFastBaud;

typedef struct {
    uint8_t reset_mode; /* EspFlasherResetMode */
    uint8_t reserved;
    uint8_t fast_baud; /* EspFlasherFastBaud */
} EspFlasherSettings;

struct EspFlasherApp {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    SceneManager* scene_manager;

    /* Written by the worker and RX threads under console_mutex. */
    FuriString* text_box_store;
    /* What the text box points at. Only the app thread touches these: it fills the idle
     * one and swaps it in, so the GUI thread never draws a string that is being edited. */
    FuriString* text_box_display[2];
    uint8_t text_box_display_index;
    FuriMutex* console_mutex;
    bool console_active;
    bool console_refresh_pending;
    TextBox* text_box;
    Storage* storage;
    DialogsApp* dialogs;
    NotificationApp* notification;

    VariableItemList* var_item_list;
    Widget* widget;
    Submenu* submenu;
    TextInput* text_input;

    EspFlasherUart* uart;
    EspFlasherSettings settings;
    bool turbospeed;

    bool reset;
    bool boot;
    bool quickflash;

    SwitchFirmware switch_fw;

    /* ESP_UNKNOWN_CHIP = auto-detect; otherwise flashing aborts on any other chip. */
    target_chip_t expected_chip;

    bool selected_flash_options[NUM_FLASH_OPTIONS];
    int num_selected_flash_options;
    char bin_file_path_boot[ESP_FLASHER_PATH_SIZE];
    char bin_file_path_part[ESP_FLASHER_PATH_SIZE];
    char bin_file_path_nvs[ESP_FLASHER_PATH_SIZE];
    char bin_file_path_boot_app0[ESP_FLASHER_PATH_SIZE];
    char bin_file_path_app_a[ESP_FLASHER_PATH_SIZE];
    char bin_file_path_app_b[ESP_FLASHER_PATH_SIZE];
    char bin_file_path_custom[ESP_FLASHER_PATH_SIZE];

    // Per-slot flash addresses (initialized to defaults, overridable by user)
    uint32_t custom_slot_addrs[NUM_FLASH_OPTIONS];
    /* false: the bootloader offset follows the detected chip (0x0 / 0x1000 / 0x2000) */
    bool boot_addr_manually_set;
    int pending_addr_slot; // slot index awaiting address input (-1 = none)
    char addr_input_str[12]; // hex address string buffer for TextInput
    bool advanced_mode; // show custom addresses and Custom slot

    // Partition file parse results (populated when user selects a partition file)
    uint32_t parsed_slot_addrs[NUM_FLASH_OPTIONS]; // 0 = slot not found in file
    char part_confirm_text[512]; // formatted text for confirm widget

    FuriThread* flash_worker;
    volatile bool flash_worker_busy;
};

typedef enum {
    EspFlasherAppViewVarItemList,
    EspFlasherAppViewConsoleOutput,
    EspFlasherAppViewTextInput,
    EspFlasherAppViewWidget,
    EspFlasherAppViewSubmenu,
} EspFlasherAppView;

/* Appends to the console from any thread; coalesces GUI refresh events. */
void esp_flasher_console_append(EspFlasherApp* app, const uint8_t* buf, size_t len);
void esp_flasher_console_clear(EspFlasherApp* app);
/* App thread only: shows the current console contents in the text box. */
void esp_flasher_console_show(EspFlasherApp* app);

void esp_flasher_app_save_settings(EspFlasherApp* app);
uint32_t esp_flasher_fast_baud_value(EspFlasherFastBaud baud);

const char* esp_flasher_chip_name(target_chip_t chip);
/* Second-stage bootloader offset in flash for a chip family. */
uint32_t esp_flasher_boot_offset(target_chip_t chip);
