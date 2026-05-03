/* Wolf3D — Flipper-Frontend
 *
 * Liefert Lifecycle (Records, Display-Takeover, Input-PubSub, Asset-Check),
 * ruft dann die Wolf4SDL-Entry-Point-Routine `wolf_main()` (umbenannt von
 * `main()` per `#define main wolf_main` in SDL.h).
 *
 * Pattern weitestgehend von applications/main/doom/doom_app.c kopiert.
 */

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_display.h>
#include <gui/gui.h>
#include <input/input.h>
#include <storage/storage.h>
#include <dialogs/dialogs.h>

#include <bt/bt_service/bt.h>
#include <esp_system.h>
#include <esp_bt.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_ops.h>
#include <furi_hal_spi_bus.h>
#include <furi_hal_speaker.h>

#include "wolf3d_glue.h"

#define TAG "Wolf3D"

#define WOLF3D_DATA_DIR "/ext/apps_data/wolf3d"
#define WOLF3D_VSWAP    WOLF3D_DATA_DIR "/VSWAP.WL1"

extern int wolf_main(int argc, char* argv[]); /* Wolf4SDLs main(), siehe SDL.h */

static bool wolf3d_assets_present(void) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FileInfo info;
    FS_Error err = storage_common_stat(storage, WOLF3D_VSWAP, &info);
    furi_record_close(RECORD_STORAGE);
    return err == FSE_OK && (info.flags & FSF_DIRECTORY) == 0;
}

int32_t wolf3d_app(void* p) {
    UNUSED(p);

    Gui* gui = furi_record_open(RECORD_GUI);
    FuriPubSub* input_events = furi_record_open(RECORD_INPUT_EVENTS);

    if(!wolf3d_assets_present()) {
        DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
        DialogMessage* msg = dialog_message_alloc();
        dialog_message_set_header(msg, "Wolf3D", 64, 2, AlignCenter, AlignTop);
        dialog_message_set_text(
            msg,
            "Copy WL1 files to:\n/ext/apps_data/wolf3d/",
            64, 32, AlignCenter, AlignCenter);
        dialog_message_set_buttons(msg, NULL, NULL, "OK");
        dialog_message_show(dialogs, msg);
        dialog_message_free(msg);
        furi_record_close(RECORD_DIALOGS);

        furi_record_close(RECORD_INPUT_EVENTS);
        furi_record_close(RECORD_GUI);
        return 0;
    }

    /* Display-Takeover & Input-Wiring an die Glue-Schicht delegieren. */
    wolf3d_glue_setup(gui, input_events);

    /* Boot-Splash so der Bildschirm während Asset-Lade nicht schwarz ist. */
    wolf3d_glue_show_splash();

    /* Speaker-HAL freigeben — Wolf3D will I2S_NUM_0 selbst. (TODO Stage 3) */
    furi_hal_speaker_deinit();

    /* BT-Teardown — gibt ~80 KB DRAM frei. Irreversibel; weil wir am Ende
     * via esp_restart() reboot, ist das OK. (Pattern aus Doom-Port.) */
    FURI_LOG_I(TAG, "Free DRAM pre-BT: %zu", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    Bt* bt = furi_record_open(RECORD_BT);
    bt_stop_stack(bt);
    furi_record_close(RECORD_BT);
    esp_bt_controller_mem_release(ESP_BT_MODE_BLE);

    FURI_LOG_I(TAG,
               "Free DRAM before main: %zu PSRAM: %zu",
               heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* Wolf4SDL-Entry. CheckParameters / CheckForEpisodes / InitGame / DemoLoop.
     * Beim Verlassen der DemoLoop ruft Wolf4SDL `Quit()` → unser longjmp()
     * holt uns zurück (siehe wolf3d_glue.cpp::Quit). */
    /* --res 320 200 → scaleFactor=1, screenBuffer wird 320×200 (statt 640×400). */
    char arg0[] = "wolf3d";
    char arg1[] = "--res";
    char arg2[] = "320";
    char arg3[] = "200";
    char* argv[] = {arg0, arg1, arg2, arg3, NULL};
    if(wolf3d_glue_set_quit_jmp() == 0) {
        wolf_main(4, argv);
    }
    /* hier nach `Quit()` */

    wolf3d_glue_teardown();

    furi_record_close(RECORD_INPUT_EVENTS);
    furi_record_close(RECORD_GUI);

    FURI_LOG_I(TAG, "exit -> esp_restart()");
    esp_restart();
    return 0;
}
