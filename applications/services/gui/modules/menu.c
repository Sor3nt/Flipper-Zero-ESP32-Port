#include "menu.h"

#include <gui/elements.h>
#include <assets_icons.h>
#include <furi.h>
#include <m-array.h>
#include <components/momentum/settings.h>

struct Menu {
    View* view;
    MenuStyle style;
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
} MenuModel;

static void menu_process_up(Menu* menu);
static void menu_process_down(Menu* menu);
static void menu_process_left(Menu* menu);
static void menu_process_right(Menu* menu);
static void menu_process_ok(Menu* menu);

// ============================================================================
// Style: List (default OFW style - 3-line scrollable list with icons)
// ============================================================================
static void menu_draw_list(Canvas* canvas, const MenuModel* model) {
    canvas_clear(canvas);
    size_t position = model->position;
    size_t items_count = MenuItemArray_size(model->items);
    if(items_count) {
        MenuItem* item;
        size_t shift_position;
        // First line
        canvas_set_font(canvas, FontSecondary);
        shift_position = (0 + position + items_count - 1) % items_count;
        item = MenuItemArray_get(model->items, shift_position);
        canvas_draw_icon_animation(canvas, 4, 3, item->icon);
        canvas_draw_str(canvas, 22, 14, item->label);
        // Second line main (selected)
        canvas_set_font(canvas, FontPrimary);
        shift_position = (1 + position + items_count - 1) % items_count;
        item = MenuItemArray_get(model->items, shift_position);
        canvas_draw_icon_animation(canvas, 4, 25, item->icon);
        canvas_draw_str(canvas, 22, 36, item->label);
        // Third line
        canvas_set_font(canvas, FontSecondary);
        shift_position = (2 + position + items_count - 1) % items_count;
        item = MenuItemArray_get(model->items, shift_position);
        canvas_draw_icon_animation(canvas, 4, 47, item->icon);
        canvas_draw_str(canvas, 22, 58, item->label);
        // Frame and scrollbar
        elements_frame(canvas, 0, 21, 128 - 5, 21);
        elements_scrollbar(canvas, position, items_count);
    } else {
        canvas_draw_str(canvas, 2, 32, "Empty");
        elements_scrollbar(canvas, 0, 0);
    }
}

// ============================================================================
// Style: Wii (3x2 grid of large icons with labels below)
// ============================================================================
#define WII_COLS 3
#define WII_ROWS 2
#define WII_ITEMS_PER_PAGE (WII_COLS * WII_ROWS)
#define WII_CELL_W 42
#define WII_CELL_H 32
#define WII_ICON_X_OFFSET 13
#define WII_ICON_Y_OFFSET 2
#define WII_LABEL_Y_OFFSET 28

static void menu_draw_wii(Canvas* canvas, const MenuModel* model) {
    canvas_clear(canvas);
    size_t items_count = MenuItemArray_size(model->items);
    if(!items_count) {
        canvas_draw_str(canvas, 2, 32, "Empty");
        return;
    }
    size_t page_start = (model->position / WII_ITEMS_PER_PAGE) * WII_ITEMS_PER_PAGE;
    for(size_t i = 0; i < WII_ITEMS_PER_PAGE; i++) {
        size_t idx = page_start + i;
        if(idx >= items_count) break;
        MenuItem* item = MenuItemArray_get(model->items, idx);
        uint8_t col = i % WII_COLS;
        uint8_t row = i / WII_COLS;
        uint8_t x = col * WII_CELL_W + 1;
        uint8_t y = row * WII_CELL_H;
        // Highlight selected
        if(idx == model->position) {
            elements_frame(canvas, x, y, WII_CELL_W, WII_CELL_H);
        }
        canvas_draw_icon_animation(canvas, x + WII_ICON_X_OFFSET, y + WII_ICON_Y_OFFSET, item->icon);
        canvas_set_font(canvas, FontSecondary);
        // Truncate label to fit cell
        char label_buf[10];
        strncpy(label_buf, item->label, sizeof(label_buf) - 1);
        label_buf[sizeof(label_buf) - 1] = '\0';
        uint8_t label_w = canvas_string_width(canvas, label_buf);
        uint8_t label_x = x + (WII_CELL_W - label_w) / 2;
        canvas_draw_str(canvas, label_x, y + WII_LABEL_Y_OFFSET, label_buf);
    }
    // Page indicator
    size_t total_pages = (items_count + WII_ITEMS_PER_PAGE - 1) / WII_ITEMS_PER_PAGE;
    size_t current_page = model->position / WII_ITEMS_PER_PAGE;
    if(total_pages > 1) {
        elements_scrollbar(canvas, current_page, total_pages);
    }
}

// ============================================================================
// Style: DSi (horizontal carousel with focused center item)
// ============================================================================
static void menu_draw_dsi(Canvas* canvas, const MenuModel* model) {
    canvas_clear(canvas);
    size_t items_count = MenuItemArray_size(model->items);
    if(!items_count) {
        canvas_draw_str(canvas, 2, 32, "Empty");
        return;
    }
    // Draw 5 items centered on current position
    int center_x = 54;
    int spacing = 28;
    for(int offset = -2; offset <= 2; offset++) {
        int idx = ((int)model->position + offset + (int)items_count) % (int)items_count;
        MenuItem* item = MenuItemArray_get(model->items, (size_t)idx);
        int x = center_x + (offset * spacing);
        if(x < -14 || x > 128) continue;
        if(offset == 0) {
            // Center item is larger / highlighted
            elements_frame(canvas, x - 2, 8, 22, 22);
            canvas_draw_icon_animation(canvas, x + 1, 11, item->icon);
            canvas_set_font(canvas, FontPrimary);
            uint8_t label_w = canvas_string_width(canvas, item->label);
            canvas_draw_str(canvas, 64 - label_w / 2, 48, item->label);
        } else {
            // Side items are dimmer/smaller
            canvas_draw_icon_animation(canvas, x + 1, 14, item->icon);
        }
    }
    // Bottom scrollbar
    canvas_set_font(canvas, FontSecondary);
    char pos_str[16];
    snprintf(pos_str, sizeof(pos_str), "%zu/%zu", model->position + 1, items_count);
    uint8_t w = canvas_string_width(canvas, pos_str);
    canvas_draw_str(canvas, 64 - w / 2, 62, pos_str);
}

// ============================================================================
// Style: PS4 (horizontal timeline with name and battery details)
// ============================================================================
static void menu_draw_ps4(Canvas* canvas, const MenuModel* model) {
    canvas_clear(canvas);
    size_t items_count = MenuItemArray_size(model->items);
    if(!items_count) {
        canvas_draw_str(canvas, 2, 32, "Empty");
        return;
    }
    // Top bar with device name
    canvas_set_font(canvas, FontSecondary);
    const char* dev_name = furi_hal_version_get_name_ptr();
    if(!dev_name) dev_name = "Flipper";
    canvas_draw_str(canvas, 2, 9, dev_name);

    // Draw horizontal icon strip
    int center_x = 56;
    int spacing = 24;
    for(int offset = -3; offset <= 3; offset++) {
        int idx = ((int)model->position + offset + (int)items_count) % (int)items_count;
        MenuItem* item = MenuItemArray_get(model->items, (size_t)idx);
        int x = center_x + (offset * spacing);
        if(x < -14 || x > 128) continue;
        if(offset == 0) {
            canvas_draw_rframe(canvas, x - 3, 16, 22, 22, 3);
            canvas_draw_icon_animation(canvas, x, 19, item->icon);
        } else {
            canvas_draw_icon_animation(canvas, x, 19, item->icon);
        }
    }
    // Selected item label
    MenuItem* sel = MenuItemArray_get(model->items, model->position);
    canvas_set_font(canvas, FontPrimary);
    uint8_t lw = canvas_string_width(canvas, sel->label);
    canvas_draw_str(canvas, 64 - lw / 2, 52, sel->label);

    // Position indicator
    canvas_set_font(canvas, FontSecondary);
    char pos_str[16];
    snprintf(pos_str, sizeof(pos_str), "%zu/%zu", model->position + 1, items_count);
    uint8_t pw = canvas_string_width(canvas, pos_str);
    canvas_draw_str(canvas, 64 - pw / 2, 62, pos_str);
}

// ============================================================================
// Style: Vertical (90-degree rotated list)
// ============================================================================
static void menu_draw_vertical(Canvas* canvas, const MenuModel* model) {
    canvas_clear(canvas);
    size_t items_count = MenuItemArray_size(model->items);
    if(!items_count) {
        canvas_draw_str(canvas, 2, 32, "Empty");
        return;
    }
    // Draw items vertically with icons at left, text rotated
    size_t visible = 4;
    size_t start = 0;
    if(model->position >= visible / 2) {
        start = model->position - visible / 2;
    }
    if(start + visible > items_count) {
        start = (items_count > visible) ? items_count - visible : 0;
    }
    for(size_t i = 0; i < visible && (start + i) < items_count; i++) {
        size_t idx = start + i;
        MenuItem* item = MenuItemArray_get(model->items, idx);
        uint8_t y = i * 16;
        if(idx == model->position) {
            canvas_set_color(canvas, 1);
            canvas_draw_box(canvas, 0, y, 128, 15);
            canvas_set_color(canvas, 0);
            canvas_set_font(canvas, FontPrimary);
        } else {
            canvas_set_font(canvas, FontSecondary);
        }
        canvas_draw_icon_animation(canvas, 2, y + 1, item->icon);
        canvas_draw_str(canvas, 20, y + 12, item->label);
        if(idx == model->position) {
            canvas_set_color(canvas, 1);
        }
    }
    elements_scrollbar(canvas, model->position, items_count);
}

// ============================================================================
// Style: C64 (retro Commodore 64 BASIC prompt)
// ============================================================================
static void menu_draw_c64(Canvas* canvas, const MenuModel* model) {
    canvas_clear(canvas);
    size_t items_count = MenuItemArray_size(model->items);

    // Fill with "blue" background (inverted white on black display)
    canvas_set_color(canvas, 1);
    canvas_draw_box(canvas, 0, 0, 128, 64);
    canvas_set_color(canvas, 0);

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 4, 10, "**** FLIPPER 64 BASIC V2 ****");
    canvas_draw_str(canvas, 4, 20, "READY.");

    if(!items_count) {
        canvas_draw_str(canvas, 4, 30, "NO PROGRAMS FOUND");
        return;
    }

    // Show up to 4 items as BASIC-style listing
    size_t visible = 4;
    size_t start = 0;
    if(model->position >= 2) start = model->position - 2;
    if(start + visible > items_count && items_count > visible) {
        start = items_count - visible;
    }

    for(size_t i = 0; i < visible && (start + i) < items_count; i++) {
        size_t idx = start + i;
        MenuItem* item = MenuItemArray_get(model->items, idx);
        uint8_t y = 30 + i * 9;
        char line[32];
        snprintf(line, sizeof(line), "%zu %s", (idx + 1) * 10, item->label);
        if(idx == model->position) {
            // Cursor blink indicator
            canvas_draw_str(canvas, 1, y, ">");
        }
        canvas_draw_str(canvas, 8, y, line);
    }
}

// ============================================================================
// Style: Compact (2-column dense text list)
// ============================================================================
#define COMPACT_ROWS 6
#define COMPACT_COL_W 63

static void menu_draw_compact(Canvas* canvas, const MenuModel* model) {
    canvas_clear(canvas);
    size_t items_count = MenuItemArray_size(model->items);
    if(!items_count) {
        canvas_draw_str(canvas, 2, 32, "Empty");
        return;
    }

    size_t items_per_page = COMPACT_ROWS * 2;
    size_t page_start = (model->position / items_per_page) * items_per_page;

    canvas_set_font(canvas, FontSecondary);
    for(size_t i = 0; i < items_per_page; i++) {
        size_t idx = page_start + i;
        if(idx >= items_count) break;
        MenuItem* item = MenuItemArray_get(model->items, idx);
        uint8_t col = (i < COMPACT_ROWS) ? 0 : 1;
        uint8_t row = i % COMPACT_ROWS;
        uint8_t x = col * COMPACT_COL_W + 2;
        uint8_t y = row * 10 + 10;

        if(idx == model->position) {
            canvas_set_color(canvas, 1);
            canvas_draw_box(canvas, x - 1, y - 8, COMPACT_COL_W - 1, 10);
            canvas_set_color(canvas, 0);
            canvas_draw_str(canvas, x, y, item->label);
            canvas_set_color(canvas, 1);
        } else {
            canvas_draw_str(canvas, x, y, item->label);
        }
    }
    // Divider line between columns
    canvas_draw_line(canvas, COMPACT_COL_W, 0, COMPACT_COL_W, 63);
    elements_scrollbar(canvas, model->position / items_per_page,
        (items_count + items_per_page - 1) / items_per_page);
}

// ============================================================================
// Style: MNTM (Momentum dashboard with battery %, device name)
// ============================================================================
static void menu_draw_mntm(Canvas* canvas, const MenuModel* model) {
    canvas_clear(canvas);
    size_t items_count = MenuItemArray_size(model->items);

    // Top dashboard bar
    canvas_set_font(canvas, FontSecondary);
    const char* dev_name = furi_hal_version_get_name_ptr();
    if(!dev_name) dev_name = "Flipper";
    canvas_draw_str(canvas, 2, 8, dev_name);
    canvas_draw_str(canvas, 90, 8, "MNTM");

    // Horizontal divider
    canvas_draw_line(canvas, 0, 11, 127, 11);

    if(!items_count) {
        canvas_draw_str(canvas, 2, 32, "Empty");
        return;
    }

    // Menu items below dashboard
    size_t visible = 4;
    size_t start = 0;
    if(model->position >= visible / 2) start = model->position - visible / 2;
    if(start + visible > items_count && items_count > visible) {
        start = items_count - visible;
    }

    for(size_t i = 0; i < visible && (start + i) < items_count; i++) {
        size_t idx = start + i;
        MenuItem* item = MenuItemArray_get(model->items, idx);
        uint8_t y = 14 + i * 13;
        if(idx == model->position) {
            canvas_set_color(canvas, 1);
            canvas_draw_box(canvas, 0, y, 123, 12);
            canvas_set_color(canvas, 0);
            canvas_set_font(canvas, FontPrimary);
        } else {
            canvas_set_font(canvas, FontSecondary);
        }
        canvas_draw_icon_animation(canvas, 2, y + 1, item->icon);
        canvas_draw_str(canvas, 18, y + 10, item->label);
        if(idx == model->position) {
            canvas_set_color(canvas, 1);
        }
    }
    elements_scrollbar(canvas, model->position, items_count);
}

// ============================================================================
// Style: CoverFlow (3D-like album coverflow with horizontally stacked covers)
// ============================================================================
static void menu_draw_coverflow(Canvas* canvas, const MenuModel* model) {
    canvas_clear(canvas);
    size_t items_count = MenuItemArray_size(model->items);
    if(!items_count) {
        canvas_draw_str(canvas, 2, 32, "Empty");
        return;
    }

    // Draw 5 items in a coverflow arrangement
    int center_x = 54;
    for(int offset = -2; offset <= 2; offset++) {
        int idx = ((int)model->position + offset + (int)items_count) % (int)items_count;
        MenuItem* item = MenuItemArray_get(model->items, (size_t)idx);

        int abs_offset = (offset < 0) ? -offset : offset;
        int x, y, w, h;

        if(offset == 0) {
            // Center item - full size
            x = center_x;
            y = 6;
            w = 20;
            h = 20;
            canvas_draw_rframe(canvas, x - 3, y - 3, w + 6, h + 6, 2);
            canvas_draw_icon_animation(canvas, x, y, item->icon);
        } else {
            // Side items - smaller and offset
            x = center_x + (offset * 26);
            y = 10 + abs_offset * 2;
            (void)w;
            (void)h;
            canvas_draw_icon_animation(canvas, x, y, item->icon);
        }
    }

    // Selected item label at bottom
    MenuItem* sel = MenuItemArray_get(model->items, model->position);
    canvas_set_font(canvas, FontPrimary);
    uint8_t lw = canvas_string_width(canvas, sel->label);
    canvas_draw_str(canvas, 64 - lw / 2, 46, sel->label);

    // Dots indicator
    canvas_set_font(canvas, FontSecondary);
    int dots_start = 64 - (int)(items_count * 3) / 2;
    for(size_t i = 0; i < items_count && i < 20; i++) {
        int dx = dots_start + i * 6;
        if(dx < 0 || dx > 127) continue;
        if(i == model->position) {
            canvas_draw_disc(canvas, dx, 58, 2);
        } else {
            canvas_draw_circle(canvas, dx, 58, 1);
        }
    }
}

// ============================================================================
// Draw callback dispatches to the selected style
// ============================================================================
static void menu_draw_callback(Canvas* canvas, void* _model) {
    MenuModel* model = _model;

    MenuStyle style = momentum_settings.menu_style;
    if(style >= MenuStyleCount) style = MenuStyleList;

    switch(style) {
    case MenuStyleList:
        menu_draw_list(canvas, model);
        break;
    case MenuStyleWii:
        menu_draw_wii(canvas, model);
        break;
    case MenuStyleDsi:
        menu_draw_dsi(canvas, model);
        break;
    case MenuStylePs4:
        menu_draw_ps4(canvas, model);
        break;
    case MenuStyleVertical:
        menu_draw_vertical(canvas, model);
        break;
    case MenuStyleC64:
        menu_draw_c64(canvas, model);
        break;
    case MenuStyleCompact:
        menu_draw_compact(canvas, model);
        break;
    case MenuStyleMNTM:
        menu_draw_mntm(canvas, model);
        break;
    case MenuStyleCoverFlow:
        menu_draw_coverflow(canvas, model);
        break;
    default:
        menu_draw_list(canvas, model);
        break;
    }
}

// ============================================================================
// Input callback - handles navigation depending on style
// ============================================================================
static bool menu_input_callback(InputEvent* event, void* context) {
    Menu* menu = context;
    bool consumed = false;

    MenuStyle style = momentum_settings.menu_style;

    // Horizontal styles use left/right, vertical styles use up/down
    bool horizontal = (style == MenuStyleDsi || style == MenuStylePs4 ||
                       style == MenuStyleCoverFlow);

    if(event->type == InputTypeShort || event->type == InputTypeRepeat) {
        if(horizontal) {
            if(event->key == InputKeyLeft) {
                consumed = true;
                menu_process_up(menu); // previous
            } else if(event->key == InputKeyRight) {
                consumed = true;
                menu_process_down(menu); // next
            } else if(event->key == InputKeyOk && event->type == InputTypeShort) {
                consumed = true;
                menu_process_ok(menu);
            }
        } else if(style == MenuStyleWii) {
            // Grid navigation
            if(event->key == InputKeyUp) {
                consumed = true;
                menu_process_left(menu); // up a row
            } else if(event->key == InputKeyDown) {
                consumed = true;
                menu_process_right(menu); // down a row
            } else if(event->key == InputKeyLeft) {
                consumed = true;
                menu_process_up(menu);
            } else if(event->key == InputKeyRight) {
                consumed = true;
                menu_process_down(menu);
            } else if(event->key == InputKeyOk && event->type == InputTypeShort) {
                consumed = true;
                menu_process_ok(menu);
            }
        } else {
            // Standard vertical navigation
            if(event->key == InputKeyUp) {
                consumed = true;
                menu_process_up(menu);
            } else if(event->key == InputKeyDown) {
                consumed = true;
                menu_process_down(menu);
            } else if(event->key == InputKeyOk && event->type == InputTypeShort) {
                consumed = true;
                menu_process_ok(menu);
            }
        }
    }

    return consumed;
}

static void menu_enter(void* context) {
    Menu* menu = context;
    with_view_model(
        menu->view,
        MenuModel * model,
        {
            if(MenuItemArray_size(model->items)) {
                MenuItem* item = MenuItemArray_get(model->items, model->position);
                icon_animation_start(item->icon);
            }
        },
        false);
}

static void menu_exit(void* context) {
    Menu* menu = context;
    with_view_model(
        menu->view,
        MenuModel * model,
        {
            if(MenuItemArray_size(model->items)) {
                MenuItem* item = MenuItemArray_get(model->items, model->position);
                icon_animation_stop(item->icon);
            }
        },
        false);
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

    with_view_model(
        menu->view,
        MenuModel * model,
        {
            MenuItemArray_init(model->items);
            model->position = 0;
        },
        true);

    // Initialize style from momentum settings
    if(momentum_settings.menu_style < MenuStyleCount) {
        menu->style = momentum_settings.menu_style;
    } else {
        menu->style = MenuStyleList;
    }

    return menu;
}

void menu_free(Menu* menu) {
    furi_check(menu);

    menu_reset(menu);
    with_view_model(menu->view, MenuModel * model, { MenuItemArray_clear(model->items); }, false);
    view_free(menu->view);

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
        MenuModel * model,
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
        MenuModel * model,
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

void menu_set_selected_item(Menu* menu, uint32_t index) {
    furi_check(menu);

    with_view_model(
        menu->view,
        MenuModel * model,
        {
            if(index < MenuItemArray_size(model->items)) {
                model->position = index;
            }
        },
        true);
}

uint32_t menu_get_selected_item(const Menu* menu) {
    furi_check(menu);
    uint32_t result = 0;
    with_view_model(
        menu->view,
        MenuModel * model,
        { result = (uint32_t)model->position; },
        false);
    return result;
}

void menu_set_style(Menu* menu, MenuStyle style) {
    furi_check(menu);
    if(style < MenuStyleCount) {
        menu->style = style;
    }
}

MenuStyle menu_get_style(const Menu* menu) {
    furi_check(menu);
    return menu->style;
}

static void menu_process_up(Menu* menu) {
    with_view_model(
        menu->view,
        MenuModel * model,
        {
            if(MenuItemArray_size(model->items)) {
                MenuItem* item = MenuItemArray_get(model->items, model->position);
                icon_animation_stop(item->icon);

                if(model->position > 0) {
                    model->position--;
                } else {
                    model->position = MenuItemArray_size(model->items) - 1;
                }

                item = MenuItemArray_get(model->items, model->position);
                icon_animation_start(item->icon);
            }
        },
        true);
}

static void menu_process_down(Menu* menu) {
    with_view_model(
        menu->view,
        MenuModel * model,
        {
            if(MenuItemArray_size(model->items)) {
                MenuItem* item = MenuItemArray_get(model->items, model->position);
                icon_animation_stop(item->icon);

                if(model->position < MenuItemArray_size(model->items) - 1) {
                    model->position++;
                } else {
                    model->position = 0;
                }

                item = MenuItemArray_get(model->items, model->position);
                icon_animation_start(item->icon);
            }
        },
        true);
}

// For Wii grid: move up a row (subtract WII_COLS)
static void menu_process_left(Menu* menu) {
    with_view_model(
        menu->view,
        MenuModel * model,
        {
            size_t count = MenuItemArray_size(model->items);
            if(count) {
                MenuItem* item = MenuItemArray_get(model->items, model->position);
                icon_animation_stop(item->icon);

                if(model->position >= WII_COLS) {
                    model->position -= WII_COLS;
                } else {
                    // Wrap to last row
                    size_t last_row_start = (count / WII_COLS) * WII_COLS;
                    size_t target = last_row_start + (model->position % WII_COLS);
                    if(target >= count) target = count - 1;
                    model->position = target;
                }

                item = MenuItemArray_get(model->items, model->position);
                icon_animation_start(item->icon);
            }
        },
        true);
}

// For Wii grid: move down a row (add WII_COLS)
static void menu_process_right(Menu* menu) {
    with_view_model(
        menu->view,
        MenuModel * model,
        {
            size_t count = MenuItemArray_size(model->items);
            if(count) {
                MenuItem* item = MenuItemArray_get(model->items, model->position);
                icon_animation_stop(item->icon);

                if(model->position + WII_COLS < count) {
                    model->position += WII_COLS;
                } else {
                    // Wrap to first row
                    model->position = model->position % WII_COLS;
                    if(model->position >= count) model->position = 0;
                }

                item = MenuItemArray_get(model->items, model->position);
                icon_animation_start(item->icon);
            }
        },
        true);
}

static void menu_process_ok(Menu* menu) {
    MenuItem* item = NULL;
    with_view_model(
        menu->view,
        MenuModel * model,
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
