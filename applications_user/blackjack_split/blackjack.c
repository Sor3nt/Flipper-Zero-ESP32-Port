
#include <gui/gui.h>
#include <stdlib.h>
#include <dolphin/dolphin.h>
#include <dialogs/dialogs.h>
#include <gui/canvas_i.h>

#include <math.h>
#include "util.h"
#include "defines.h"
#include "common/card.h"
#include "common/dml.h"
#include "common/queue.h"
#include "util.h"
#include "ui.h"

#include "blackjack_icons.h"
#include <furi_hal_board_info.h>

#define DEALER_MAX 17

/*
 * T-Embed control state:
 * false = wheel moves through Settings rows
 * true  = wheel changes the selected Settings value
 */
static bool blackjack_settings_adjust_mode = false;
static bool blackjack_settings_loaded = false;

/* Paradoxt: real D-pad, stock Blackjack controls (arrows mean what they show, Back leaves
 * Settings or quits). Every other board keeps the wheel controls below. */
static bool blackjack_dpad = false;

static bool blackjack_dpad_press(GameState* game_state, InputKey key, bool* processing) {
    switch(key) {
    case InputKeyUp:
        game_state->selectDirection = DirectionUp;
        return true;
    case InputKeyDown:
        game_state->selectDirection = DirectionDown;
        return true;
    case InputKeyBack:
        if(game_state->state == GameStateSettings) {
            game_state->state = GameStateStart;
        } else {
            *processing = false;
        }
        return true;
    case InputKeyOk:
        /* Settings values change with Left/Right: OK has nothing to toggle there. */
        return game_state->state == GameStateSettings;
    default:
        return false; /* Left/Right: same as every board */
    }
}

static uint8_t blackjack_card_value(Card card) {
    if(card.character == 12) return 11; /* Ace */
    if(card.character > 8) return 10;   /* 10, J, Q, K */
    return card.character + 2;
}

static bool blackjack_can_split(const GameState* game_state) {
    if(game_state->player_card_count != 2) return false;
    if(game_state->split_active) return false;
    if(game_state->player_score < game_state->bet) return false;

    return blackjack_card_value(game_state->player_cards[0]) ==
           blackjack_card_value(game_state->player_cards[1]);
}

static void blackjack_refresh_split_visibility(GameState* game_state) {
    set_menu_state(game_state->menu, 0, blackjack_can_split(game_state));
}

void start_round(GameState *game_state);

void init(GameState *game_state);







static void draw_ui(Canvas* const canvas, const GameState* game_state) {
    draw_money(canvas, game_state->player_score);

    if(!game_state->queue_state.running &&
       game_state->state == GameStatePlay) {
        render_menu(game_state->menu, canvas, 2, 52);
    }
}

static void render_callback(Canvas *const canvas, void *ctx) {
    const GameState *game_state = ctx;
    furi_mutex_acquire(game_state->mutex, 25);

    if (game_state == NULL) {
        return;
    }

    canvas_set_color(canvas, ColorBlack);
    canvas_draw_frame(canvas, 0, 0, 128, 64);

    if(game_state->state == GameStateStart) {
        canvas_draw_icon(canvas, 0, 0, &I_blackjack);
    }

    if(game_state->state == GameStateGameOver) {
        canvas_draw_icon(canvas, 0, 0, &I_endscreen);
    }

    if (game_state->state == GameStatePlay || game_state->state == GameStateDealer) {
        if (game_state->state == GameStatePlay)
            draw_player_scene(canvas, game_state);
        else
            draw_dealer_scene(canvas, game_state);
        render_queue(&(game_state->queue_state), game_state, canvas);
        draw_ui(canvas, game_state);
    } else if (game_state->state == GameStateSettings) {
        settings_page(canvas, game_state);
    }

    furi_mutex_release(game_state->mutex);
}

//region card draw
Card draw_card(GameState *game_state) {
    Card c = game_state->deck.cards[game_state->deck.index];
    game_state->deck.index++;
    return c;
}


void drawPlayerCard(void *ctx) {
    GameState *game_state = ctx;
    Card c = draw_card(game_state);
    game_state->player_cards[game_state->player_card_count] = c;
    game_state->player_card_count++;
    if(game_state->player_score < game_state->settings.round_price || game_state->doubled){
        set_menu_state(game_state->menu, 0, false);
    }
}

void drawDealerCard(void *ctx) {
    GameState *game_state = ctx;
    Card c = draw_card(game_state);
    game_state->dealer_cards[game_state->dealer_card_count] = c;
    game_state->dealer_card_count++;
}
//endregion

//region queue callbacks
void to_lose_state(const void* ctx, Canvas* const canvas) {
    const GameState* game_state = ctx;
    if(game_state->settings.message_duration == 0) return;
    popup_frame(canvas);
    elements_multiline_text_aligned(canvas, 64, 26, AlignCenter, AlignCenter, "YOU LOSE");
}

void to_bust_state(const void* ctx, Canvas* const canvas) {
    const GameState* game_state = ctx;
    if(game_state->settings.message_duration == 0) return;
    popup_frame(canvas);
    elements_multiline_text_aligned(canvas, 64, 26, AlignCenter, AlignCenter, "BUST");
}

void to_draw_state(const void* ctx, Canvas* const canvas) {
    const GameState* game_state = ctx;
    if(game_state->settings.message_duration == 0) return;
    popup_frame(canvas);
    elements_multiline_text_aligned(canvas, 64, 26, AlignCenter, AlignCenter, "PUSH");
}

void to_dealer_turn(const void* ctx, Canvas* const canvas) {
    const GameState* game_state = ctx;
    if(game_state->settings.message_duration == 0) return;
    popup_frame(canvas);
    elements_multiline_text_aligned(canvas, 64, 26, AlignCenter, AlignCenter, "DEALER");
}

void to_win_state(const void* ctx, Canvas* const canvas) {
    const GameState* game_state = ctx;
    if(game_state->settings.message_duration == 0) return;
    popup_frame(canvas);
    elements_multiline_text_aligned(canvas, 64, 26, AlignCenter, AlignCenter, "YOU WIN");
}

void to_start(const void* ctx, Canvas* const canvas) {
    const GameState* game_state = ctx;
    if(game_state->settings.message_duration == 0) return;
    popup_frame(canvas);
    elements_multiline_text_aligned(canvas, 64, 26, AlignCenter, AlignCenter, "NEW ROUND");
}

void before_start(void *ctx) {
    GameState *game_state = ctx;
    game_state->dealer_card_count = 0;
    game_state->player_card_count = 0;
}


void start(void *ctx) {
    GameState *game_state = ctx;
    start_round(game_state);
}

void draw(void *ctx) {
    GameState *game_state = ctx;
    game_state->player_score += game_state->bet;
    game_state->bet = 0;
    enqueue(&(game_state->queue_state), game_state, start, before_start, to_start,
            game_state->settings.message_duration);
}

void game_over(void *ctx) {
    GameState *game_state = ctx;
    game_state->state = GameStateGameOver;
}

void lose(void *ctx) {
    GameState *game_state = ctx;
    game_state->state = GameStatePlay;
    game_state->bet = 0;
    if (game_state->player_score >= game_state->settings.round_price) {
        enqueue(&(game_state->queue_state), game_state, start, before_start, to_start,
                game_state->settings.message_duration);
    } else {
        enqueue(&(game_state->queue_state), game_state, game_over, NULL, NULL,
                0);
    }
}

void win(void *ctx) {
    dolphin_deed(DolphinDeedPluginGameWin);
    GameState *game_state = ctx;
    game_state->state = GameStatePlay;
    game_state->player_score += game_state->bet * 2;
    game_state->bet = 0;
    enqueue(&(game_state->queue_state), game_state, start, before_start, to_start,
            game_state->settings.message_duration);
}


void dealerTurn(void *ctx) {
    GameState *game_state = ctx;
    game_state->state = GameStateDealer;
}

float animationTime(const GameState *game_state){
    return (float) (furi_get_tick() - game_state->queue_state.start) /
           (float) (game_state->settings.animation_duration);
}

void dealer_card_animation(const void* ctx, Canvas* const canvas) {
    const GameState* game_state = ctx;
    float t = animationTime(game_state);

    Card animatingCard = game_state->deck.cards[game_state->deck.index];

    /*
     * The dealer's visible cards start after the face-down card.
     */
    Vector end = {
        48 + (int16_t)game_state->dealer_card_count * 13,
        2
    };

    draw_card_animation(
        animatingCard,
        (Vector){0, 64},
        (Vector){0, 32},
        end,
        t,
        true,
        canvas);
}

void dealer_back_card_animation(const void *ctx, Canvas *const canvas) {
    const GameState *game_state = ctx;
    float t = animationTime(game_state);

    Vector currentPos = quadratic_2d((Vector) {32, -CARD_HEIGHT}, (Vector) {64, 32}, (Vector) {13, 5}, t);
    draw_card_back_at(currentPos.x, currentPos.y, canvas);
}

void player_card_animation(const void* ctx, Canvas* const canvas) {
    const GameState* game_state = ctx;
    float t = animationTime(game_state);

    Card animatingCard = game_state->deck.cards[game_state->deck.index];

    /*
     * Match the final card position used by the clean table UI.
     */
    Vector end = {
        42 + (int16_t)game_state->player_card_count * 13,
        29
    };

    draw_card_animation(
        animatingCard,
        (Vector){32, -CARD_HEIGHT},
        (Vector){0, 32},
        end,
        t,
        true,
        canvas);
}
//endregion

void player_tick(GameState *game_state) {
    blackjack_refresh_split_visibility(game_state);

    uint8_t score = hand_count(game_state->player_cards, game_state->player_card_count);
    if ((game_state->doubled && score <= 21) || score == 21) {
        enqueue(&(game_state->queue_state), game_state, dealerTurn, NULL, to_dealer_turn,
                game_state->settings.message_duration);
    } else if (score > 21) {
        enqueue(&(game_state->queue_state), game_state, lose, NULL, to_bust_state,
                game_state->settings.message_duration);
    } else {
        if(game_state->selectDirection == DirectionUp || game_state->selectDirection == DirectionDown){
            move_menu(game_state->menu, game_state->selectDirection == DirectionUp ? -1 : 1);
        }

        if (game_state->selectDirection == Select){
            activate_menu(game_state->menu, game_state);

        }
    }
}

void dealer_tick(GameState *game_state) {
    uint8_t dealer_score = hand_count(game_state->dealer_cards, game_state->dealer_card_count);
    uint8_t player_score = hand_count(game_state->player_cards, game_state->player_card_count);

    if (dealer_score >= DEALER_MAX) {
        if (dealer_score > 21 || dealer_score < player_score) {
            enqueue(&(game_state->queue_state), game_state, win, NULL, to_win_state,
                    game_state->settings.message_duration);
        } else if (dealer_score > player_score) {
            enqueue(&(game_state->queue_state), game_state, lose, NULL, to_lose_state,
                    game_state->settings.message_duration);
        } else if (dealer_score == player_score) {
            enqueue(&(game_state->queue_state), game_state, draw, NULL, to_draw_state,
                    game_state->settings.message_duration);
        }
    } else {
        enqueue(&(game_state->queue_state), game_state, drawDealerCard, NULL, dealer_card_animation,
                game_state->settings.animation_duration);
    }
}

void settings_tick(GameState *game_state) {
    if (game_state->selectDirection == DirectionDown && game_state->selectedMenu < 4) {
        game_state->selectedMenu++;
    }
    if (game_state->selectDirection == DirectionUp && game_state->selectedMenu > 0) {
        game_state->selectedMenu--;
    }

    if (game_state->selectDirection == DirectionLeft || game_state->selectDirection == DirectionRight) {
        int nextScore = 0;
        switch (game_state->selectedMenu) {
            case 0:
                nextScore = game_state->settings.starting_money;
                if (game_state->selectDirection == DirectionLeft)
                    nextScore -= 10;
                else
                    nextScore += 10;
                if (nextScore >= (int) game_state->settings.round_price && nextScore < 400)
                    game_state->settings.starting_money = nextScore;
                break;
            case 1:
                nextScore = game_state->settings.round_price;
                if (game_state->selectDirection == DirectionLeft)
                    nextScore -= 10;
                else
                    nextScore += 10;
                if (nextScore >= 5 && nextScore <= (int) game_state->settings.starting_money)
                    game_state->settings.round_price = nextScore;
                break;
            case 2:
                nextScore = game_state->settings.animation_duration;
                if (game_state->selectDirection == DirectionLeft)
                    nextScore -= 100;
                else
                    nextScore += 100;
                if (nextScore >= 0 && nextScore < 2000)
                    game_state->settings.animation_duration = nextScore;
                break;
            case 3:
                nextScore = game_state->settings.message_duration;
                if (game_state->selectDirection == DirectionLeft)
                    nextScore -= 100;
                else
                    nextScore += 100;
                if (nextScore >= 0 && nextScore < 2000)
                    game_state->settings.message_duration = nextScore;
                break;
            case 4:
                game_state->settings.sound_effects = !game_state->settings.sound_effects;
            default:
                break;
        }
    }

}

void tick(GameState *game_state) {
    game_state->last_tick = furi_get_tick();
    bool queue_ran = run_queue(&(game_state->queue_state), game_state);

    switch (game_state->state) {
        case GameStateGameOver:
        case GameStateStart:
            if (game_state->selectDirection == Select)
                init(game_state);
            else if (game_state->selectDirection == DirectionRight) {
                game_state->selectedMenu = 0;
                game_state->state = GameStateSettings;
            }
            break;
        case GameStatePlay:
            if (!game_state->started) {
                game_state->selectedMenu = 0;
                game_state->started = true;
                enqueue(&(game_state->queue_state), game_state, drawDealerCard, NULL, dealer_back_card_animation,
                        game_state->settings.animation_duration);
                enqueue(&(game_state->queue_state), game_state, drawPlayerCard, NULL, player_card_animation,
                        game_state->settings.animation_duration);
                enqueue(&(game_state->queue_state), game_state, drawDealerCard, NULL, dealer_card_animation,
                        game_state->settings.animation_duration);
                enqueue(&(game_state->queue_state), game_state, drawPlayerCard, NULL, player_card_animation,
                        game_state->settings.animation_duration);
            }
            if (!queue_ran)
                player_tick(game_state);
            break;
        case GameStateDealer:
            if (!queue_ran)
                dealer_tick(game_state);
            break;
        case GameStateSettings:
            settings_tick(game_state);
            break;
        default:
            break;
    }

    game_state->selectDirection = None;

}

void start_round(GameState *game_state) {
    game_state->menu->current_menu=2;
    game_state->player_card_count = 0;
    game_state->dealer_card_count = 0;
    set_menu_state(game_state->menu, 0, false);
    game_state->menu->enabled=true;
    game_state->started = false;
    game_state->doubled = false;
    game_state->queue_state.running = true;
    shuffle_deck(&(game_state->deck));
    game_state->doubled = false;
    game_state->bet = game_state->settings.round_price;
    if (game_state->player_score < game_state->settings.round_price) {
        game_state->state = GameStateGameOver;
    } else {
        game_state->player_score -= game_state->settings.round_price;
    }
    game_state->state = GameStatePlay;
}

void init(GameState *game_state) {
    set_menu_state(game_state->menu, 0, true);
    game_state->menu->enabled=true;
    game_state->menu->current_menu=2;
    if(!blackjack_settings_loaded) {
        game_state->settings = load_settings();
        blackjack_settings_loaded = true;
    }

    game_state->last_tick = 0;
    game_state->processing = true;
    game_state->selectedMenu = 0;
    game_state->player_score = game_state->settings.starting_money;
    generate_deck(&(game_state->deck), 6);
    start_round(game_state);
}

static void input_callback(InputEvent* input_event, void* context) {
    FuriMessageQueue* event_queue = context;
    furi_assert(event_queue);

    AppEvent event = {.type = EventTypeKey, .input = *input_event};
    furi_message_queue_put(event_queue, &event, FuriWaitForever);
}

static void update_timer_callback(void* context) {
    FuriMessageQueue* event_queue = context;
    furi_assert(event_queue);

    AppEvent event = {.type = EventTypeTick};
    furi_message_queue_put(event_queue, &event, 0);
}

void doubleAction(void *state){
    GameState *game_state = state;
    if (!game_state->doubled &&  game_state->player_score >= game_state->settings.round_price) {
        game_state->player_score -= game_state->settings.round_price;
        game_state->bet += game_state->settings.round_price;
        game_state->doubled = true;
        enqueue(&(game_state->queue_state), game_state, drawPlayerCard, NULL, player_card_animation,
                game_state->settings.animation_duration);
        game_state->player_cards[game_state->player_card_count] = game_state->deck.cards[game_state->deck.index];
        uint8_t score = hand_count(game_state->player_cards, game_state->player_card_count + 1);
        if (score > 21) {
            enqueue(&(game_state->queue_state), game_state, lose, NULL, to_bust_state,
                    game_state->settings.message_duration);
        } else {
            enqueue(&(game_state->queue_state), game_state, dealerTurn, NULL, to_dealer_turn,
                    game_state->settings.message_duration);
        }
        set_menu_state(game_state->menu, 0, false);
    }
}

void splitAction(void *state) {
    /*
     * Full Split behavior is added in phase 2.
     * This action exists now so menu indexes stay stable.
     */
    UNUSED(state);
}

void hitAction(void *state){
    GameState *game_state = state;
    enqueue(&(game_state->queue_state), game_state, drawPlayerCard, NULL, player_card_animation,
            game_state->settings.animation_duration);
}
void stayAction(void *state){
    GameState *game_state = state;
    enqueue(&(game_state->queue_state), game_state, dealerTurn, NULL, to_dealer_turn,
            game_state->settings.message_duration);
}

int32_t blackjack_app(void *p) {
    blackjack_dpad = furi_hal_board_get_input_layout() == FuriHalBoardInputLayoutDpad;
    UNUSED(p);

    int32_t return_code = 0;

    FuriMessageQueue *event_queue = furi_message_queue_alloc(8, sizeof(AppEvent));
    dolphin_deed(DolphinDeedPluginGameStart);
    /*
     * calloc makes every pointer and counter start at zero.
     * This is required because generate_deck() checks deck.cards
     * before it allocates the deck.
     */
    GameState* game_state = calloc(1, sizeof(GameState));
    game_state->menu = calloc(1, sizeof(Menu));
    game_state->menu->menu_width = 40;

    /*
     * Create all menu entries before init()/start_round() uses them.
     */
    add_menu(game_state->menu, "Split", splitAction);
    add_menu(game_state->menu, "Double", doubleAction);
    add_menu(game_state->menu, "Hit", hitAction);
    add_menu(game_state->menu, "Stay", stayAction);

    /* Split remains hidden until it is legally available. */
    set_menu_state(game_state->menu, 0, false);

    set_card_graphics(&I_card_graphics);
    init(game_state);

    game_state->state = GameStateStart;

    game_state->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    if (!game_state->mutex) {
        FURI_LOG_E(APP_NAME, "cannot create mutex\r\n");
        return_code = 255;
        goto free_and_exit;
    }

    ViewPort *view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, render_callback, game_state);
    view_port_input_callback_set(view_port, input_callback, event_queue);

    FuriTimer *timer =
            furi_timer_alloc(update_timer_callback, FuriTimerTypePeriodic, event_queue);
    furi_timer_start(timer, furi_kernel_get_tick_frequency() / 25);

    Gui *gui = furi_record_open("gui");
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    AppEvent event;

    for (bool processing = true; processing;) {
        FuriStatus event_status = furi_message_queue_get(event_queue, &event, 100);
        furi_mutex_acquire(game_state->mutex, FuriWaitForever);
        if (event_status == FuriStatusOk) {
            if (event.type == EventTypeKey) {

                if (event.input.type == InputTypePress) {
                    if(!(blackjack_dpad &&
                         blackjack_dpad_press(game_state, event.input.key, &processing)))
                    switch (event.input.key) {
                        case InputKeyUp:
                            if(game_state->state == GameStateSettings &&
                               blackjack_settings_adjust_mode) {
                                /* Wheel LEFT: decrease selected value. */
                                game_state->selectDirection = DirectionLeft;
                            } else {
                                game_state->selectDirection = DirectionUp;
                            }
                            break;

                        case InputKeyDown:
                            if(game_state->state == GameStateSettings &&
                               blackjack_settings_adjust_mode) {
                                /* Wheel RIGHT: increase selected value. */
                                game_state->selectDirection = DirectionRight;
                            } else if(game_state->state == GameStateStart ||
                                      game_state->state == GameStateGameOver) {
                                /*
                                 * Wheel RIGHT from the main screen opens Settings.
                                 */
                                blackjack_settings_adjust_mode = false;
                                game_state->selectedMenu = 0;
                                game_state->selectDirection = DirectionRight;
                            } else {
                                game_state->selectDirection = DirectionDown;
                            }
                            break;

                        case InputKeyRight:
                            game_state->selectDirection = DirectionRight;
                            break;

                        case InputKeyLeft:
                            game_state->selectDirection = DirectionLeft;
                            break;

                        case InputKeyBack:
                            if(game_state->state == GameStateSettings) {
                                blackjack_settings_adjust_mode = false;
                                game_state->state = GameStateStart;
                            } else if(game_state->state == GameStateStart ||
                                      game_state->state == GameStateGameOver) {
                                blackjack_settings_adjust_mode = false;
                                game_state->selectedMenu = 0;
                                game_state->selectDirection = DirectionRight;
                            } else {
                                processing = false;
                            }
                            break;

                        case InputKeyOk:
                            if(game_state->state == GameStateSettings) {
                                blackjack_settings_adjust_mode =
                                    !blackjack_settings_adjust_mode;
                            } else {
                                game_state->selectDirection = Select;
                            }
                            break;
                        default:
                            break;
                    }
                }
            } else if (event.type == EventTypeTick) {
                tick(game_state);
                processing = game_state->processing;
            }
        } else {
            FURI_LOG_D(APP_NAME, "osMessageQueue: event timeout");
            // event timeout
        }
        view_port_update(view_port);
        furi_mutex_release(game_state->mutex);
    }


    furi_timer_free(timer);
    view_port_enabled_set(view_port, false);
    gui_remove_view_port(gui, view_port);
    furi_record_close(RECORD_GUI);
    view_port_free(view_port);
    furi_mutex_free(game_state->mutex);

    free_and_exit:
    free(game_state->deck.cards);
    free_menu(game_state->menu);
    queue_clear(&(game_state->queue_state));
    free(game_state);
    furi_message_queue_free(event_queue);

    return return_code;
}