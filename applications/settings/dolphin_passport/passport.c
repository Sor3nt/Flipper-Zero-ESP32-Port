#include <furi.h>
#include <furi_hal_version.h>

#include <gui/gui.h>
#include <dolphin/dolphin.h>
#include <dolphin/helpers/dolphin_state.h>

#include <assets_icons.h>
#include <asset_packs.h>

#define MOODS_TOTAL  3
#define BUTTHURT_MAX 3

static const Icon* const portrait_happy[BUTTHURT_MAX] = {
    &I_passport_happy1_46x49,
    &I_passport_happy2_46x49,
    &I_passport_happy3_46x49};
static const Icon* const portrait_ok[BUTTHURT_MAX] = {
    &I_passport_okay1_46x49,
    &I_passport_okay2_46x49,
    &I_passport_okay3_46x49};
static const Icon* const portrait_bad[BUTTHURT_MAX] = {
    &I_passport_bad1_46x49,
    &I_passport_bad2_46x49,
    &I_passport_bad3_46x49};

static const Icon* const* portraits[MOODS_TOTAL] = {portrait_happy, portrait_ok, portrait_bad};

static void input_callback(InputEvent* input, void* ctx) {
    FuriSemaphore* semaphore = ctx;

    if((input->type == InputTypeShort) && (input->key == InputKeyBack)) {
        furi_semaphore_release(semaphore);
    }
}

static void render_callback(Canvas* canvas, void* ctx) {
    DolphinStats* stats = ctx;

    char level_str[20];
    char mood_str[32];
    uint8_t mood = 0;

    if(stats->butthurt <= 4) {
        mood = 0;
        snprintf(mood_str, 20, "Mood: Happy");
    } else if(stats->butthurt <= 9) {
        mood = 1;
        snprintf(mood_str, 20, "Mood: Ok");
    } else {
        mood = 2;
        snprintf(mood_str, 20, "Mood: Angry");
    }

    uint32_t xp_progress = 0;
    uint32_t xp_to_levelup = dolphin_state_xp_to_levelup(stats->icounter);
    uint32_t xp_above_last_levelup = dolphin_state_xp_above_last_levelup(stats->icounter);
    uint32_t xp_for_current_level = xp_to_levelup + xp_above_last_levelup;
    if(stats->level == 3) {
        xp_progress = 0;
    } else {
        xp_progress = xp_to_levelup * 64 / xp_for_current_level;
    }

    furi_assert((stats->level > 0) && (stats->level <= 3));
    const Icon* portrait = portraits[mood][stats->level - 1];
    const char* my_name = furi_hal_version_get_name_ptr();
    snprintf(level_str, 20, "Level: %hu", stats->level);

    /* Asset pack with a full passport background (Momentum layout). */
    const Icon* background = asset_packs_get_passport_background();
    if(background) {
        char xp_str[24];
        canvas_draw_icon(canvas, 0, 0, background);
        canvas_draw_icon(canvas, 11, 2, portrait);
        canvas_draw_str(canvas, 59, 10, my_name ? my_name : "Unknown");
        canvas_draw_str(canvas, 59, 22, mood_str);
        canvas_draw_str(canvas, 59, 34, level_str);
        if(stats->level == 3) {
            snprintf(xp_str, sizeof(xp_str), "Max Level!");
        } else {
            snprintf(
                xp_str,
                sizeof(xp_str),
                "%lu/%lu",
                (unsigned long)xp_above_last_levelup,
                (unsigned long)xp_for_current_level);
        }
        canvas_set_font(canvas, FontBatteryPercent);
        canvas_draw_str(canvas, 59, 42, xp_str);
        canvas_set_font(canvas, FontSecondary);
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_box(canvas, 123 - xp_progress, 45, xp_progress + (xp_progress > 0), 5);
        canvas_set_color(canvas, ColorBlack);
        return;
    }

    // multipass
    canvas_draw_icon(canvas, 0, 0, &I_passport_left_6x46);
    canvas_draw_icon(canvas, 0, 46, &I_passport_bottom_128x18);
    canvas_draw_line(canvas, 6, 0, 125, 0);
    canvas_draw_line(canvas, 127, 2, 127, 47);
    canvas_draw_dot(canvas, 126, 1);

    // portrait
    canvas_draw_icon(canvas, 9, 5, portrait);
    canvas_draw_line(canvas, 58, 16, 123, 16);
    canvas_draw_line(canvas, 58, 30, 123, 30);
    canvas_draw_line(canvas, 58, 44, 123, 44);

    canvas_draw_str(canvas, 58, 12, my_name ? my_name : "Unknown");
    canvas_draw_str(canvas, 58, 26, mood_str);
    canvas_draw_str(canvas, 58, 40, level_str);

    canvas_set_color(canvas, ColorWhite);
    canvas_draw_box(canvas, 123 - xp_progress, 47, xp_progress + 1, 6);
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_line(canvas, 123, 47, 123, 52);
}

int32_t passport_app(void* p) {
    UNUSED(p);
    FuriSemaphore* semaphore = furi_semaphore_alloc(1, 0);
    ViewPort* view_port = view_port_alloc();

    Dolphin* dolphin = furi_record_open(RECORD_DOLPHIN);
    DolphinStats stats = dolphin_stats(dolphin);
    furi_record_close(RECORD_DOLPHIN);
    view_port_draw_callback_set(view_port, render_callback, &stats);
    view_port_input_callback_set(view_port, input_callback, semaphore);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);
    view_port_update(view_port);

    furi_check(furi_semaphore_acquire(semaphore, FuriWaitForever) == FuriStatusOk);

    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_record_close(RECORD_GUI);
    furi_semaphore_free(semaphore);

    return 0;
}
