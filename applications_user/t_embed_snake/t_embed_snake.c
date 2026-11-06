#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <furi_hal_board_info.h>

#define GRID_W 20
#define GRID_H 10
#define CELL_SIZE 5
#define FIELD_X 14
#define FIELD_Y 11
#define MAX_SNAKE (GRID_W * GRID_H)

typedef struct {
    int8_t x;
    int8_t y;
} SnakePoint;

typedef enum {
    SnakeDirUp,
    SnakeDirRight,
    SnakeDirDown,
    SnakeDirLeft,
} SnakeDirection;

typedef enum {
    SnakeEventTick,
    SnakeEventInput,
} SnakeEventType;

typedef struct {
    SnakeEventType type;
    InputEvent input;
} SnakeEvent;

typedef struct {
    SnakePoint body[MAX_SNAKE];
    uint16_t length;

    SnakePoint food;
    SnakeDirection direction;

    bool running;
    bool game_over;
    bool exit_requested;

    uint32_t score;
    uint32_t random_state;
    uint8_t move_tick;
    uint8_t move_interval;
    bool turn_locked;

    FuriMutex* mutex;
} SnakeGame;

static uint32_t snake_random(SnakeGame* game) {
    game->random_state =
        game->random_state * 1664525UL + 1013904223UL;
    return game->random_state;
}

static bool snake_occupies(
    const SnakeGame* game,
    int8_t x,
    int8_t y) {
    for(uint16_t i = 0; i < game->length; i++) {
        if(game->body[i].x == x && game->body[i].y == y) {
            return true;
        }
    }

    return false;
}

static void snake_place_food(SnakeGame* game) {
    for(uint16_t attempt = 0; attempt < 500; attempt++) {
        int8_t x = snake_random(game) % GRID_W;
        int8_t y = snake_random(game) % GRID_H;

        if(!snake_occupies(game, x, y)) {
            game->food.x = x;
            game->food.y = y;
            return;
        }
    }

    game->food.x = 0;
    game->food.y = 0;
}

static void snake_reset(SnakeGame* game) {
    memset(game->body, 0, sizeof(game->body));

    game->length = 4;

    game->body[0].x = 10;
    game->body[0].y = 5;

    game->body[1].x = 9;
    game->body[1].y = 5;

    game->body[2].x = 8;
    game->body[2].y = 5;

    game->body[3].x = 7;
    game->body[3].y = 5;

    game->direction = SnakeDirRight;
    game->running = false;
    game->game_over = false;
    game->score = 0;
    game->move_tick = 0;
    game->move_interval = 5;
    game->turn_locked = false;

    snake_place_food(game);
}

static void snake_turn_left(SnakeGame* game) {
    if(game->direction == SnakeDirUp) {
        game->direction = SnakeDirLeft;
    } else {
        game->direction--;
    }
}

static void snake_turn_right(SnakeGame* game) {
    game->direction =
        (SnakeDirection)((game->direction + 1) % 4);
}

static void snake_step(SnakeGame* game) {
    if(!game->running || game->game_over) return;

    SnakePoint next = game->body[0];

    switch(game->direction) {
    case SnakeDirUp:
        next.y--;
        break;

    case SnakeDirRight:
        next.x++;
        break;

    case SnakeDirDown:
        next.y++;
        break;

    case SnakeDirLeft:
        next.x--;
        break;
    }

    if(next.x < 0) next.x = GRID_W - 1;
    if(next.x >= GRID_W) next.x = 0;
    if(next.y < 0) next.y = GRID_H - 1;
    if(next.y >= GRID_H) next.y = 0;

    bool eating =
        next.x == game->food.x &&
        next.y == game->food.y;

    uint16_t collision_length =
        eating ? game->length : game->length - 1;

    for(uint16_t i = 0; i < collision_length; i++) {
        if(game->body[i].x == next.x &&
           game->body[i].y == next.y) {
            game->game_over = true;
            game->running = false;
            return;
        }
    }

    uint16_t last =
        eating && game->length < MAX_SNAKE ?
            game->length :
            game->length - 1;

    for(uint16_t i = last; i > 0; i--) {
        game->body[i] = game->body[i - 1];
    }

    game->body[0] = next;

    if(eating) {
        if(game->length < MAX_SNAKE) {
            game->length++;
        }

        game->score += 10;

        if(game->move_interval > 2 &&
           game->score % 50 == 0) {
            game->move_interval--;
        }

        snake_place_food(game);
    }

    game->turn_locked = false;
}

static void snake_draw_callback(
    Canvas* canvas,
    void* context) {
    SnakeGame* game = context;

    if(furi_mutex_acquire(
           game->mutex,
           25) != FuriStatusOk) {
        return;
    }

    canvas_clear(canvas);

    char score_text[24];
    snprintf(
        score_text,
        sizeof(score_text),
        "SCORE %lu",
        (unsigned long)game->score);

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 8, "SNAKE");
    canvas_draw_str(canvas, 70, 8, score_text);

    // outer and inner border
    canvas_draw_frame(
        canvas,
        FIELD_X - 3,
        FIELD_Y - 3,
        GRID_W * CELL_SIZE + 6,
        GRID_H * CELL_SIZE + 6);

    canvas_draw_frame(
        canvas,
        FIELD_X - 1,
        FIELD_Y - 1,
        GRID_W * CELL_SIZE + 2,
        GRID_H * CELL_SIZE + 2);

    // food
    uint8_t food_px = FIELD_X + game->food.x * CELL_SIZE;
    uint8_t food_py = FIELD_Y + game->food.y * CELL_SIZE;

    canvas_draw_frame(
        canvas,
        food_px,
        food_py,
        CELL_SIZE,
        CELL_SIZE);

    canvas_draw_box(
        canvas,
        food_px + 1,
        food_py + 1,
        CELL_SIZE - 2,
        CELL_SIZE - 2);

    canvas_set_color(canvas, ColorWhite);
    canvas_draw_dot(canvas, food_px + 2, food_py + 2);
    canvas_set_color(canvas, ColorBlack);

    // snake
    for(uint16_t i = 0; i < game->length; i++) {
        uint8_t px = FIELD_X + game->body[i].x * CELL_SIZE;
        uint8_t py = FIELD_Y + game->body[i].y * CELL_SIZE;

        if(i == 0) {
            // head
            canvas_draw_box(canvas, px, py, CELL_SIZE, CELL_SIZE);

            canvas_set_color(canvas, ColorWhite);

            switch(game->direction) {
            case SnakeDirUp:
                canvas_draw_dot(canvas, px + 1, py + 1);
                canvas_draw_dot(canvas, px + 3, py + 1);
                break;
            case SnakeDirRight:
                canvas_draw_dot(canvas, px + 3, py + 1);
                canvas_draw_dot(canvas, px + 3, py + 3);
                break;
            case SnakeDirDown:
                canvas_draw_dot(canvas, px + 1, py + 3);
                canvas_draw_dot(canvas, px + 3, py + 3);
                break;
            case SnakeDirLeft:
                canvas_draw_dot(canvas, px + 1, py + 1);
                canvas_draw_dot(canvas, px + 1, py + 3);
                break;
            }

            canvas_set_color(canvas, ColorBlack);
        } else if(i == game->length - 1) {
            // tail
            canvas_draw_frame(canvas, px, py, CELL_SIZE, CELL_SIZE);
            canvas_draw_box(canvas, px + 1, py + 1, 2, 2);
        } else {
            // body
            canvas_draw_frame(canvas, px, py, CELL_SIZE, CELL_SIZE);
            canvas_draw_box(canvas, px + 1, py + 1, CELL_SIZE - 2, CELL_SIZE - 2);
        }
    }

    // overlays
    if(game->game_over) {
        canvas_draw_box(canvas, 24, 20, 80, 24);
        canvas_set_color(canvas, ColorWhite);
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 33, 32, "GAME OVER");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 28, 41, "Press wheel");
        canvas_set_color(canvas, ColorBlack);
    } else if(!game->running) {
        if(game->score == 0) {
            canvas_draw_box(canvas, 24, 20, 80, 24);
            canvas_set_color(canvas, ColorWhite);
            canvas_set_font(canvas, FontPrimary);
            canvas_draw_str(canvas, 34, 32, "SNAKE");
            canvas_set_font(canvas, FontSecondary);
            canvas_draw_str(canvas, 26, 41, "Press wheel");
            canvas_set_color(canvas, ColorBlack);
        } else {
            canvas_draw_box(canvas, 32, 21, 64, 22);
            canvas_set_color(canvas, ColorWhite);
            canvas_set_font(canvas, FontPrimary);
            canvas_draw_str(canvas, 44, 34, "PAUSED");
            canvas_set_color(canvas, ColorBlack);
        }
    }

    furi_mutex_release(game->mutex);
}

static void snake_input_callback(
    InputEvent* input,
    void* context) {
    FuriMessageQueue* queue = context;

    SnakeEvent event = {
        .type = SnakeEventInput,
        .input = *input,
    };

    furi_message_queue_put(
        queue,
        &event,
        0);
}

static void snake_timer_callback(void* context) {
    FuriMessageQueue* queue = context;

    SnakeEvent event = {
        .type = SnakeEventTick,
    };

    furi_message_queue_put(
        queue,
        &event,
        0);
}

int32_t t_embed_snake_app(void* context) {
    UNUSED(context);

    SnakeGame* game = malloc(sizeof(SnakeGame));
    memset(game, 0, sizeof(SnakeGame));

    game->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    game->random_state = furi_get_tick() ^ 0x53A91EUL;

    snake_reset(game);

    FuriMessageQueue* queue =
        furi_message_queue_alloc(
            16,
            sizeof(SnakeEvent));

    ViewPort* viewport = view_port_alloc();

    view_port_draw_callback_set(
        viewport,
        snake_draw_callback,
        game);

    view_port_input_callback_set(
        viewport,
        snake_input_callback,
        queue);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(
        gui,
        viewport,
        GuiLayerFullscreen);

    FuriTimer* timer = furi_timer_alloc(
        snake_timer_callback,
        FuriTimerTypePeriodic,
        queue);

    furi_timer_start(
        timer,
        furi_kernel_get_tick_frequency() / 20);

    /* Paradoxt: real D-pad, arrows steer absolutely (one Press = one turn). Other boards keep
     * the relative wheel steering (Up/Left = turn left, Down/Right = turn right). */
    const bool dpad = furi_hal_board_get_input_layout() == FuriHalBoardInputLayoutDpad;

    while(!game->exit_requested) {
        SnakeEvent event;

        if(furi_message_queue_get(
               queue,
               &event,
               FuriWaitForever) != FuriStatusOk) {
            continue;
        }

        if(furi_mutex_acquire(
               game->mutex,
               FuriWaitForever) != FuriStatusOk) {
            continue;
        }

        if(event.type == SnakeEventTick) {
            game->move_tick++;

            if(game->move_tick >= game->move_interval) {
                game->move_tick = 0;
                snake_step(game);
            }
        } else if(event.type == SnakeEventInput) {
            const bool dpad_arrow = dpad && event.input.key <= InputKeyLeft;
            if(dpad_arrow) {
                if(event.input.type == InputTypePress && !game->game_over &&
                   !game->turn_locked) {
                    static const SnakeDirection dpad_direction[] = {
                        [InputKeyUp] = SnakeDirUp,
                        [InputKeyDown] = SnakeDirDown,
                        [InputKeyRight] = SnakeDirRight,
                        [InputKeyLeft] = SnakeDirLeft,
                    };
                    const SnakeDirection wanted = dpad_direction[event.input.key];
                    /* No reversal into the body, no-op on the current direction. */
                    if(wanted != game->direction &&
                       wanted != (SnakeDirection)((game->direction + 2) % 4)) {
                        game->direction = wanted;
                        game->turn_locked = true;
                    }
                }
            } else if(event.input.type == InputTypeShort) {
                if(event.input.key == InputKeyUp ||
                   event.input.key == InputKeyLeft) {
                    if(!game->game_over &&
                       !game->turn_locked) {
                        snake_turn_left(game);
                        game->turn_locked = true;
                    }
                } else if(
                    event.input.key == InputKeyDown ||
                    event.input.key == InputKeyRight) {
                    if(!game->game_over &&
                       !game->turn_locked) {
                        snake_turn_right(game);
                        game->turn_locked = true;
                    }
                } else if(event.input.key == InputKeyOk) {
                    if(game->game_over) {
                        snake_reset(game);
                        game->running = true;
                    } else {
                        game->running = !game->running;
                    }
                } else if(event.input.key == InputKeyBack) {
                    game->exit_requested = true;
                }
            }

            if(event.input.key == InputKeyBack &&
               event.input.type == InputTypeLong) {
                game->exit_requested = true;
            }
        }

        furi_mutex_release(game->mutex);
        view_port_update(viewport);
    }

    furi_timer_stop(timer);
    furi_timer_free(timer);

    view_port_enabled_set(viewport, false);
    gui_remove_view_port(gui, viewport);
    furi_record_close(RECORD_GUI);

    view_port_free(viewport);
    furi_message_queue_free(queue);
    furi_mutex_free(game->mutex);
    free(game);

    return 0;
}
