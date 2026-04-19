#include "flipper.h"
#include <furi.h>
#include <furi_hal_version.h>
#include <furi_hal_memory.h>
#include <furi_hal_rtc.h>
#include <version.h>

#include <freertos/FreeRTOS.h>

#define TAG "Flipper"

static void flipper_print_version(const char* target, const Version* version) {
    if(version) {
        FURI_LOG_I(
            TAG,
            "\r\n\t%s version:\t%s\r\n"
            "\tBuild date:\t\t%s\r\n"
            "\tGit Commit:\t\t%s (%s)%s\r\n"
            "\tGit Branch:\t\t%s",
            target,
            version_get_version(version),
            version_get_builddate(version),
            version_get_githash(version),
            version_get_gitbranchnum(version),
            version_get_dirty_flag(version) ? " (dirty)" : "",
            version_get_gitbranch(version));
    } else {
        FURI_LOG_I(TAG, "No build info for %s", target);
    }
}

void flipper_init(void) {
    flipper_print_version("Firmware", furi_hal_version_get_firmware_version());

    FURI_LOG_I(TAG, "Boot mode %d, ESP32 port", furi_hal_rtc_get_boot_mode());

    FURI_LOG_I(TAG, "Startup complete");
}

// ESP32: vApplicationGetIdleTaskMemory and vApplicationGetTimerTaskMemory
// are not needed - ESP-IDF handles this internally
