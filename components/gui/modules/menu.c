#include "menu.h"

#include "../elements.h"
#include "../icon_animation_i.h"
#include "../icon_i.h"
#include "../canvas_i.h"
#include <assets_icons.h>
#include <furi.h>
#include <furi_hal.h>
#include <locale/locale.h>
#include <dolphin/dolphin.h>
#include <dolphin/helpers/dolphin_state.h>
#include <momentum_settings.h>
#include <m-array.h>
#include <saved_struct.h>

/* Centered/aligned scrollable text line, using the port's _str helper
 * (same parameters as Momentum's elements_scrollable_text_line_centered). */
static void menu_text_line_centered(
    Canvas* canvas,
    int32_t x,
    int32_t y,
    size_t width,
    FuriString* name,
    size_t scroll,
    bool ellipsis,
    bool centered) {
    elements_scrollable_text_line_str(
        canvas,
        (uint8_t)x,
        (uint8_t)y,
        (uint8_t)width,
        furi_string_get_cstr(name),
        scroll,
        ellipsis,
        centered);
}

#define MENU_STYLE_SETTINGS_PATH "/int/.menu.settings"
#define MENU_STYLE_SETTINGS_MAGIC (0x4D)
#define MENU_STYLE_SETTINGS_VER (1)

#define LOCK_SCREEN_SETTINGS_PATH "/int/.lock.screen"
#define LOCK_SCREEN_SETTINGS_MAGIC (0x4C)
#define LOCK_SCREEN_SETTINGS_VER (1)

static MenuStyle g_menu_style = MenuStyleList;
static bool g_menu_style_loaded = false;
static LockScreenStyle g_lock_screen_style = LockScreenStyleDefault;
static bool g_lock_screen_loaded = false;

static void menu_style_load(void) {
    MenuStyle loaded;
    bool valid = saved_struct_load(
                     MENU_STYLE_SETTINGS_PATH,
                     &loaded,
                     sizeof(loaded),
                     MENU_STYLE_SETTINGS_MAGIC,
                     MENU_STYLE_SETTINGS_VER) &&
                 ((int)loaded >= 0) && ((int)loaded < (int)MenuStyleCount);
    FURI_CRITICAL_ENTER();
    if(!g_menu_style_loaded && valid) g_menu_style = loaded;
    g_menu_style_loaded = true;
    FURI_CRITICAL_EXIT();
}

static void lock_screen_load(void) {
    LockScreenStyle loaded;
    bool valid = saved_struct_load(
                     LOCK_SCREEN_SETTINGS_PATH,
                     &loaded,
                     sizeof(loaded),
                     LOCK_SCREEN_SETTINGS_MAGIC,
                     LOCK_SCREEN_SETTINGS_VER) &&
                 ((int)loaded >= 0) && ((int)loaded < (int)LockScreenStyleCount);
    FURI_CRITICAL_ENTER();
    if(!g_lock_screen_loaded && valid) g_lock_screen_style = loaded;
    g_lock_screen_loaded = true;
    FURI_CRITICAL_EXIT();
}

void menu_set_style(MenuStyle style) {
    if(((int)style < 0) || ((int)style >= (int)MenuStyleCount)) {
        style = MenuStyleList;
    }
    FURI_CRITICAL_ENTER();
    g_menu_style = style;
    g_menu_style_loaded = true;
    FURI_CRITICAL_EXIT();
    saved_struct_save(
        MENU_STYLE_SETTINGS_PATH,
        &style,
        sizeof(style),
        MENU_STYLE_SETTINGS_MAGIC,
        MENU_STYLE_SETTINGS_VER);
}

MenuStyle menu_get_style(void) {
    bool loaded;
    /* Separate block so each FURI_CRITICAL_ENTER's __furi_critical_info has its
     * own scope (two in one scope would redefine it). */
    {
        FURI_CRITICAL_ENTER();
        loaded = g_menu_style_loaded;
        FURI_CRITICAL_EXIT();
    }
    if(!loaded) menu_style_load();
    MenuStyle style;
    {
        FURI_CRITICAL_ENTER();
        style = g_menu_style;
        FURI_CRITICAL_EXIT();
    }
    return style;
}

void lock_screen_set_style(LockScreenStyle style) {
    if(((int)style < 0) || ((int)style >= (int)LockScreenStyleCount)) {
        style = LockScreenStyleDefault;
    }
    FURI_CRITICAL_ENTER();
    g_lock_screen_style = style;
    g_lock_screen_loaded = true;
    FURI_CRITICAL_EXIT();
    saved_struct_save(
        LOCK_SCREEN_SETTINGS_PATH,
        &style,
        sizeof(style),
        LOCK_SCREEN_SETTINGS_MAGIC,
        LOCK_SCREEN_SETTINGS_VER);
}

LockScreenStyle lock_screen_get_style(void) {
    bool loaded;
    {
        FURI_CRITICAL_ENTER();
        loaded = g_lock_screen_loaded;
        FURI_CRITICAL_EXIT();
    }
    if(!loaded) {
        lock_screen_load();
    }
    LockScreenStyle style;
    {
        FURI_CRITICAL_ENTER();
        style = g_lock_screen_style;
        FURI_CRITICAL_EXIT();
    }
    return style;
}

struct Menu {
    View* view;
    FuriTimer* scroll_timer;
};

typedef struct {
    const char* label;
    IconAnimation* icon;
    uint32_t index;
    MenuItemCallback callback;
    void* callback_context;
} MenuItem;

ARRAY_DEF(MenuItemArray, MenuItem, M_POD_OPLIST); //-V658

#define M_OPL_MenuItemArray_t() ARRAY_OPLIST(MenuItemArray, M_POD_OPLIST)

typedef struct {
    MenuItemArray_t items;
    size_t position;
    size_t scroll_counter;
    size_t vertical_offset;
} MenuModel;

static void menu_process_up(Menu* menu);
static void menu_process_down(Menu* menu);
static void menu_process_left(Menu* menu);
static void menu_process_right(Menu* menu);
static void menu_process_ok(Menu* menu);
static void menu_set_position(Menu* menu, uint32_t position);

static void menu_get_name(MenuItem* item, FuriString* name, bool shorter) {
    furi_string_set(name, item->label);
    if(shorter) {
        if(!furi_string_cmp(name, "Momentum")) {
            furi_string_set(name, "MNTM");
            return;
        } else if(!furi_string_cmp(name, "125 kHz RFID")) {
            furi_string_set(name, "RFID");
            return;
        } else if(!furi_string_cmp(name, "Sub-GHz")) {
            furi_string_set(name, "SubGHz");
            return;
        }
    }
    if(furi_string_start_with_str(name, "[")) {
        size_t trim = furi_string_search_str(name, "] ", 1);
        if(trim != FURI_STRING_FAILURE) {
            furi_string_right(name, trim + 2);
        }
    }
}

static void menu_centered_icon(
    Canvas* canvas,
    MenuItem* item,
    size_t x,
    size_t y,
    size_t width,
    size_t height) {
    canvas_draw_icon_animation(
        canvas,
        x + (width - item->icon->icon->width) / 2,
        y + (height - item->icon->icon->height) / 2,
        item->icon);
}

static void menu_centered_icon_scaled(
    Canvas* canvas,
    MenuItem* item,
    size_t x,
    size_t y,
    size_t width,
    size_t height,
    size_t width_scale,
    size_t height_scale) {
    canvas_draw_icon_animation_ex(
        canvas,
        x + (width - item->icon->icon->width) / 2,
        y + (height - item->icon->icon->height) / 2,
        width_scale,
        height_scale,
        item->icon);
}

static size_t menu_scroll_counter(MenuModel* model, bool selected) {
    if(!selected) return 0;
    size_t scroll_counter = model->scroll_counter;
    if(scroll_counter > 0) {
        scroll_counter--;
    }
    return scroll_counter;
}

static void menu_draw_callback(Canvas* canvas, void* _model) {
    MenuModel* model = _model;

    canvas_clear(canvas);

    size_t position = model->position;
    size_t items_count = MenuItemArray_size(model->items);
    if(items_count) {
        MenuItem* item;
        size_t shift_position;
        FuriString* name = furi_string_alloc();
        switch(menu_get_style()) {
        case MenuStyleList: {
            for(uint8_t i = 0; i < 3; i++) {
                canvas_set_font(canvas, i == 1 ? FontPrimary : FontSecondary);
                shift_position = (position + items_count + i - 1) % items_count;
                item = MenuItemArray_get(model->items, shift_position);
                menu_centered_icon(canvas, item, 4, 3 + 22 * i, 14, 14);
                menu_get_name(item, name, false);
                size_t scroll_counter = menu_scroll_counter(model, i == 1);
                elements_scrollable_text_line(
                    canvas, 22, 14 + 22 * i, 98, name, scroll_counter, false);
            }
            elements_frame(canvas, 0, 21, 128 - 5, 21);
            elements_scrollbar(canvas, position, items_count);
            break;
        }
        case MenuStyleWii: {
            if(items_count > 6 && position >= 4) {
                if(position >= items_count - 2 + (items_count % 2)) {
                    shift_position = position - (position % 2) - 4;
                } else {
                    shift_position = position - (position % 2) - 2;
                }
            } else {
                shift_position = 0;
            }
            canvas_set_font(canvas, FontSecondary);
            size_t item_i;
            size_t x_off, y_off;
            for(uint8_t i = 0; i < 6; i++) {
                item_i = shift_position + i;
                if(item_i >= items_count) continue;
                x_off = (i / 2) * 43 + 1;
                y_off = (i % 2) * 32;
                bool selected = item_i == position;
                if(selected) {
                    elements_slightly_rounded_box(canvas, 0 + x_off, 0 + y_off, 40, 30);
                    canvas_set_color(canvas, ColorWhite);
                }
                item = MenuItemArray_get(model->items, item_i);
                menu_centered_icon(canvas, item, x_off, y_off, 40, 20);
                menu_get_name(item, name, true);
                size_t scroll_counter = menu_scroll_counter(model, selected);
                menu_text_line_centered(
                    canvas, 20 + x_off, 26 + y_off, 36, name, scroll_counter, false, true);
                if(selected) {
                    canvas_set_color(canvas, ColorBlack);
                } else {
                    elements_frame(canvas, 0 + x_off, 0 + y_off, 40, 30);
                }
            }
            break;
        }
        case MenuStyleDsi: {
            for(int8_t i = -2; i <= 2; i++) {
                shift_position = (position + items_count + i) % items_count;
                item = MenuItemArray_get(model->items, shift_position);
                size_t width = 24;
                size_t height = 26;
                int32_t pos_x = 64;
                int32_t pos_y = 36;
                if(i == 0) {
                    width += 6;
                    height += 4;
                    elements_bold_rounded_frame(
                        canvas, pos_x - width / 2, pos_y - height / 2, width, height + 5);
                    canvas_set_font(canvas, FontBatteryPercent);
                    canvas_draw_str_aligned(
                        canvas, pos_x - 9, pos_y + height / 2 + 1, AlignCenter, AlignBottom, "S");
                    canvas_draw_str_aligned(
                        canvas, pos_x, pos_y + height / 2 + 1, AlignCenter, AlignBottom, "TAR");
                    canvas_draw_str_aligned(
                        canvas, pos_x + 9, pos_y + height / 2 + 1, AlignCenter, AlignBottom, "T");

                    canvas_draw_rframe(canvas, 0, 0, 128, 18, 3);
                    canvas_draw_line(canvas, 60, 18, 64, 26);
                    canvas_draw_line(canvas, 64, 26, 68, 18);
                    canvas_set_color(canvas, ColorWhite);
                    canvas_draw_line(canvas, 60, 17, 68, 17);
                    canvas_draw_box(canvas, 62, 21, 5, 2);
                    canvas_set_color(canvas, ColorBlack);

                    canvas_set_font(canvas, FontPrimary);
                    menu_get_name(item, name, false);
                    size_t scroll_counter = menu_scroll_counter(model, true);
                    menu_text_line_centered(
                        canvas,
                        pos_x,
                        pos_y - height / 2 - 8,
                        124,
                        name,
                        scroll_counter,
                        false,
                        true);
                } else {
                    pos_x += (width + 6) * i;
                    pos_y += 2;
                    elements_slightly_rounded_frame(
                        canvas, pos_x - width / 2, pos_y - height / 2, width, height);
                }
                menu_centered_icon(canvas, item, pos_x - 7, pos_y - 7, 14, 14);
            }
            elements_scrollbar_horizontal(canvas, 0, 64, 128, position, items_count);
            break;
        }
        case MenuStylePs4: {
            canvas_set_font(canvas, FontSecondary);
            canvas_draw_str_aligned(
                canvas, 1, 1, AlignLeft, AlignTop, furi_hal_version_get_name_ptr());
            char str[10];
            Dolphin* dolphin = furi_record_open(RECORD_DOLPHIN);
            DolphinStats stats = dolphin_stats(dolphin);
            furi_record_close(RECORD_DOLPHIN);
            snprintf(str, 10, "Level %i", stats.level);
            canvas_draw_str_aligned(canvas, 127, 1, AlignRight, AlignTop, str);
            for(int8_t i = -1; i <= 4; i++) {
                shift_position = position + i;
                if(shift_position >= items_count) continue;
                item = MenuItemArray_get(model->items, shift_position);
                size_t width = 20;
                size_t height = 20;
                size_t pos_x = 36;
                size_t pos_y = 27;
                if(i == 0) {
                    width += 10;
                    height += 10;
                    pos_y += 2;
                    canvas_draw_box(canvas, pos_x - width / 2, pos_y + height / 2, width, 9);
                    canvas_set_color(canvas, ColorWhite);
                    canvas_set_font(canvas, FontBatteryPercent);
                    canvas_draw_str_aligned(
                        canvas, pos_x, pos_y + height / 2 + 1, AlignCenter, AlignTop, "Start");

                    canvas_set_color(canvas, ColorBlack);
                    canvas_set_font(canvas, FontSecondary);
                    menu_get_name(item, name, true);
                    size_t scroll_counter = menu_scroll_counter(model, true);
                    elements_scrollable_text_line(
                        canvas,
                        pos_x + width / 2 + 2,
                        pos_y + height / 2 + 7,
                        74,
                        name,
                        scroll_counter,
                        false);
                } else {
                    pos_x += (width + 1) * i + (i < 0 ? -6 : 6);
                }
                canvas_draw_frame(canvas, pos_x - width / 2, pos_y - height / 2, width, height);
                menu_centered_icon(canvas, item, pos_x - 7, pos_y - 7, 14, 14);
            }
            elements_scrollbar_horizontal(canvas, 0, 64, 128, position, items_count);
            break;
        }
        case MenuStyleVertical: {
            canvas_set_orientation(canvas, CanvasOrientationVertical);
            shift_position = model->vertical_offset;
            if(shift_position >= position || shift_position + 7 <= position) {
                shift_position = CLAMP(
                    MAX((int32_t)position - 4, 0),
                    MAX((int32_t)MenuItemArray_size(model->items) - 8, 0),
                    0);
                model->vertical_offset = shift_position;
            }
            canvas_set_font(canvas, FontSecondary);
            size_t item_i;
            size_t y_off;
            for(size_t i = 0; i < 8; i++) {
                item_i = shift_position + i;
                if(item_i >= items_count) continue;
                y_off = 16 * i;
                bool selected = item_i == position;
                if(selected) {
                    elements_slightly_rounded_box(canvas, 0, y_off, 64, 16);
                    canvas_set_color(canvas, ColorWhite);
                }
                item = MenuItemArray_get(model->items, item_i);
                menu_centered_icon(canvas, item, 0, y_off, 16, 16);
                menu_get_name(item, name, true);
                size_t scroll_counter = menu_scroll_counter(model, selected);
                elements_scrollable_text_line(
                    canvas, 17, y_off + 12, 46, name, scroll_counter, false);
                if(selected) {
                    canvas_set_color(canvas, ColorBlack);
                }
            }
            canvas_set_orientation(canvas, CanvasOrientationHorizontal);
            break;
        }
        case MenuStyleC64: {
            size_t index;
            size_t y_off, x_off;
            canvas_set_font(canvas, FontSecondary);
            canvas_draw_str_aligned(
                canvas, 64, 0, AlignCenter, AlignTop, "* FLIPPADORE 64 BASIC *");
            char memstr[29];
            snprintf(memstr, sizeof(memstr), "%d BASIC BYTES FREE", (int)memmgr_get_free_heap());
            canvas_draw_str_aligned(canvas, 64, 9, AlignCenter, AlignTop, memstr);
            canvas_set_font(canvas, FontKeyboard);
            for(size_t i = 0; i < 2; i++) {
                for(size_t j = 0; j < 5; j++) {
                    index = i * 5 + j + (position - (position % 10));
                    if(index >= items_count) continue;
                    y_off = (9 * j) + 13;
                    x_off = 64 * i;
                    bool selected = index == position;
                    size_t scroll_counter = menu_scroll_counter(model, selected);
                    if(selected) {
                        canvas_draw_box(canvas, x_off, y_off + 4, 64, 9);
                        canvas_set_color(canvas, ColorWhite);
                    }
                    item = MenuItemArray_get(model->items, index);
                    menu_get_name(item, name, true);
                    char indexstr[5];
                    snprintf(indexstr, sizeof(indexstr), "%d.", (int)index);
                    furi_string_replace_at(name, 0, 0, indexstr);
                    elements_scrollable_text_line(
                        canvas, x_off + 2, y_off + 12, 60, name, scroll_counter, false);
                    if(selected) {
                        canvas_set_color(canvas, ColorBlack);
                    }
                }
            }
            break;
        }
        case MenuStyleCompact: {
            size_t index;
            size_t y_off, x_off;
            canvas_set_font(canvas, FontBatteryPercent);
            for(size_t i = 0; i < 2; i++) {
                for(size_t j = 0; j < 8; j++) {
                    index = i * 8 + j + (position - (position % 16));
                    if(index >= items_count) continue;
                    y_off = (8 * j);
                    x_off = 64 * i;
                    bool selected = index == position;
                    size_t scroll_counter = menu_scroll_counter(model, selected);
                    if(selected) {
                        canvas_draw_box(canvas, x_off, y_off, 64, 8);
                        canvas_set_color(canvas, ColorWhite);
                    }
                    item = MenuItemArray_get(model->items, index);
                    menu_get_name(item, name, true);
                    elements_scrollable_text_line(
                        canvas, x_off + 1, y_off + 7, 62, name, scroll_counter, false);
                    if(selected) {
                        canvas_set_color(canvas, ColorBlack);
                    }
                }
            }
            break;
        }
        case MenuStyleMNTM: {
            canvas_set_font(canvas, FontPrimary);
            canvas_draw_icon(canvas, 62, 4, &I_Release_arrow_18x15);
            canvas_draw_line(canvas, 5, 15, 59, 15);
            canvas_draw_line(canvas, 7, 17, 61, 17);
            canvas_draw_line(canvas, 10, 19, 63, 19);
            char title[20];
            snprintf(title, sizeof(title), "%s", furi_hal_version_get_name_ptr());
            canvas_draw_str(canvas, 5, 12, title);
            DateTime curr_dt;
            furi_hal_rtc_get_datetime(&curr_dt);
            uint8_t hour = curr_dt.hour;
            uint8_t min = curr_dt.minute;
            LocaleTimeFormat time_format = locale_get_time_format();
            if(time_format == LocaleTimeFormat12h) {
                if(hour > 12) hour -= 12;
                if(hour == 0) hour = 12;
            }
            canvas_set_font(canvas, FontSecondary);
            char clk[20];
            snprintf(clk, sizeof(clk), "%02u:%02u", hour, min);
            canvas_draw_str(canvas, 5, 34, clk);

            uint8_t battery_percent = furi_hal_power_get_pct();
            bool charge_state = false;
            if(furi_hal_power_is_charging()) {
                if(battery_percent < 100 && !furi_hal_power_is_charging_done()) {
                    charge_state = true;
                }
            }
            char bat_display[20];
            snprintf(bat_display, sizeof(bat_display), "%d%%", battery_percent);
            canvas_draw_str(canvas, 5, 45, bat_display);
            if(charge_state) {
                canvas_draw_icon(canvas, 28, 33, &I_Voltage_16x16);
            }
            bool ext5v = furi_hal_power_is_otg_enabled();
            char ext5v_display[20];
            snprintf(ext5v_display, sizeof(ext5v_display), "5v: %s", ext5v ? "On" : "Off");
            canvas_draw_str(canvas, 5, 56, ext5v_display);

            item = MenuItemArray_get(model->items, position);
            menu_get_name(item, name, true);
            elements_bold_rounded_frame(canvas, 42, 23, 35, 33);
            menu_centered_icon(canvas, item, 43, 24, 35, 32);
            canvas_draw_frame(canvas, 0, 0, 128, 64);

            uint8_t startY = 15;
            uint8_t itemHeight = 10;
            uint8_t itemMaxVisible = 5;
            size_t endItem = position + itemMaxVisible;
            endItem = (endItem > MenuItemArray_size(model->items)) ?
                          MenuItemArray_size(model->items) :
                          endItem;
            for(size_t i = position; i < endItem; i++) {
                MenuItem* it = MenuItemArray_get(model->items, i);
                menu_get_name(it, name, true);
                uint8_t yPos = startY + ((i - position) * itemHeight);
                size_t scroll_counter = menu_scroll_counter(model, i == position);
                elements_scrollable_text_line(canvas, 83, yPos, 43, name, scroll_counter, false);
            }
            break;
        }
        case MenuStyleCoverFlow: {
            canvas_set_font(canvas, FontPrimary);
            canvas_set_bitmap_mode(canvas, true);
            canvas_draw_frame(canvas, 0, 0, 128, 64);
            canvas_draw_rframe(canvas, 45, 4, 38, 38, 3);

            canvas_draw_line(canvas, 5, 36, 1, 37);
            canvas_draw_line(canvas, 4, 9, 1, 8);
            canvas_draw_line(canvas, 6, 41, 17, 36);
            canvas_draw_line(canvas, 19, 41, 30, 36);
            canvas_draw_line(canvas, 32, 41, 43, 36);
            canvas_draw_line(canvas, 6, 4, 17, 9);
            canvas_draw_line(canvas, 19, 4, 30, 9);
            canvas_draw_line(canvas, 32, 4, 43, 9);
            canvas_draw_line(canvas, 5, 5, 5, 40);
            canvas_draw_line(canvas, 18, 5, 18, 40);
            canvas_draw_line(canvas, 31, 5, 31, 40);

            canvas_draw_line(canvas, 95, 41, 84, 36);
            canvas_draw_line(canvas, 108, 41, 97, 36);
            canvas_draw_line(canvas, 121, 41, 110, 36);
            canvas_draw_line(canvas, 84, 9, 95, 4);
            canvas_draw_line(canvas, 97, 9, 108, 4);
            canvas_draw_line(canvas, 110, 9, 121, 4);
            canvas_draw_line(canvas, 96, 5, 96, 40);
            canvas_draw_line(canvas, 109, 5, 109, 40);
            canvas_draw_line(canvas, 122, 5, 122, 40);
            canvas_draw_line(canvas, 123, 9, 126, 8);
            canvas_draw_line(canvas, 123, 36, 126, 37);

            const int32_t pos_x_center = 128 / 2;
            const int32_t pos_y_center = (64 / 2) + 1;
            const int32_t pos_y_offset = 10;
            const int32_t icon_size = 20;
            const int32_t side_icon_width = icon_size / 2;
            const int32_t padding_center_icon = 14;
            const int32_t spacing_between_icons = 3;
            const int32_t scale_base = 100;

            MenuItem* center_item = NULL;
            for(int8_t i = -3; i <= 3; i++) {
                shift_position = (position + items_count + i) % items_count;
                item = MenuItemArray_get(model->items, shift_position);
                int32_t pos_x = pos_x_center;
                int32_t pos_y = pos_y_center;
                int32_t scale_width = scale_base;
                int32_t scale_height = scale_base;
                if(i < 0) {
                    pos_x -= padding_center_icon;
                    pos_x -= ((-i) * (side_icon_width + spacing_between_icons));
                    pos_x -= (side_icon_width / 2) / 2;
                    pos_y = (pos_y_center - icon_size / 2) - pos_y_offset;
                    scale_width = 50;
                } else if(i > 0) {
                    pos_x += padding_center_icon;
                    pos_x += (i * (side_icon_width + spacing_between_icons));
                    pos_x -= side_icon_width;
                    pos_y = (pos_y_center - icon_size / 2) - pos_y_offset;
                    scale_width = 50;
                } else {
                    pos_x -= icon_size / 2;
                    pos_y = (pos_y_center - (icon_size / 2)) - pos_y_offset;
                    scale_width = scale_base;
                    scale_height = scale_base;
                    center_item = item;
                }
                menu_centered_icon_scaled(
                    canvas, item, pos_x, pos_y, icon_size, icon_size, scale_width, scale_height);
            }
            if(center_item) {
                menu_get_name(center_item, name, false);
                size_t scroll_counter = menu_scroll_counter(model, true);
                menu_text_line_centered(
                    canvas,
                    pos_x_center,
                    (pos_y_center + icon_size / 2) + pos_y_offset + 1,
                    124,
                    name,
                    scroll_counter,
                    false,
                    true);
            }
            elements_scrollbar_horizontal(canvas, 0, 60, 128, position, items_count);
            break;
        }
        default:
            break;
        }

        furi_string_free(name);
    } else {
        canvas_draw_str(canvas, 2, 32, "Empty");
        elements_scrollbar(canvas, 0, 0);
    }
}

static bool menu_input_callback(InputEvent* event, void* context) {
    Menu* menu = context;
    bool consumed = true;

    /* Vertical style rotates the screen; swap Left/Right when hand-oriented. */
    if(menu_get_style() == MenuStyleVertical &&
       furi_hal_rtc_is_flag_set(FuriHalRtcFlagHandOrient)) {
        if(event->key == InputKeyLeft) {
            event->key = InputKeyRight;
        } else if(event->key == InputKeyRight) {
            event->key = InputKeyLeft;
        }
    }

    if(event->type == InputTypeShort || event->type == InputTypeRepeat) {
        switch(event->key) {
        case InputKeyUp:
            menu_process_up(menu);
            break;
        case InputKeyDown:
            menu_process_down(menu);
            break;
        case InputKeyLeft:
            menu_process_left(menu);
            break;
        case InputKeyRight:
            menu_process_right(menu);
            break;
        case InputKeyOk:
            if(event->type != InputTypeRepeat) {
                menu_process_ok(menu);
            }
            break;
        default:
            consumed = false;
            break;
        }
    } else {
        consumed = false;
    }

    return consumed;
}

static void menu_scroll_timer_callback(void* context) {
    Menu* menu = context;
    with_view_model(menu->view, MenuModel* model, { model->scroll_counter++; }, true);
}

/* Styles whose primary (prev/next item) navigation runs along the horizontal
 * axis: Up/Down do nothing for them, Left/Right move the selection. On the
 * T-Embed's single-axis rotary encoder the plain turn physically emits
 * Up/Down, so these styles must put the view into ViewInputModeLeftRight to get
 * the turn remapped to Left/Right (see view_dispatcher input remap). The other
 * styles (List, Wii, MNTM, C64, Compact) keep Up/Down as their primary axis and
 * stay in the default ViewInputModeUpDown. */
static bool menu_style_is_horizontal(MenuStyle style) {
    switch(style) {
    case MenuStyleDsi:
    case MenuStylePs4:
    case MenuStyleVertical:
    case MenuStyleCoverFlow:
        return true;
    default:
        return false;
    }
}

static void menu_enter(void* context) {
    Menu* menu = context;
    /* Match the encoder axis to the active menu style. Done on enter so a style
     * change made in Settings takes effect as soon as the menu is shown again. */
    view_set_input_mode(
        menu->view,
        menu_style_is_horizontal(menu_get_style()) ? ViewInputModeLeftRight :
                                                      ViewInputModeUpDown);
    with_view_model(
        menu->view,
        MenuModel* model,
        {
            if(MenuItemArray_size(model->items)) {
                MenuItem* item = MenuItemArray_get(model->items, model->position);
                icon_animation_start(item->icon);
            }
            model->scroll_counter = 0;
        },
        true);
    furi_timer_start(menu->scroll_timer, 333);
}

static void menu_exit(void* context) {
    Menu* menu = context;
    with_view_model(
        menu->view,
        MenuModel* model,
        {
            if(MenuItemArray_size(model->items)) {
                MenuItem* item = MenuItemArray_get(model->items, model->position);
                icon_animation_stop(item->icon);
            }
        },
        false);
    furi_timer_stop(menu->scroll_timer);
}

Menu* menu_alloc(void) {
    Menu* menu = malloc(sizeof(Menu));
    menu->view = view_alloc();
    view_set_context(menu->view, menu);
    view_allocate_model(menu->view, ViewModelTypeLocking, sizeof(MenuModel));
    view_set_draw_callback(menu->view, menu_draw_callback);
    view_set_input_callback(menu->view, menu_input_callback);
    view_set_enter_callback(menu->view, menu_enter);
    view_set_exit_callback(menu->view, menu_exit);

    menu->scroll_timer = furi_timer_alloc(menu_scroll_timer_callback, FuriTimerTypePeriodic, menu);

    menu_style_load();
    lock_screen_load();

    with_view_model(
        menu->view,
        MenuModel* model,
        {
            MenuItemArray_init(model->items);
            model->position = 0;
            model->scroll_counter = 0;
            model->vertical_offset = 0;
        },
        true);

    return menu;
}

void menu_free(Menu* menu) {
    furi_check(menu);

    menu_reset(menu);
    with_view_model(menu->view, MenuModel* model, { MenuItemArray_clear(model->items); }, false);
    view_free(menu->view);
    furi_timer_free(menu->scroll_timer);

    free(menu);
}

View* menu_get_view(Menu* menu) {
    furi_check(menu);
    return menu->view;
}

void menu_add_item(
    Menu* menu,
    const char* label,
    const Icon* icon,
    uint32_t index,
    MenuItemCallback callback,
    void* context) {
    furi_check(menu);
    furi_check(label);

    MenuItem* item = NULL;
    with_view_model(
        menu->view,
        MenuModel* model,
        {
            item = MenuItemArray_push_new(model->items);
            item->label = label;
            item->icon = icon ? icon_animation_alloc(icon) : icon_animation_alloc(&A_Plugins_14);
            view_tie_icon_animation(menu->view, item->icon);
            item->index = index;
            item->callback = callback;
            item->callback_context = context;
        },
        true);
}

void menu_reset(Menu* menu) {
    furi_check(menu);
    with_view_model(
        menu->view,
        MenuModel* model,
        {
            for
                M_EACH(item, model->items, MenuItemArray_t) {
                    icon_animation_stop(item->icon);
                    icon_animation_free(item->icon);
                }

            MenuItemArray_reset(model->items);
            model->position = 0;
        },
        true);
}

static void menu_set_position(Menu* menu, uint32_t position) {
    furi_check(menu);

    with_view_model(
        menu->view,
        MenuModel* model,
        {
            if(position < MenuItemArray_size(model->items) && position != model->position) {
                model->scroll_counter = 0;

                MenuItem* item = MenuItemArray_get(model->items, model->position);
                icon_animation_stop(item->icon);

                item = MenuItemArray_get(model->items, position);
                icon_animation_start(item->icon);

                model->position = position;
            }
        },
        true);
}

void menu_set_selected_item(Menu* menu, uint32_t index) {
    furi_check(menu);

    with_view_model(
        menu->view,
        MenuModel* model,
        {
            if(index < MenuItemArray_size(model->items)) {
                model->position = index;
            }
        },
        true);
}

static void menu_process_up(Menu* menu) {
    size_t position;
    with_view_model(
        menu->view,
        MenuModel* model,
        {
            position = model->position;
            size_t count = MenuItemArray_size(model->items);

            switch(menu_get_style()) {
            case MenuStyleList:
            case MenuStyleMNTM:
            case MenuStyleC64:
            case MenuStyleCompact:
                if(position > 0) {
                    position--;
                } else {
                    position = count - 1;
                }
                break;
            case MenuStyleWii:
                if(position % 2 || (position == count - 1 && count % 2)) {
                    position--;
                } else {
                    position++;
                }
                break;
            default:
                break;
            }
        },
        false);
    menu_set_position(menu, position);
}

static void menu_process_down(Menu* menu) {
    size_t position;
    with_view_model(
        menu->view,
        MenuModel* model,
        {
            position = model->position;
            size_t count = MenuItemArray_size(model->items);

            switch(menu_get_style()) {
            case MenuStyleList:
            case MenuStyleMNTM:
            case MenuStyleC64:
            case MenuStyleCompact:
                if(position < count - 1) {
                    position++;
                } else {
                    position = 0;
                }
                break;
            case MenuStyleWii:
                if(position % 2 || (position == count - 1 && count % 2)) {
                    position--;
                } else {
                    position++;
                }
                break;
            default:
                break;
            }
        },
        false);
    menu_set_position(menu, position);
}

static void menu_process_left(Menu* menu) {
    size_t position;
    with_view_model(
        menu->view,
        MenuModel* model,
        {
            position = model->position;
            size_t count = MenuItemArray_size(model->items);

            switch(menu_get_style()) {
            case MenuStyleWii:
                if(position < 2) {
                    if(count % 2) {
                        position = count - 1;
                    } else {
                        position = count - 2 + position % 2;
                    }
                } else {
                    position -= 2;
                }
                break;
            case MenuStyleDsi:
            case MenuStylePs4:
            case MenuStyleVertical:
            case MenuStyleCoverFlow: {
                size_t vertical_offset = model->vertical_offset;
                if(position > 0) {
                    position--;
                    if(vertical_offset && vertical_offset == position) {
                        vertical_offset--;
                    }
                } else {
                    position = count - 1;
                    vertical_offset = count > 8 ? count - 8 : 0;
                }
                model->vertical_offset = vertical_offset;
                break;
            }
            case MenuStyleC64:
                if((position % 10) < 5) {
                    position = position + 5;
                } else {
                    position = position - 5;
                }
                break;
            case MenuStyleCompact:
                if((position % 16) < 8) {
                    position = position + 8;
                } else {
                    position = position - 8;
                }
                break;
            default:
                break;
            }
            if(position >= count) position = count - 1;
        },
        false);
    menu_set_position(menu, position);
}

static void menu_process_right(Menu* menu) {
    size_t position;
    with_view_model(
        menu->view,
        MenuModel* model,
        {
            position = model->position;
            size_t count = MenuItemArray_size(model->items);

            switch(menu_get_style()) {
            case MenuStyleWii:
                if(count % 2) {
                    if(position == count - 1) {
                        position = 0;
                    } else if(position == count - 2) {
                        position = count - 1;
                    } else {
                        position += 2;
                    }
                } else {
                    position += 2;
                    if(position >= count) {
                        position = position % 2;
                    }
                }
                break;
            case MenuStyleDsi:
            case MenuStylePs4:
            case MenuStyleVertical:
            case MenuStyleCoverFlow: {
                size_t vertical_offset = model->vertical_offset;
                if(position < count - 1) {
                    position++;
                    if(count > 8 && vertical_offset < count - 8 && vertical_offset == position - 7) {
                        vertical_offset++;
                    }
                } else {
                    position = 0;
                    vertical_offset = 0;
                }
                model->vertical_offset = vertical_offset;
                break;
            }
            case MenuStyleC64:
                if((position % 10) < 5) {
                    position = position + 5;
                } else {
                    position = position - 5;
                }
                break;
            case MenuStyleCompact:
                if((position % 16) < 8) {
                    position = position + 8;
                } else {
                    position = position - 8;
                }
                break;
            default:
                break;
            }
            if(position >= count) position = count - 1;
        },
        false);
    menu_set_position(menu, position);
}

static void menu_process_ok(Menu* menu) {
    MenuItem* item = NULL;
    with_view_model(
        menu->view,
        MenuModel* model,
        {
            if(MenuItemArray_size(model->items)) {
                item = MenuItemArray_get(model->items, model->position);
            }
        },
        true);
    if(item && item->callback) {
        item->callback(item->callback_context, item->index);
    }
}