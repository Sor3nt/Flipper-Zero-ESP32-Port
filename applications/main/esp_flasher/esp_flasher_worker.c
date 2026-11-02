#include "esp_flasher_worker.h"
#include "esp_flasher_partition.h"

#include <esp_timer.h>
#include <stdarg.h>

#define TAG "EspFlasherWorker"

#define ESP_FLASHER_BLOCK_SIZE    (1024)
#define ESP_FLASHER_PROGRESS_STEP (64 * 1024)

/* Connect retries: ~2 s when we drive the target into download mode ourselves,
 * ~10 s when the user has to hold BOOT and tap RESET by hand. */
#define ESP_FLASHER_CONNECT_TRIALS_AUTO   (10)
#define ESP_FLASHER_CONNECT_TRIALS_MANUAL (50)

/* ESP32-C5 ROM baud quirk, from esptool targets/esp32c5.py: the ROM may assume a 40 MHz
 * crystal on a 48 MHz part (or the reverse) and then programs CHANGE_BAUDRATE ~20% off. */
#define ESP32C5_PCR_SYSCLK_CONF_REG    (0x60096110UL)
#define ESP32C5_PCR_SYSCLK_XTAL_FREQ_V (0x7FUL << 24)
#define ESP32C5_PCR_SYSCLK_XTAL_FREQ_S (24)
#define ESP32C5_UART_CLKDIV_REG        (0x60000014UL)
#define ESP_UART_CLKDIV_MASK           (0xFFFFFUL)

static EspFlasherApp* s_app;
static int64_t s_timer_deadline_us;

static void esp_flasher_printf(const char* format, ...) __attribute__((format(printf, 1, 2)));

static void esp_flasher_printf(const char* format, ...) {
    char buf[192];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    loader_port_debug_print(buf);
}

static const char* esp_flasher_error_name(esp_loader_error_t err) {
    switch(err) {
    case ESP_LOADER_SUCCESS:
        return "OK";
    case ESP_LOADER_ERROR_TIMEOUT:
        return "timeout";
    case ESP_LOADER_ERROR_IMAGE_SIZE:
        return "image larger than flash";
    case ESP_LOADER_ERROR_INVALID_MD5:
        return "MD5 mismatch";
    case ESP_LOADER_ERROR_INVALID_PARAM:
        return "invalid parameter";
    case ESP_LOADER_ERROR_INVALID_TARGET:
        return "unknown target";
    case ESP_LOADER_ERROR_UNSUPPORTED_CHIP:
        return "unsupported chip";
    case ESP_LOADER_ERROR_UNSUPPORTED_FUNC:
        return "unsupported function";
    case ESP_LOADER_ERROR_INVALID_RESPONSE:
        return "invalid response";
    default:
        return "failed";
    }
}

static esp_loader_error_t esp_flasher_flash_file(EspFlasherApp* app, const char* path, uint32_t addr) {
    esp_loader_error_t err = ESP_LOADER_ERROR_FAIL;
    /* Heap, not static: a built-in app must not hold internal RAM while it is closed.
     * esp_loader_flash_write() pads the tail of this buffer in place. */
    uint8_t* payload = malloc(ESP_FLASHER_BLOCK_SIZE);
    File* file = storage_file_alloc(app->storage);

    do {
        if(!payload) {
            loader_port_debug_print("Out of memory\n");
            break;
        }
        if(!storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
            esp_flasher_printf("Cannot open %s\n", path);
            break;
        }

        const uint64_t file_size = storage_file_size(file);
        if(file_size == 0 || file_size > UINT32_MAX - 3) {
            esp_flasher_printf("Invalid file size: %s\n", path);
            break;
        }
        /* FLASH_BEGIN needs a 4-byte aligned size; the last block is 0xFF padded anyway. */
        const uint32_t image_size = ((uint32_t)file_size + 3) & ~3UL;
        if((uint64_t)addr + image_size > UINT32_MAX) {
            esp_flasher_printf("Invalid flash range at 0x%lx: %s\n", addr, path);
            err = ESP_LOADER_ERROR_INVALID_PARAM;
            break;
        }

        loader_port_debug_print("Erasing flash...this may take a while\n");
        err = esp_loader_flash_start(addr, image_size, ESP_FLASHER_BLOCK_SIZE);
        if(err != ESP_LOADER_SUCCESS) {
            esp_flasher_printf("Erasing flash failed: %s\n", esp_flasher_error_name(err));
            break;
        }

        loader_port_debug_print("Start programming\n");
        uint32_t remaining = (uint32_t)file_size;
        uint32_t written = 0;
        uint32_t next_progress = ESP_FLASHER_PROGRESS_STEP;
        while(remaining > 0) {
            const size_t to_read = MIN(remaining, ESP_FLASHER_BLOCK_SIZE);
            const size_t read = storage_file_read(file, payload, to_read);
            if(read != to_read) {
                esp_flasher_printf("SD read error at %lu\n", written);
                err = ESP_LOADER_ERROR_FAIL;
                break;
            }
            err = esp_loader_flash_write(payload, read);
            if(err != ESP_LOADER_SUCCESS) {
                esp_flasher_printf(
                    "Packet at %lu could not be written: %s\n",
                    written,
                    esp_flasher_error_name(err));
                break;
            }
            remaining -= read;
            written += read;
            if(written >= next_progress || remaining == 0) {
                esp_flasher_printf(
                    "%lu%% (%lu/%lu KB)\n",
                    (uint32_t)((uint64_t)written * 100 / file_size),
                    written / 1024,
                    (uint32_t)(file_size / 1024));
                next_progress += ESP_FLASHER_PROGRESS_STEP;
            }
        }
        if(err != ESP_LOADER_SUCCESS) break;

        if(esp_loader_get_target() == ESP8266_CHIP) {
            loader_port_debug_print("Finished programming (no MD5 on ESP8266 ROM)\n");
            break;
        }
        loader_port_debug_print("Verifying...\n");
        err = esp_loader_flash_verify();
        if(err == ESP_LOADER_SUCCESS) {
            loader_port_debug_print("Finished programming, MD5 OK\n");
        } else {
            esp_flasher_printf("Verify failed: %s\n", esp_flasher_error_name(err));
        }
    } while(false);

    storage_file_close(file);
    storage_file_free(file);
    free(payload);
    return err;
}

// This in-app FW switch "exploits" the otadata (boot_app0)
// - the first four bytes of each array are the counter and the last four bytes are just a CRC of that counter
// - the bootloader will just boot whichever app has the highest counter in the otadata partition
//   so we'll just pick 1 for A, and then B will use either 0 or 2 depending on whether it's the slot in use

#define MAGIC_PAYLOAD_SIZE (32)

static const uint8_t magic_payload_app_a[MAGIC_PAYLOAD_SIZE] = {
    0x01, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x9a, 0x98, 0x43, 0x47};

static const uint8_t magic_payload_app_b_unset[MAGIC_PAYLOAD_SIZE] = {
    0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

static const uint8_t magic_payload_app_b_set[MAGIC_PAYLOAD_SIZE] = {
    0x02, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x74, 0x37, 0xf6, 0x55};

static esp_loader_error_t esp_flasher_write_otadata(uint32_t addr, const uint8_t* payload) {
    esp_loader_error_t err = esp_loader_flash_start(addr, MAGIC_PAYLOAD_SIZE, MAGIC_PAYLOAD_SIZE);
    if(err != ESP_LOADER_SUCCESS) {
        esp_flasher_printf("Erasing flash failed: %s\n", esp_flasher_error_name(err));
        return err;
    }
    /* flash_write pads in place, so hand it a writable copy */
    uint8_t buf[MAGIC_PAYLOAD_SIZE];
    memcpy(buf, payload, sizeof(buf));
    err = esp_loader_flash_write(buf, sizeof(buf));
    if(err != ESP_LOADER_SUCCESS) {
        esp_flasher_printf("Packet could not be written: %s\n", esp_flasher_error_name(err));
    }
    return err;
}

static bool esp_flasher_switch_fw(EspFlasherApp* app) {
    loader_port_debug_print("Setting flags for firmware A\n");
    if(esp_flasher_write_otadata(
           ESP_ADDR_BOOT_APP0 + ESP_ADDR_OTADATA_OFFSET_APP_A, magic_payload_app_a) !=
       ESP_LOADER_SUCCESS) {
        return false;
    }

    loader_port_debug_print("Setting flags for firmware B\n");
    const uint8_t* payload_b = app->switch_fw == SwitchToFirmwareB ? magic_payload_app_b_set :
                                                                     magic_payload_app_b_unset;
    if(esp_flasher_write_otadata(ESP_ADDR_BOOT_APP0 + ESP_ADDR_OTADATA_OFFSET_APP_B, payload_b) !=
       ESP_LOADER_SUCCESS) {
        return false;
    }

    loader_port_debug_print("Finished programming\n");
    return true;
}

typedef struct {
    SelectedFlashOptions selected;
    const char* description;
    const char* path;
} FlashItem;

static void esp_flasher_get_items(EspFlasherApp* app, FlashItem items[NUM_FLASH_OPTIONS]) {
    const FlashItem all[NUM_FLASH_OPTIONS] = {
        {SelectedFlashBoot, "bootloader", app->bin_file_path_boot},
        {SelectedFlashPart, "partition table", app->bin_file_path_part},
        {SelectedFlashNvs, "NVS", app->bin_file_path_nvs},
        {SelectedFlashBootApp0, "boot_app0", app->bin_file_path_boot_app0},
        {SelectedFlashAppA, "firmware A", app->bin_file_path_app_a},
        {SelectedFlashAppB, "firmware B", app->bin_file_path_app_b},
        {SelectedFlashCustom, "custom data", app->bin_file_path_custom},
    };
    memcpy(items, all, sizeof(all));
}

/* Checked before the target is touched, so a missing SD file never leaves it in download mode. */
static bool esp_flasher_check_files(EspFlasherApp* app) {
    FlashItem items[NUM_FLASH_OPTIONS];
    esp_flasher_get_items(app, items);

    bool ok = true;
    int selected = 0;
    for(size_t i = 0; i < NUM_FLASH_OPTIONS; i++) {
        if(!app->selected_flash_options[items[i].selected]) continue;
        selected++;
        if(items[i].path[0] == '\0' || !storage_file_exists(app->storage, items[i].path)) {
            if(ok) loader_port_debug_print("Missing file(s):\n");
            esp_flasher_printf("%s\n", items[i].path[0] ? items[i].path : items[i].description);
            ok = false;
        }
    }
    if(!ok) {
        loader_port_debug_print(
            "Copy the firmware to the SD card (see About) and try again.\n");
    } else if(selected == 0) {
        loader_port_debug_print("Nothing selected to flash.\n");
        ok = false;
    }
    return ok;
}

static bool esp_flasher_flash_all_files(EspFlasherApp* app) {
    FlashItem items[NUM_FLASH_OPTIONS];
    esp_flasher_get_items(app, items);

    const target_chip_t chip = esp_loader_get_target();
    int num_steps = 0;
    for(size_t i = 0; i < NUM_FLASH_OPTIONS; i++) {
        if(app->selected_flash_options[items[i].selected]) num_steps++;
    }

    int current_step = 1;
    for(size_t i = 0; i < NUM_FLASH_OPTIONS; i++) {
        if(!app->selected_flash_options[items[i].selected]) continue;

        uint32_t addr = app->custom_slot_addrs[items[i].selected];
        if(items[i].selected == SelectedFlashBoot && !app->boot_addr_manually_set) {
            const uint32_t chip_addr = esp_flasher_boot_offset(chip);
            if(chip_addr != addr) {
                esp_flasher_printf(
                    "%s bootloader goes to 0x%lx\n", esp_flasher_chip_name(chip), chip_addr);
                addr = chip_addr;
            }
        }

        esp_flasher_printf(
            "Flashing %s (%d/%d) to address 0x%lx\n",
            items[i].description,
            current_step++,
            num_steps,
            addr);
        if(esp_flasher_flash_file(app, items[i].path, addr) != ESP_LOADER_SUCCESS) return false;
    }
    return true;
}

/* A layout bigger than the chip's flash writes fine but never boots (Marauder C5 needs 8 MB). */
static bool esp_flasher_check_flash_size(EspFlasherApp* app) {
    if(!app->selected_flash_options[SelectedFlashPart]) return true;

    EspFlasherPartitionInfo info;
    if(!esp_flasher_partition_parse(app->storage, app->bin_file_path_part, &info, NULL, 0)) {
        loader_port_debug_print("Partition table is invalid or not recognised. Nothing was written.\n");
        return false;
    }

    uint32_t flash_size = 0;
    if(esp_loader_flash_detect_size(&flash_size) != ESP_LOADER_SUCCESS) {
        loader_port_debug_print("Warning: flash size not detected, layout not checked\n");
        return true;
    }
    esp_flasher_printf("Target flash: %lu MB\n", flash_size / (1024 * 1024));

    if(info.end > flash_size) {
        esp_flasher_printf(
            "Firmware needs %lu KB of flash, target has %lu KB. Nothing was written.\n",
            info.end / 1024,
            flash_size / 1024);
        return false;
    }
    return true;
}

/* Baud rate to request from the ROM so the target really ends up at `baud`. */
static uint32_t esp_flasher_rom_baud(uint32_t baud) {
    if(esp_loader_get_target() != ESP32C5_CHIP) return baud;

    uint32_t pcr = 0;
    uint32_t clkdiv = 0;
    if(esp_loader_read_register(ESP32C5_PCR_SYSCLK_CONF_REG, &pcr) != ESP_LOADER_SUCCESS ||
       esp_loader_read_register(ESP32C5_UART_CLKDIV_REG, &clkdiv) != ESP_LOADER_SUCCESS) {
        loader_port_debug_print("ESP32-C5: crystal check failed, baud not corrected\n");
        return baud;
    }

    const uint32_t rom_mhz = (pcr & ESP32C5_PCR_SYSCLK_XTAL_FREQ_V) >>
                             ESP32C5_PCR_SYSCLK_XTAL_FREQ_S;
    /* The ROM auto-bauded to our 115200, so divider * baud recovers its UART clock. */
    const uint32_t est_mhz =
        (uint32_t)(((uint64_t)BAUDRATE * (clkdiv & ESP_UART_CLKDIV_MASK)) / 1000000ULL);
    const uint32_t xtal_mhz = est_mhz > 45 ? 48 : (est_mhz > 33 ? 40 : 26);
    esp_flasher_printf("ESP32-C5: crystal %lu MHz, ROM expects %lu MHz\n", xtal_mhz, rom_mhz);

    if(xtal_mhz == 48 && rom_mhz == 40) return baud * 40 / 48;
    if(xtal_mhz == 40 && rom_mhz == 48) return baud * 48 / 40;
    return baud;
}

static void esp_flasher_flash_job(EspFlasherApp* app) {
    const EspFlasherResetMode mode = app->settings.reset_mode;
    esp_loader_error_t err;
    bool success = false;
    bool baud_changed = false;

    // blue LED for the duration of the flash
    notification_message(app->notification, &sequence_set_only_blue_255);
    esp_flasher_uart_claim_rx(app->uart);

    esp_loader_connect_args_t connect_config = ESP_LOADER_CONNECT_DEFAULT();
    if(esp_flasher_reset_enter_bootloader(mode)) {
        loader_port_debug_print("Entered bootloader via EN/IO0\n");
        connect_config.trials = ESP_FLASHER_CONNECT_TRIALS_AUTO;
    } else {
        loader_port_debug_print("Hold BOOT and tap RESET on the target now...\n");
        connect_config.trials = ESP_FLASHER_CONNECT_TRIALS_MANUAL;
    }
    esp_flasher_uart_flush_rx(app->uart);

    do {
        loader_port_debug_print("Connecting\n");
        err = esp_loader_connect(&connect_config);
        if(err != ESP_LOADER_SUCCESS) {
            esp_flasher_printf(
                "Cannot connect to target: %s\n"
                "Check TX->RX, RX->TX, GND and that the target is in download mode.\n",
                esp_flasher_error_name(err));
            break;
        }

        const target_chip_t chip = esp_loader_get_target();
        esp_flasher_printf("Connected: %s\n", esp_flasher_chip_name(chip));
        if(app->expected_chip != ESP_UNKNOWN_CHIP && chip != app->expected_chip) {
            esp_flasher_printf(
                "Wrong chip! Firmware is for %s. Nothing was written.\n",
                esp_flasher_chip_name(app->expected_chip));
            break;
        }
        if(app->switch_fw == SwitchNotSet && !esp_flasher_check_flash_size(app)) break;

        if(app->turbospeed && chip != ESP8266_CHIP) {
            const uint32_t fast = esp_flasher_fast_baud_value(app->settings.fast_baud);
            const uint32_t rom_baud = esp_flasher_rom_baud(fast);
            esp_flasher_printf("Increasing speed to %lu baud\n", fast);
            err = esp_loader_change_transmission_rate(rom_baud);
            if(err != ESP_LOADER_SUCCESS) {
                esp_flasher_printf(
                    "Cannot change baud rate: %s. Use the slow mode or a lower Fast baud.\n",
                    esp_flasher_error_name(err));
                break;
            }
            baud_changed = true;
            if(!esp_flasher_uart_set_br(app->uart, fast)) {
                loader_port_debug_print("Cannot change the local baud rate\n");
                break;
            }
            // get rid of garbage sent during the baud rate change
            furi_delay_ms(50);
            esp_flasher_uart_flush_rx(app->uart);
        }

        const uint32_t start_tick = furi_get_tick();
        success = app->switch_fw != SwitchNotSet ? esp_flasher_switch_fw(app) :
                                                   esp_flasher_flash_all_files(app);
        esp_flasher_printf(
            "Flash took: %lus\n", (furi_get_tick() - start_tick) / furi_kernel_get_tick_frequency());
    } while(false);

    if(baud_changed) {
        loader_port_debug_print("Restoring transmission rate\n");
        esp_flasher_uart_set_br(app->uart, BAUDRATE);
    }

    if(!success) {
        loader_port_debug_print("Flash failed. Reset the target and try again.\n");
    } else {
        if(esp_flasher_reset_target(mode)) {
            loader_port_debug_print("Done flashing, target reset.\n");
        } else {
            loader_port_debug_print("Done flashing. Press RESET on the target.\n");
        }
    }

    esp_flasher_uart_flush_rx(app->uart);
    esp_flasher_uart_release_rx(app->uart);
    notification_message(app->notification, &sequence_reset_blue);
    notification_message(app->notification, success ? &sequence_success : &sequence_error);
}

static int32_t esp_flasher_worker_thread(void* context) {
    EspFlasherApp* app = context;
    const EspFlasherResetMode mode = app->settings.reset_mode;
    const bool flash_job = app->quickflash || !(app->reset || app->boot);

    if(flash_job) {
        if(app->switch_fw != SwitchNotSet || esp_flasher_check_files(app)) {
            esp_flasher_flash_job(app);
        }
    } else if(app->reset) {
        if(esp_flasher_reset_target(mode)) {
            loader_port_debug_print("Resetting board\n");
        } else {
            loader_port_debug_print(
                esp_flasher_reset_available() ?
                    "Reset pins are off: enable them in Settings or press RESET.\n" :
                    "This board has no reset pins: press RESET on the target.\n");
        }
    } else if(app->boot) {
        if(esp_flasher_reset_enter_bootloader(mode)) {
            loader_port_debug_print("Entered bootloader, target is in download mode\n");
        } else {
            loader_port_debug_print(
                esp_flasher_reset_available() ?
                    "Reset pins are off: enable them in Settings or hold BOOT + tap RESET.\n" :
                    "This board has no reset pins: hold BOOT and tap RESET on the target.\n");
        }
    }

    app->reset = false;
    app->boot = false;
    app->quickflash = false;
    app->switch_fw = SwitchNotSet;
    app->flash_worker_busy = false;
    return 0;
}

void esp_flasher_worker_start_thread(EspFlasherApp* app) {
    s_app = app;
    /* Busy before the thread exists, so Back cannot slip in between steps. */
    app->flash_worker_busy = true;
    app->flash_worker =
        furi_thread_alloc_ex("EspFlasherFlashWorker", 6 * 1024, esp_flasher_worker_thread, app);
    furi_thread_start(app->flash_worker);
}

void esp_flasher_worker_stop_thread(EspFlasherApp* app) {
    furi_thread_join(app->flash_worker);
    furi_thread_free(app->flash_worker);
    app->flash_worker = NULL;
    if(s_app == app) s_app = NULL;
}

void esp_flasher_worker_handle_rx_data_cb(uint8_t* buf, size_t len, void* context) {
    esp_flasher_console_append(context, buf, len);
}

// esp-serial-flasher port layer

esp_loader_error_t loader_port_read(uint8_t* data, uint16_t size, uint32_t timeout) {
    if(!s_app || !s_app->uart) return ESP_LOADER_ERROR_FAIL;
    const size_t read = esp_flasher_uart_read(s_app->uart, data, size, timeout);
    return read == size ? ESP_LOADER_SUCCESS : ESP_LOADER_ERROR_TIMEOUT;
}

esp_loader_error_t loader_port_write(const uint8_t* data, uint16_t size, uint32_t timeout) {
    UNUSED(timeout);
    if(!s_app || !s_app->uart) return ESP_LOADER_ERROR_FAIL;
    return esp_flasher_uart_tx(s_app->uart, data, size) ? ESP_LOADER_SUCCESS :
                                                          ESP_LOADER_ERROR_FAIL;
}

esp_loader_error_t loader_port_change_transmission_rate(uint32_t transmission_rate) {
    if(!s_app || !s_app->uart) return ESP_LOADER_ERROR_FAIL;
    return esp_flasher_uart_set_br(s_app->uart, transmission_rate) ? ESP_LOADER_SUCCESS :
                                                                     ESP_LOADER_ERROR_FAIL;
}

void loader_port_reset_target(void) {
    if(s_app) esp_flasher_reset_target(s_app->settings.reset_mode);
}

void loader_port_enter_bootloader(void) {
    if(s_app) esp_flasher_reset_enter_bootloader(s_app->settings.reset_mode);
}

void loader_port_delay_ms(uint32_t ms) {
    furi_delay_ms(ms);
}

void loader_port_start_timer(uint32_t ms) {
    s_timer_deadline_us = esp_timer_get_time() + (int64_t)ms * 1000;
}

uint32_t loader_port_remaining_time(void) {
    const int64_t remaining_us = s_timer_deadline_us - esp_timer_get_time();
    return remaining_us > 0 ? (uint32_t)((remaining_us + 999) / 1000) : 0;
}

void loader_port_debug_print(const char* str) {
    if(!s_app || !str) return;
    const size_t len = strlen(str);
    FURI_LOG_D(TAG, "%s", str);
    esp_flasher_console_append(s_app, (const uint8_t*)str, len);
    // library messages come without a line break
    if(len && str[len - 1] != '\n') esp_flasher_console_append(s_app, (const uint8_t*)"\n", 1);
}
