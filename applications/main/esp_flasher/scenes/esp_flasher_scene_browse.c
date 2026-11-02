#include "../esp_flasher_app_i.h"

#include "../esp_flasher_worker.h"
#include "../esp_flasher_partition.h"

#include <stdlib.h>

enum SubmenuIndex {
    SubmenuIndexAdvancedMode,
    SubmenuIndexTarget,
    SubmenuIndexBoot,
    SubmenuIndexPart,
    SubmenuIndexNvs,
    SubmenuIndexBootApp0,
    SubmenuIndexAppA,
    SubmenuIndexAppB,
    SubmenuIndexCustom,
    SubmenuIndexFlash,
    SubmenuIndexFlashTurbo,
};

/* "Target" cycles through these; anything but auto-detect aborts on another chip. */
static const target_chip_t browse_targets[] = {
    ESP_UNKNOWN_CHIP,
    ESP32_CHIP,
    ESP32S2_CHIP,
    ESP32S3_CHIP,
    ESP32C3_CHIP,
    ESP32C5_CHIP,
    ESP32C6_CHIP,
};

typedef struct {
    enum SubmenuIndex index;
    SelectedFlashOptions slot;
    const char* name;
    const char* addr_header;
} BrowseSlot;

static const BrowseSlot browse_slots[] = {
    {SubmenuIndexBoot, SelectedFlashBoot, "Bootloader", "Bootloader offset (hex)"},
    {SubmenuIndexPart, SelectedFlashPart, "Part Table", "Partition table offset (hex)"},
    {SubmenuIndexNvs, SelectedFlashNvs, "NVS", "NVS offset (hex)"},
    {SubmenuIndexBootApp0, SelectedFlashBootApp0, "boot_app0", "boot_app0 offset (hex)"},
    {SubmenuIndexAppA, SelectedFlashAppA, "FirmwareA", "Firmware A offset (hex)"},
    {SubmenuIndexAppB, SelectedFlashAppB, "FirmwareB", "Firmware B offset (hex)"},
    {SubmenuIndexCustom, SelectedFlashCustom, "Custom", "Custom flash offset (hex)"},
};

/* true while the address prompt or the partition dialog covers the file list */
static bool s_overlay_shown;

static void _parse_partition_file(EspFlasherApp* app, const char* filepath);
static void _part_confirm_widget_callback(GuiButtonType result, InputType type, void* context);
static void _show_part_confirm(EspFlasherApp* app);

static char* _slot_path(EspFlasherApp* app, SelectedFlashOptions slot) {
    switch(slot) {
    case SelectedFlashBoot:
        return app->bin_file_path_boot;
    case SelectedFlashPart:
        return app->bin_file_path_part;
    case SelectedFlashNvs:
        return app->bin_file_path_nvs;
    case SelectedFlashBootApp0:
        return app->bin_file_path_boot_app0;
    case SelectedFlashAppA:
        return app->bin_file_path_app_a;
    case SelectedFlashAppB:
        return app->bin_file_path_app_b;
    default:
        return app->bin_file_path_custom;
    }
}

static const BrowseSlot* _slot_for_index(uint32_t index) {
    for(size_t i = 0; i < COUNT_OF(browse_slots); i++) {
        if((uint32_t)browse_slots[i].index == index) return &browse_slots[i];
    }
    return NULL;
}

static uint32_t _default_boot_addr(EspFlasherApp* app) {
    // auto-detect: the worker derives it from the chip; 0x1000 is only a placeholder
    return app->expected_chip == ESP_UNKNOWN_CHIP ? ESP_ADDR_BOOT :
                                                    esp_flasher_boot_offset(app->expected_chip);
}

static void _select_slot(EspFlasherApp* app, const BrowseSlot* slot) {
    bool* selected = &app->selected_flash_options[slot->slot];
    char* path = _slot_path(app, slot->slot);

    *selected = !*selected;
    if(*selected) {
        FuriString* predefined_filepath = furi_string_alloc_set_str(ESP_APP_FOLDER);
        FuriString* selected_filepath = furi_string_alloc();
        DialogsFileBrowserOptions browser_options;
        dialog_file_browser_set_basic_options(&browser_options, ".bin", &I_file_10px);

        if(dialog_file_browser_show(
               app->dialogs, selected_filepath, predefined_filepath, &browser_options)) {
            if(furi_string_size(selected_filepath) < ESP_FLASHER_PATH_SIZE) {
                snprintf(path, ESP_FLASHER_PATH_SIZE, "%s", furi_string_get_cstr(selected_filepath));
            } else {
                dialog_message_show_storage_error(app->dialogs, "Path too long");
            }
        }
        furi_string_free(selected_filepath);
        furi_string_free(predefined_filepath);
    } else {
        path[0] = '\0';
    }

    if(path[0] == '\0') {
        // if user didn't select a file, leave unselected
        *selected = false;
        view_dispatcher_send_custom_event(app->view_dispatcher, EspFlasherEventRefreshSubmenu);
        return;
    }

    if(app->advanced_mode || slot->slot == SelectedFlashCustom) {
        // prompt user to confirm or override the default address
        app->pending_addr_slot = slot->slot;
        snprintf(
            app->addr_input_str,
            sizeof(app->addr_input_str),
            "0x%lx",
            app->custom_slot_addrs[slot->slot]);
    }

    if(slot->slot == SelectedFlashPart) {
        // parse partition file and offer to apply addresses regardless of mode
        _parse_partition_file(app, path);
        for(int i = 0; i < NUM_FLASH_OPTIONS; i++) {
            if(app->parsed_slot_addrs[i] != 0) {
                view_dispatcher_send_custom_event(
                    app->view_dispatcher, EspFlasherEventShowPartConfirm);
                return;
            }
        }
    }

    view_dispatcher_send_custom_event(
        app->view_dispatcher,
        app->pending_addr_slot >= 0 ? EspFlasherEventShowAddressInput :
                                      EspFlasherEventRefreshSubmenu);
}

static void esp_flasher_scene_browse_callback(void* context, uint32_t index) {
    EspFlasherApp* app = context;

    scene_manager_set_scene_state(app->scene_manager, EspFlasherSceneBrowse, index);
    app->pending_addr_slot = -1;

    switch(index) {
    case SubmenuIndexAdvancedMode:
        app->advanced_mode = !app->advanced_mode;
        // leaving advanced mode: reset any manually-set bootloader address and
        // clear the custom slot so it does not silently flash on next run
        if(!app->advanced_mode) {
            app->boot_addr_manually_set = false;
            app->custom_slot_addrs[SelectedFlashBoot] = _default_boot_addr(app);
            app->selected_flash_options[SelectedFlashCustom] = false;
            app->bin_file_path_custom[0] = '\0';
        }
        view_dispatcher_send_custom_event(app->view_dispatcher, EspFlasherEventRefreshSubmenu);
        break;

    case SubmenuIndexTarget: {
        size_t next = 0;
        for(size_t i = 0; i < COUNT_OF(browse_targets); i++) {
            if(browse_targets[i] == app->expected_chip) {
                next = (i + 1) % COUNT_OF(browse_targets);
                break;
            }
        }
        app->expected_chip = browse_targets[next];
        // update bootloader default address unless user set one manually
        if(!app->boot_addr_manually_set) {
            app->custom_slot_addrs[SelectedFlashBoot] = _default_boot_addr(app);
        }
        view_dispatcher_send_custom_event(app->view_dispatcher, EspFlasherEventRefreshSubmenu);
        break;
    }

    case SubmenuIndexFlash:
    case SubmenuIndexFlashTurbo:
        app->turbospeed = (index == SubmenuIndexFlashTurbo); // faster baudrate toggle
        app->num_selected_flash_options = 0;
        for(int i = 0; i < NUM_FLASH_OPTIONS; i++) {
            if(app->selected_flash_options[i]) ++app->num_selected_flash_options;
        }
        if(app->num_selected_flash_options) {
            // only start next scene if at least one option is selected
            app->reset = false;
            app->boot = false;
            app->quickflash = false;
            app->switch_fw = SwitchNotSet;
            scene_manager_next_scene(app->scene_manager, EspFlasherSceneConsoleOutput);
        }
        break;

    default: {
        const BrowseSlot* slot = _slot_for_index(index);
        if(slot) _select_slot(app, slot);
        break;
    }
    }
}

#define STR_SELECT   "[x]"
#define STR_UNSELECT "[ ]"

static void _parse_partition_file(EspFlasherApp* app, const char* filepath) {
    EspFlasherPartitionInfo info;
    esp_flasher_partition_parse(
        app->storage, filepath, &info, app->part_confirm_text, sizeof(app->part_confirm_text));
    memcpy(app->parsed_slot_addrs, info.slot_addrs, sizeof(app->parsed_slot_addrs));
}

static void _part_confirm_widget_callback(GuiButtonType result, InputType type, void* context) {
    EspFlasherApp* app = context;
    if(type == InputTypeShort) {
        view_dispatcher_send_custom_event(
            app->view_dispatcher,
            result == GuiButtonTypeRight ? EspFlasherEventPartConfirmYes :
                                           EspFlasherEventPartConfirmNo);
    }
}

static void _show_part_confirm(EspFlasherApp* app) {
    s_overlay_shown = true;
    widget_reset(app->widget);
    widget_add_text_scroll_element(app->widget, 0, 0, 128, 52, app->part_confirm_text);
    widget_add_button_element(
        app->widget, GuiButtonTypeLeft, "No", _part_confirm_widget_callback, app);
    widget_add_button_element(
        app->widget, GuiButtonTypeRight, "Yes", _part_confirm_widget_callback, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, EspFlasherAppViewWidget);
}

static void _addr_input_callback(void* context) {
    EspFlasherApp* app = context;

    uint32_t addr = (uint32_t)strtoul(app->addr_input_str, NULL, 16);
    app->custom_slot_addrs[app->pending_addr_slot] = addr;

    // if user explicitly changed the bootloader address, stop auto-updating it
    if(app->pending_addr_slot == SelectedFlashBoot) {
        app->boot_addr_manually_set = true;
    }

    view_dispatcher_send_custom_event(app->view_dispatcher, EspFlasherEventAddrInputDone);
}

static bool _addr_input_validator(const char* text, FuriString* error, void* context) {
    UNUSED(context);
    const char* digits = text;
    if(digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) digits += 2;

    size_t digit_count = 0;
    for(const char* p = digits; *p; p++) {
        const char c = *p;
        if(!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
             (c >= 'A' && c <= 'F'))) {
            furi_string_set(error, "Enter a hex address");
            return false;
        }
        digit_count++;
    }

    const uint64_t addr = strtoull(text, NULL, 16);
    if(digit_count == 0 || digit_count > 8 || (addr & 3) != 0) {
        furi_string_set(error, "Use 0x0-0xFFFFFFFF, aligned to 4");
        return false;
    }
    return true;
}

static void _refresh_submenu(EspFlasherApp* app) {
    Submenu* submenu = app->submenu;
    char label[64];

    s_overlay_shown = false;
    submenu_reset(app->submenu);

    submenu_set_header(submenu, "Browse for files to flash");

    submenu_add_item(
        submenu,
        app->advanced_mode ? "[x] Advanced Mode" : "[ ] Advanced Mode",
        SubmenuIndexAdvancedMode,
        esp_flasher_scene_browse_callback,
        app);

    snprintf(
        label,
        sizeof(label),
        "Target: %s",
        app->expected_chip == ESP_UNKNOWN_CHIP ? "Auto-detect" :
                                                 esp_flasher_chip_name(app->expected_chip));
    submenu_add_item(submenu, label, SubmenuIndexTarget, esp_flasher_scene_browse_callback, app);

    for(size_t i = 0; i < COUNT_OF(browse_slots); i++) {
        const BrowseSlot* slot = &browse_slots[i];
        // the Custom slot is only offered in advanced mode
        if(slot->slot == SelectedFlashCustom && !app->advanced_mode) continue;

        const char* check = app->selected_flash_options[slot->slot] ? STR_SELECT : STR_UNSELECT;
        if(slot->slot == SelectedFlashBoot && !app->boot_addr_manually_set &&
           app->expected_chip == ESP_UNKNOWN_CHIP) {
            snprintf(label, sizeof(label), "%s %s (auto)", check, slot->name);
        } else {
            snprintf(
                label,
                sizeof(label),
                "%s %s (0x%lx)",
                check,
                slot->name,
                app->custom_slot_addrs[slot->slot]);
        }
        submenu_add_item(submenu, label, slot->index, esp_flasher_scene_browse_callback, app);
    }

    submenu_add_item(
        submenu,
        "[>] FLASH - fast",
        SubmenuIndexFlashTurbo,
        esp_flasher_scene_browse_callback,
        app);
    submenu_add_item(
        submenu, "[>] FLASH - slow", SubmenuIndexFlash, esp_flasher_scene_browse_callback, app);

    submenu_set_selected_item(
        submenu, scene_manager_get_scene_state(app->scene_manager, EspFlasherSceneBrowse));
    view_dispatcher_switch_to_view(app->view_dispatcher, EspFlasherAppViewSubmenu);
}

void esp_flasher_scene_browse_on_enter(void* context) {
    EspFlasherApp* app = context;

    memset(app->selected_flash_options, 0, sizeof(app->selected_flash_options));
    for(int i = 0; i < NUM_FLASH_OPTIONS; i++) {
        _slot_path(app, i)[0] = '\0';
    }

    // initialise slot addresses to standard defaults
    app->expected_chip = ESP_UNKNOWN_CHIP;
    app->boot_addr_manually_set = false;
    app->custom_slot_addrs[SelectedFlashBoot] = _default_boot_addr(app);
    app->custom_slot_addrs[SelectedFlashPart] = ESP_ADDR_PART;
    app->custom_slot_addrs[SelectedFlashNvs] = ESP_ADDR_NVS;
    app->custom_slot_addrs[SelectedFlashBootApp0] = ESP_ADDR_BOOT_APP0;
    app->custom_slot_addrs[SelectedFlashAppA] = ESP_ADDR_APP_A;
    app->custom_slot_addrs[SelectedFlashAppB] = ESP_ADDR_APP_B;
    app->custom_slot_addrs[SelectedFlashCustom] = 0x0;

    app->pending_addr_slot = -1;
    s_overlay_shown = false;
    // advanced_mode intentionally not reset here so it persists across
    // re-entries (e.g. back-navigating from console output)

    _refresh_submenu(app);
}

bool esp_flasher_scene_browse_on_event(void* context, SceneManagerEvent event) {
    EspFlasherApp* app = context;
    bool consumed = false;
    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == EspFlasherEventRefreshSubmenu) {
            _refresh_submenu(app);
            consumed = true;
        } else if(event.event == EspFlasherEventShowAddressInput) {
            // show text input so user can confirm or override the default address
            const char* header = "Flash offset (hex)";
            for(size_t i = 0; i < COUNT_OF(browse_slots); i++) {
                if((int)browse_slots[i].slot == app->pending_addr_slot) {
                    header = browse_slots[i].addr_header;
                }
            }

            s_overlay_shown = true;
            text_input_reset(app->text_input);
            text_input_set_header_text(app->text_input, header);
            text_input_set_validator(app->text_input, _addr_input_validator, NULL);
            text_input_set_result_callback(
                app->text_input,
                _addr_input_callback,
                app,
                app->addr_input_str,
                sizeof(app->addr_input_str),
                false);
            view_dispatcher_switch_to_view(app->view_dispatcher, EspFlasherAppViewTextInput);
            consumed = true;
        } else if(event.event == EspFlasherEventAddrInputDone) {
            app->pending_addr_slot = -1;
            _refresh_submenu(app);
            consumed = true;
        } else if(event.event == EspFlasherEventShowPartConfirm) {
            _show_part_confirm(app);
            consumed = true;
        } else if(
            event.event == EspFlasherEventPartConfirmYes ||
            event.event == EspFlasherEventPartConfirmNo) {
            if(event.event == EspFlasherEventPartConfirmYes) {
                // apply parsed addresses to any matched slots
                for(int i = 0; i < NUM_FLASH_OPTIONS; i++) {
                    if(app->parsed_slot_addrs[i] != 0) {
                        app->custom_slot_addrs[i] = app->parsed_slot_addrs[i];
                    }
                }
            }
            widget_reset(app->widget);
            if(app->pending_addr_slot >= 0) {
                // still let user confirm or override the partition table's own flash address
                view_dispatcher_send_custom_event(
                    app->view_dispatcher, EspFlasherEventShowAddressInput);
            } else {
                _refresh_submenu(app);
            }
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack && s_overlay_shown) {
        // Back from the address prompt or the partition dialog returns to the file list
        // instead of dropping every selection.
        app->pending_addr_slot = -1;
        widget_reset(app->widget);
        _refresh_submenu(app);
        consumed = true;
    }

    return consumed;
}

void esp_flasher_scene_browse_on_exit(void* context) {
    EspFlasherApp* app = context;
    submenu_reset(app->submenu);
    widget_reset(app->widget);
    text_input_reset(app->text_input);
    text_input_set_validator(app->text_input, NULL, NULL);
}
