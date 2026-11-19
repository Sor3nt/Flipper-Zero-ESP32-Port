#include "namechanger.h"
#include <furi_hal.h>
#include <furi_hal_version.h>
#include <cli/cli_vcp.h>
#include <bt/bt_service/bt.h>
#include <storage/storage.h>
#include <flipper_format/flipper_format.h>

#define TAG "NameChanger"

static bool namechanger_init() {
    Storage* storage = furi_record_open(RECORD_STORAGE);

    /* One check, no polling. storage_srv runs the boot mount, including every
     * retry, before it creates RECORD_STORAGE, and namechanger_on_system_start()
     * only gets here once that record exists. So the SD status is already
     * final. The old 11 x 250 ms poll could never change it. It only delayed
     * every later on_system_start hook (and the "All services started" line)
     * by ~2.75 s when there was no card. */
    if(storage_sd_status(storage) != FSE_OK) {
        furi_record_close(RECORD_STORAGE);
        return false;
    }

    FuriString* str = furi_string_alloc();
    FlipperFormat* file = flipper_format_file_alloc(storage);

    bool res = false;

    do {
        uint32_t version;
        if(!flipper_format_file_open_existing(file, NAMECHANGER_PATH)) break;
        if(!flipper_format_read_header(file, str, &version)) break;
        if(furi_string_cmp_str(str, NAMECHANGER_HEADER)) break;
        if(version != NAMECHANGER_VERSION) break;

        if(!flipper_format_read_string(file, "Name", str)) break;
        // Check for size
        size_t temp_string_size = furi_string_size(str);
        if(temp_string_size > (size_t)8) break;
        if(temp_string_size < (size_t)2) break;

        // Check for forbidden characters
        const char* name_ptr = furi_string_get_cstr(str);
        bool chars_check_failed = false;

        for(; *name_ptr; ++name_ptr) {
            const char c = *name_ptr;
            if((c < '0' || c > '9') && (c < 'A' || c > 'Z') && (c < 'a' || c > 'z')) {
                chars_check_failed = true;
                break;
            }
        }

        if(chars_check_failed) break;

        // If all checks was good we can set the name.
        // NB: call furi_hal_version_set_name() directly with the string — it
        // copies the name into all live fields AND updates the Version custom
        // name via refresh_names(). Going through version_set_custom_name(NULL,
        // …) is a no-op (it early-returns on a NULL Version*), which used to
        // leave the name at the eFuse-derived default.
        furi_hal_version_set_name(furi_string_get_cstr(str));

        /* Optional shell color (added when NVS persistence moved to SD). Older
         * files without the field simply keep the default color. */
        uint32_t color;
        if(flipper_format_read_uint32(file, "Color", &color, 1)) {
            furi_hal_version_set_hw_color((FuriHalVersionColor)color);
        }

        res = true;
    } while(false);

    flipper_format_free(file);
    furi_record_close(RECORD_STORAGE);
    furi_string_free(str);

    return res;
}

int32_t namechanger_on_system_start(void* p) {
    UNUSED(p);
    if(furi_hal_rtc_get_boot_mode() != FuriHalRtcBootModeNormal) {
        return 0;
    }

    // Wait for all required services to start and create their records
    uint8_t timeout = 0;
    while(!furi_record_exists(RECORD_CLI_VCP) || !furi_record_exists(RECORD_BT) ||
          !furi_record_exists(RECORD_STORAGE)) {
        timeout++;
        if(timeout > 250) {
            return 0;
        }
        furi_delay_ms(5);
    }

    // Hehe bad code now here, bad bad bad, very bad, bad example, dont take it, make it better

    if(namechanger_init()) {
        CliVcp* cli = furi_record_open(RECORD_CLI_VCP);
        cli_vcp_disable(cli);
        furi_delay_ms(2); // why i added delays here
        cli_vcp_enable(cli);
        furi_record_close(RECORD_CLI_VCP);

        furi_delay_ms(3);
        Bt* bt = furi_record_open(RECORD_BT);
        if(!bt_profile_restore_default(bt)) {
            //FURI_LOG_D(TAG, "Failed to touch bluetooth to name change");
        }
        furi_record_close(RECORD_BT);
        bt = NULL;
        furi_delay_ms(3);
    }

    return 0;
}
