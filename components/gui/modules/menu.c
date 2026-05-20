#include "menu.h"

#include "../elements.h"
#include <assets_icons.h>
#include <furi.h>
#include <m-array.h>

struct Menu {
    View* view;
};

typedef struct {
    const char* label;
    IconAnimation* icon;
    uint32_t index;
    MenuItemCallback callback;
    void* callback_context;
} MenuItem;

ARRAY_DEF(MenuItemArray, MenuItem, M_POD_OPLIST);

#define M_OPL_MenuItemArray_t() ARRAY_OPLIST(MenuItemArray, M_POD_OPLIST)

typedef struct {
    MenuItemArray_t items;
    size_t position;
    MenuStyle style;
} MenuModel;

static void menu_process_up(Menu* menu);
static void menu_process_down(Menu* menu);
static void menu_process_ok(Menu* menu);

static void menu_draw_list(Canvas* canvas, MenuModel* model) {
    size_t position = model->position;
    size_t items_count = MenuItemArray_size(model->items);
    if(!items_count) {
        canvas_draw_str(canvas, 2, 32, "Empty");
        elements_scrollbar(canvas, 0, 0);
        return;
    }

    MenuItem* item;
    size_t shift_position;

    canvas_set_font(canvas, FontSecondary);
    shift_position = (0 + position + items_count - 1) % items_count;
    item = MenuItemArray_get(model->items, shift_position);
    canvas_draw_icon_animation(canvas, 4, 3, item->icon);
    canvas_draw_str(canvas, 22, 14, item->label);

    canvas_set_font(canvas, FontPrimary);
    shift_position = (1 + position + items_count - 1) % items_count;
    item = MenuItemArray_get(model->items, shift_position);
    canvas_draw_icon_animation(canvas, 4, 25, item->icon);
    canvas_draw_str(canvas, 22, 36, item->label);

    canvas_set_font(canvas, FontSecondary);
    shift_position = (2 + position + items_count - 1) % items_count;
    item = MenuItemArray_get(model->items, shift_position);
    canvas_draw_icon_animation(canvas, 4, 47, item->icon);
    canvas_draw_str(canvas, 22, 58, item->label);

    elements_frame(canvas, 0, 21, 128 - 5, 21);
    elements_scrollbar(canvas, position, items_count);
}

static void menu_draw_compact(Canvas* canvas, MenuModel* model) {
    size_t position = model->position;
    size_t items_count = MenuItemArray_size(model->items);
    if(!items_count) {
        canvas_draw_str(canvas, 2, 32, "Empty");
        elements_scrollbar(canvas, 0, 0);
        return;
    }

    size_t visible = 6;
    size_t start = (position > 2) ? position - 2 : 0;
    if(start + visible > items_count) start = (items_count > visible) ? items_count - visible : 0;

    canvas_set_font(canvas, FontSecondary);
    for(size_t i = 0; i < visible && (start + i) < items_count; i++) {
        MenuItem* item = MenuItemArray_get(model->items, start + i);
        uint8_t y = i * 10 + 2;
        if(start + i == position) {
            canvas_set_color(canvas, ColorBlack);
            elements_slightly_rounded_box(canvas, 0, y - 1, 123, 10);
            canvas_set_color(canvas, ColorWhite);
            canvas_draw_str(canvas, 4, y + 8, item->label);
            canvas_set_color(canvas, ColorBlack);
        } else {
            canvas_draw_str(canvas, 4, y + 8, item->label);
        }
    }
    elements_scrollbar(canvas, position, items_count);
}

static void menu_draw_vertical(Canvas* canvas, MenuModel* model) {
    size_t position = model->position;
    size_t items_count = MenuItemArray_size(model->items);
    if(!items_count) {
        canvas_draw_str(canvas, 2, 32, "Empty");
        elements_scrollbar(canvas, 0, 0);
        return;
    }

    size_t visible = 4;
    size_t start = (position > 1) ? position - 1 : 0;
    if(start + visible > items_count) start = (items_count > visible) ? items_count - visible : 0;

    canvas_set_font(canvas, FontSecondary);
    for(size_t i = 0; i < visible && (start + i) < items_count; i++) {
        MenuItem* item = MenuItemArray_get(model->items, start + i);
        uint8_t y = i * 14 + 2;
        canvas_draw_icon_animation(canvas, 4, y + 1, item->icon);
        if(start + i == position) {
            canvas_set_color(canvas, ColorBlack);
            elements_slightly_rounded_box(canvas, 0, y - 1, 123, 14);
            canvas_set_color(canvas, ColorWhite);
            canvas_draw_str(canvas, 22, y + 10, item->label);
            canvas_set_color(canvas, ColorBlack);
        } else {
            canvas_draw_str(canvas, 22, y + 10, item->label);
        }
    }
    elements_scrollbar(canvas, position, items_count);
}

static void menu_draw_callback(Canvas* canvas, void* _model) {
    MenuModel* model = _model;
    canvas_clear(canvas);

    switch(model->style) {
    case MenuStyleCompact:
        menu_draw_compact(canvas, model);
        break;
    case MenuStyleVertical:
        menu_draw_vertical(canvas, model);
        break;
    default:
        menu_draw_list(canvas, model);
        break;
    }
}

static bool menu_input_callback(InputEvent* event, void* context) {
    Menu* menu = context;
    bool consumed = false;

    if(event->type == InputTypeShort) {
        if(event->key == InputKeyUp) {
            consumed = true;
            menu_process_up(menu);
        } else if(event->key == InputKeyDown) {
            consumed = true;
            menu_process_down(menu);
        } else if(event->key == InputKeyOk) {
            consumed = true;
            menu_process_ok(menu);
        }
    } else if(event->type == InputTypeRepeat) {
        if(event->key == InputKeyUp) {
            consumed = true;
            menu_process_up(menu);
        } else if(event->key == InputKeyDown) {
            consumed = true;
            menu_process_down(menu);
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
            model->style = MenuStyleList;
        },
        true);

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

void menu_set_style(Menu* menu, MenuStyle style) {
    furi_check(menu);
    with_view_model(
        menu->view,
        MenuModel * model,
        { model->style = style; },
        true);
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
