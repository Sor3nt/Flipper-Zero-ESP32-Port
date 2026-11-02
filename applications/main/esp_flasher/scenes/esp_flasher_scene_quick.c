#include "../esp_flasher_app_i.h"
#include "../esp_flasher_worker.h"
#include "../esp_flasher_partition.h"

// Marauder firmware source - https://github.com/justcallmekoko/ESP32Marauder
// BlackMagic firmware source - https://github.com/flipperdevices/blackmagic-esp32-s2
// FlipperHTTP firmware source - https://github.com/jblanked/FlipperHTTP
// Wardriver firmware source - https://github.com/Sil333033/flipperzero-wardriver

// One scene shows both levels: the chip list (QuickStart and the chip entries) and, for
// states after a chip entry, that chip's firmware list. Firmware states of a chip sit
// between its chip entry and the next chip entry. Whether the target is put into download
// mode automatically follows Settings > Reset pins, so there is one entry per chip.
enum QuickState {
    QuickStart,
    QuickS2,
    QuickS2_Marauder,
    QuickS2_Flipperhttp,
    QuickS2_Blackmagic,
    QuickWROOM,
    QuickWROOM_Marauder,
    QuickWROOM_Wardriver,
    QuickS3,
    QuickS3_Marauder,
    QuickS3_Wardriver,
    QuickC5,
    QuickC5_Marauder,
    QuickC5_Custom,
};

static uint32_t esp_flasher_scene_quick_chip_of(uint32_t state) {
    if(state >= QuickC5) return QuickC5;
    if(state >= QuickS3) return QuickS3;
    if(state >= QuickWROOM) return QuickWROOM;
    if(state >= QuickS2) return QuickS2;
    return QuickStart;
}

static void esp_flasher_scene_quick_submenu_callback(void* context, uint32_t index) {
    furi_assert(context);
    EspFlasherApp* app = context;

    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

static void esp_flasher_scene_quick_add(EspFlasherApp* app, const char* label, uint32_t index) {
    submenu_add_item(app->submenu, label, index, esp_flasher_scene_quick_submenu_callback, app);
}

void esp_flasher_scene_quick_on_enter(void* context) {
    furi_assert(context);
    EspFlasherApp* app = context;
    Submenu* submenu = app->submenu;
    uint32_t state = scene_manager_get_scene_state(app->scene_manager, EspFlasherSceneQuick);

    switch(state) {
    case QuickStart:
    case QuickS2:
    case QuickWROOM:
    case QuickS3:
    case QuickC5:
        submenu_set_header(submenu, "Choose ESP32 Type:");
        esp_flasher_scene_quick_add(app, "ESP32-S2 / WiFi Devboard", QuickS2);
        esp_flasher_scene_quick_add(app, "ESP32 (WROOM)", QuickWROOM);
        esp_flasher_scene_quick_add(app, "ESP32-S3", QuickS3);
        esp_flasher_scene_quick_add(app, "ESP32-C5", QuickC5);
        break;
    case QuickS2_Marauder:
    case QuickS2_Flipperhttp:
    case QuickS2_Blackmagic:
        submenu_set_header(submenu, "ESP32-S2 Firmware:");
        esp_flasher_scene_quick_add(app, "Marauder (has Evil Portal)", QuickS2_Marauder);
        esp_flasher_scene_quick_add(app, "FlipperHTTP (web access)", QuickS2_Flipperhttp);
        esp_flasher_scene_quick_add(app, "Black Magic (FZ debugger)", QuickS2_Blackmagic);
        break;
    case QuickWROOM_Marauder:
    case QuickWROOM_Wardriver:
        submenu_set_header(submenu, "ESP32 Firmware:");
        esp_flasher_scene_quick_add(app, "Marauder (has Evil Portal)", QuickWROOM_Marauder);
        esp_flasher_scene_quick_add(app, "Wardriver (GPS -> Flipper)", QuickWROOM_Wardriver);
        break;
    case QuickS3_Marauder:
    case QuickS3_Wardriver:
        submenu_set_header(submenu, "ESP32-S3 Firmware:");
        esp_flasher_scene_quick_add(app, "Marauder (has Evil Portal)", QuickS3_Marauder);
        esp_flasher_scene_quick_add(app, "Wardriver (GPS -> Flipper)", QuickS3_Wardriver);
        break;
    case QuickC5_Marauder:
    case QuickC5_Custom:
        submenu_set_header(submenu, "ESP32-C5 Firmware:");
        esp_flasher_scene_quick_add(app, "Marauder (has Evil Portal)", QuickC5_Marauder);
        esp_flasher_scene_quick_add(app, "Custom (custom/c5)", QuickC5_Custom);
        break;
    default:
        break;
    }

    submenu_set_selected_item(submenu, state);

    view_dispatcher_switch_to_view(app->view_dispatcher, EspFlasherAppViewSubmenu);
}

static void esp_flasher_scene_quick_set_path(
    bool* selected,
    char* dest,
    size_t dest_size,
    const char* path) {
    if(!path) return;
    *selected = true;
    snprintf(dest, dest_size, "%s", path);
}

bool esp_flasher_scene_quick_on_event(void* context, SceneManagerEvent event) {
    furi_assert(context);

    EspFlasherApp* app = context;
    bool consumed = false;
    if(event.type == SceneManagerEventTypeCustom) {
        consumed = true;

        target_chip_t chip = ESP_UNKNOWN_CHIP;
        const char* boot = NULL; // chip's bootloader offset (0x1000 / 0x0 / 0x2000)
        const char* part = NULL; // 0x8000
        const char* app0 = NULL; // otadata, 0xE000 (from the partition table)
        const char* firm = NULL; // first app partition, 0x10000 (from the partition table)

// Official Marauder release set (marauder-installer-assets.zip), one folder per chip
#define MARAUDER_SET(dir)                                         \
    do {                                                          \
        boot = ESP_APP_ASSETS("marauder/" dir "/bootloader.bin"); \
        part = ESP_APP_ASSETS("marauder/" dir "/partitions.bin"); \
        app0 = ESP_APP_ASSETS("marauder/" dir "/boot_app0.bin");  \
        firm = ESP_APP_ASSETS("marauder/" dir "/firmware.bin");   \
    } while(0)

        switch(event.event) {
        case QuickS2:
        case QuickWROOM:
        case QuickS3:
        case QuickC5:
            // Select first item of submenu
            scene_manager_set_scene_state(
                app->scene_manager, EspFlasherSceneQuick, event.event + 1);
            scene_manager_next_scene(app->scene_manager, EspFlasherSceneQuick);
            return consumed;

        case QuickS2_Marauder:
            chip = ESP32S2_CHIP;
            MARAUDER_SET("s2");
            break;

        case QuickS2_Flipperhttp:
            chip = ESP32S2_CHIP;
            boot = ESP_APP_ASSETS("flipperhttp/s2/flipper_http_bootloader.bin");
            part = ESP_APP_ASSETS("flipperhttp/s2/flipper_http_partitions.bin");
            firm = ESP_APP_ASSETS("flipperhttp/s2/flipper_http_firmware_a.bin");
            // resets otadata to ota_0, else a previous "Switch to Firmware B" keeps booting app1
            if(storage_file_exists(app->storage, ESP_APP_ASSETS("flipperhttp/s2/boot_app0.bin"))) {
                app0 = ESP_APP_ASSETS("flipperhttp/s2/boot_app0.bin");
            }
            break;

        case QuickS2_Blackmagic:
            chip = ESP32S2_CHIP;
            boot = ESP_APP_ASSETS("blackmagic/s2/bootloader.bin");
            part = ESP_APP_ASSETS("blackmagic/s2/partition-table.bin");
            firm = ESP_APP_ASSETS("blackmagic/s2/blackmagic.bin");
            break;

        case QuickWROOM_Marauder:
            chip = ESP32_CHIP;
            MARAUDER_SET("wroom");
            break;

        case QuickWROOM_Wardriver:
            // merged image, starts at the bootloader offset
            chip = ESP32_CHIP;
            boot = ESP_APP_ASSETS("wardriver/f0-wardrive-wroom.bin");
            break;

        case QuickS3_Marauder:
            chip = ESP32S3_CHIP;
            MARAUDER_SET("s3");
            break;

        case QuickS3_Wardriver:
            chip = ESP32S3_CHIP;
            boot = ESP_APP_ASSETS("wardriver/f0-wardrive-s3.bin");
            break;

        case QuickC5_Marauder:
            chip = ESP32C5_CHIP;
            MARAUDER_SET("c5");
            break;

        case QuickC5_Custom:
            // own build: boot_app0 (otadata) only when the layout has one
            chip = ESP32C5_CHIP;
            boot = ESP_APP_ASSETS("custom/c5/bootloader.bin");
            part = ESP_APP_ASSETS("custom/c5/partitions.bin");
            firm = ESP_APP_ASSETS("custom/c5/firmware.bin");
            if(storage_file_exists(app->storage, ESP_APP_ASSETS("custom/c5/boot_app0.bin"))) {
                app0 = ESP_APP_ASSETS("custom/c5/boot_app0.bin");
            }
            break;

        default:
            consumed = false;
            return consumed;
        }

        scene_manager_set_scene_state(app->scene_manager, EspFlasherSceneQuick, event.event);
        memset(app->selected_flash_options, 0, sizeof(app->selected_flash_options));
        app->bin_file_path_boot[0] = '\0';
        app->bin_file_path_part[0] = '\0';
        app->bin_file_path_nvs[0] = '\0';
        app->bin_file_path_boot_app0[0] = '\0';
        app->bin_file_path_app_a[0] = '\0';
        app->bin_file_path_app_b[0] = '\0';
        app->bin_file_path_custom[0] = '\0';

        // The worker takes every address from custom_slot_addrs, so fill in the standard
        // layout. The bootloader offset is re-derived from the connected chip.
        app->expected_chip = chip;
        app->boot_addr_manually_set = false;
        app->custom_slot_addrs[SelectedFlashBoot] = esp_flasher_boot_offset(chip);
        app->custom_slot_addrs[SelectedFlashPart] = ESP_ADDR_PART;
        app->custom_slot_addrs[SelectedFlashNvs] = ESP_ADDR_NVS;
        app->custom_slot_addrs[SelectedFlashBootApp0] = ESP_ADDR_BOOT_APP0;
        app->custom_slot_addrs[SelectedFlashAppA] = ESP_ADDR_APP_A;
        app->custom_slot_addrs[SelectedFlashAppB] = ESP_ADDR_APP_B;
        app->custom_slot_addrs[SelectedFlashCustom] = 0x0;

        // The firmware's own partition table is the authority for where app and otadata go
        // (Marauder 1.17 puts app1 at 0x1F0000 / 0x340000, not the old 0x150000).
        EspFlasherPartitionInfo info;
        if(part && esp_flasher_partition_parse(app->storage, part, &info, NULL, 0)) {
            for(int i = 0; i < NUM_FLASH_OPTIONS; i++) {
                if(info.slot_addrs[i]) app->custom_slot_addrs[i] = info.slot_addrs[i];
            }
        }

        esp_flasher_scene_quick_set_path(
            &app->selected_flash_options[SelectedFlashBoot],
            app->bin_file_path_boot,
            sizeof(app->bin_file_path_boot),
            boot);
        esp_flasher_scene_quick_set_path(
            &app->selected_flash_options[SelectedFlashPart],
            app->bin_file_path_part,
            sizeof(app->bin_file_path_part),
            part);
        esp_flasher_scene_quick_set_path(
            &app->selected_flash_options[SelectedFlashBootApp0],
            app->bin_file_path_boot_app0,
            sizeof(app->bin_file_path_boot_app0),
            app0);
        esp_flasher_scene_quick_set_path(
            &app->selected_flash_options[SelectedFlashAppA],
            app->bin_file_path_app_a,
            sizeof(app->bin_file_path_app_a),
            firm);

        app->reset = false;
        app->boot = false;
        app->switch_fw = SwitchNotSet;
        app->quickflash = true;
        app->turbospeed = true;
        scene_manager_next_scene(app->scene_manager, EspFlasherSceneConsoleOutput);
    } else if(event.type == SceneManagerEventTypeBack) {
        // Leaving a firmware list: highlight its chip in the chip list underneath.
        // From the chip list itself the state no longer matters.
        const uint32_t state =
            scene_manager_get_scene_state(app->scene_manager, EspFlasherSceneQuick);
        scene_manager_set_scene_state(
            app->scene_manager, EspFlasherSceneQuick, esp_flasher_scene_quick_chip_of(state));
    }

    return consumed;
}

void esp_flasher_scene_quick_on_exit(void* context) {
    furi_assert(context);

    EspFlasherApp* app = context;
    submenu_reset(app->submenu);
}
