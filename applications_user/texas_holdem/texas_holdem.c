#include <furi.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <input/input.h>

typedef enum {
    HoldemActionFold = 0,
    HoldemActionCheck,
    HoldemActionCall,
    HoldemActionRaise,
    HoldemActionCount,
} HoldemAction;

typedef struct {
    FuriMessageQueue* input_queue;
    ViewPort* view_port;

    uint16_t player_chips;
    uint16_t cpu_chips;
    uint16_t pot;

    uint8_t selected_action;
    bool show_menu;
    bool player_folded;
} TexasHoldemApp;

static const char* action_names[HoldemActionCount] = {
    "FOLD",
    "CHK",
    "CALL",
    "BET",
};

static void draw_card(
    Canvas* canvas,
    int16_t x,
    int16_t y,
    const char* rank,
    char suit,
    bool face_down) {
    const int16_t w = 13;
    const int16_t h = 17;

    canvas_set_color(canvas, ColorWhite);
    canvas_draw_rbox(canvas, x, y, w, h, 2);

    canvas_set_color(canvas, ColorBlack);
    canvas_draw_rframe(canvas, x, y, w, h, 2);

    if(face_down) {
        canvas_draw_rframe(canvas, x + 2, y + 2, 9, 13, 1);
        canvas_draw_line(canvas, x + 3, y + 4, x + 9, y + 12);
        canvas_draw_line(canvas, x + 9, y + 4, x + 3, y + 12);
        return;
    }

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, x + 2, y + 7, rank);

    char suit_text[2] = {suit, '\0'};
    canvas_draw_str_aligned(
        canvas,
        x + 7,
        y + 13,
        AlignCenter,
        AlignCenter,
        suit_text);
}

static void draw_chip(Canvas* canvas, int16_t x, int16_t y) {
    canvas_draw_disc(canvas, x, y, 4);

    canvas_set_color(canvas, ColorWhite);
    canvas_draw_disc(canvas, x, y, 1);

    canvas_set_color(canvas, ColorBlack);
}

static void draw_action_menu(Canvas* canvas, TexasHoldemApp* app) {
    const int16_t menu_x = 21;
    const int16_t menu_y = 25;
    const int16_t menu_w = 86;
    const int16_t menu_h = 14;

    canvas_set_color(canvas, ColorWhite);
    canvas_draw_rbox(canvas, menu_x, menu_y, menu_w, menu_h, 3);

    canvas_set_color(canvas, ColorBlack);
    canvas_draw_rframe(canvas, menu_x, menu_y, menu_w, menu_h, 3);

    for(uint8_t i = 0; i < HoldemActionCount; i++) {
        const int16_t item_x = 25 + ((int16_t)i * 21);

        if(i == app->selected_action) {
            canvas_draw_rbox(canvas, item_x - 2, 28, 18, 8, 2);
            canvas_set_color(canvas, ColorWhite);
        }

        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(
            canvas,
            item_x + 7,
            32,
            AlignCenter,
            AlignCenter,
            action_names[i]);

        canvas_set_color(canvas, ColorBlack);
    }
}

static void texas_holdem_draw(Canvas* canvas, void* context) {
    TexasHoldemApp* app = context;

    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);

    /* Table outline */
    canvas_draw_rframe(canvas, 1, 1, 126, 62, 10);
    canvas_draw_rframe(canvas, 4, 4, 120, 56, 8);

    /* CPU row */
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 8, 10, "CPU");

    char cpu_text[14];
    snprintf(cpu_text, sizeof(cpu_text), "$%u", app->cpu_chips);
    canvas_draw_str_aligned(canvas, 119, 10, AlignRight, AlignBottom, cpu_text);

    draw_card(canvas, 51, 7, "", 0, true);
    draw_card(canvas, 65, 7, "", 0, true);

    /* Community-card row */
    draw_card(canvas, 28, 23, "A", 'S', false);
    draw_card(canvas, 42, 23, "K", 'H', false);
    draw_card(canvas, 56, 23, "7", 'D', false);
    draw_card(canvas, 70, 23, "", 0, true);
    draw_card(canvas, 84, 23, "", 0, true);

    /* Pot, placed between board and player */
    draw_chip(canvas, 54, 43);
    draw_chip(canvas, 64, 41);
    draw_chip(canvas, 74, 43);

    char pot_text[16];
    snprintf(pot_text, sizeof(pot_text), "POT $%u", app->pot);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 49, AlignCenter, AlignCenter, pot_text);

    /* Player row */
    canvas_draw_str(canvas, 8, 60, "YOU");

    char player_text[14];
    snprintf(player_text, sizeof(player_text), "$%u", app->player_chips);
    canvas_draw_str_aligned(canvas, 119, 60, AlignRight, AlignBottom, player_text);

    if(app->player_folded) {
        canvas_draw_str_aligned(
            canvas,
            64,
            59,
            AlignCenter,
            AlignBottom,
            "YOU FOLDED - OK");
    } else {
        draw_card(canvas, 51, 47, "Q", 'C', false);
        draw_card(canvas, 65, 47, "J", 'S', false);
    }

    if(app->show_menu && !app->player_folded) {
        draw_action_menu(canvas, app);
    }
}

static void texas_holdem_input(InputEvent* input_event, void* context) {
    TexasHoldemApp* app = context;
    furi_message_queue_put(app->input_queue, input_event, FuriWaitForever);
}

static void reset_round(TexasHoldemApp* app) {
    app->player_chips = 500;
    app->cpu_chips = 500;
    app->pot = 30;
    app->selected_action = HoldemActionCheck;
    app->show_menu = false;
    app->player_folded = false;
}

static void select_action(TexasHoldemApp* app) {
    switch(app->selected_action) {
    case HoldemActionFold:
        app->player_folded = true;
        app->cpu_chips += app->pot;
        app->pot = 0;
        app->show_menu = false;
        break;

    case HoldemActionCheck:
        app->show_menu = false;
        break;

    case HoldemActionCall:
        if(app->player_chips >= 10) {
            app->player_chips -= 10;
            app->pot += 10;
        }
        app->show_menu = false;
        break;

    case HoldemActionRaise:
        if(app->player_chips >= 20) {
            app->player_chips -= 20;
            app->pot += 20;
        }
        app->show_menu = false;
        break;

    default:
        break;
    }
}

int32_t texas_holdem_app(void* p) {
    UNUSED(p);

    TexasHoldemApp* app = malloc(sizeof(TexasHoldemApp));
    if(!app) {
        return -1;
    }

    app->input_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->view_port = view_port_alloc();

    if(!app->input_queue || !app->view_port) {
        if(app->view_port) view_port_free(app->view_port);
        if(app->input_queue) furi_message_queue_free(app->input_queue);
        free(app);
        return -1;
    }

    reset_round(app);

    view_port_draw_callback_set(app->view_port, texas_holdem_draw, app);
    view_port_input_callback_set(app->view_port, texas_holdem_input, app);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, app->view_port, GuiLayerFullscreen);

    bool running = true;

    while(running) {
        InputEvent event;

        if(furi_message_queue_get(app->input_queue, &event, FuriWaitForever) != FuriStatusOk) {
            continue;
        }

        /* One action per click: every board sends Press AND Short for one click, so handling
         * both ran each command twice (OK opened the menu and picked "Check" at once, Back
         * closed the menu and quit). Act on Short only; arrows also auto-repeat. */
        const bool is_arrow = event.key == InputKeyLeft || event.key == InputKeyRight ||
                              event.key == InputKeyUp || event.key == InputKeyDown;
        if(event.type != InputTypeShort && !(is_arrow && event.type == InputTypeRepeat)) {
            continue;
        }
        /* Up/Down pick the action too: the T-Embed wheel only sends Up/Down. */
        if(event.key == InputKeyUp) event.key = InputKeyLeft;
        if(event.key == InputKeyDown) event.key = InputKeyRight;

        if(event.key == InputKeyBack) {
            if(app->show_menu) {
                app->show_menu = false;
            } else {
                running = false;
            }
        } else if(event.key == InputKeyOk) {
            if(app->player_folded) {
                reset_round(app);
            } else if(app->show_menu) {
                select_action(app);
            } else {
                app->show_menu = true;
            }
        } else if(app->show_menu && event.key == InputKeyLeft) {
            if(app->selected_action == 0) {
                app->selected_action = HoldemActionCount - 1;
            } else {
                app->selected_action--;
            }
        } else if(app->show_menu && event.key == InputKeyRight) {
            app->selected_action = (app->selected_action + 1) % HoldemActionCount;
        }

        view_port_update(app->view_port);
    }

    gui_remove_view_port(gui, app->view_port);
    view_port_free(app->view_port);
    furi_message_queue_free(app->input_queue);
    furi_record_close(RECORD_GUI);
    free(app);

    return 0;
}
