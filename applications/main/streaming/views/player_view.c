#include "player_view.h"

#include <furi.h>
#include <gui/elements.h>
#include <stdio.h>
#include <string.h>

struct PlayerView {
    View* view;
    PlayerViewCallback callback;
    void* callback_ctx;
};

typedef struct {
    char title[96];
    char target[24];
    uint32_t elapsed_ms;
    uint32_t duration_ms;
    uint8_t state; /* 0 idle, 1 playing, 2 paused */
    bool seekable;
    bool buffering; /* Cast session still launching on the TV */
} PlayerViewModel;

static void format_mmss(uint32_t ms, char* out, size_t out_size) {
    uint32_t total = ms / 1000;
    snprintf(out, out_size, "%02lu:%02lu", (unsigned long)(total / 60), (unsigned long)(total % 60));
}

static void player_view_draw(Canvas* canvas, void* model) {
    PlayerViewModel* m = model;

    canvas_set_font(canvas, FontPrimary);
    char title[96];
    strncpy(title, m->title[0] ? m->title : "—", sizeof(title) - 1);
    title[sizeof(title) - 1] = '\0';
    title[21] = '\0'; /* clip to the mono line width */
    canvas_draw_str(canvas, 2, 10, title);
    canvas_draw_line(canvas, 0, 12, 127, 12);

    canvas_set_font(canvas, FontSecondary);
    char elapsed[8], total[8];
    format_mmss(m->elapsed_ms, elapsed, sizeof(elapsed));
    if(m->duration_ms > 0) {
        format_mmss(m->duration_ms, total, sizeof(total));
    } else {
        strncpy(total, "--:--", sizeof(total));
    }
    canvas_draw_str(canvas, 2, 26, elapsed);
    canvas_draw_str_aligned(canvas, 126, 26, AlignRight, AlignBottom, total);

    int32_t bar_y = 30;
    int32_t bar_w = 124;
    canvas_draw_frame(canvas, 2, bar_y, bar_w, 6);
    if(m->duration_ms > 0) {
        uint32_t fill =
            (uint32_t)(((uint64_t)m->elapsed_ms * (uint32_t)(bar_w - 2)) / m->duration_ms);
        if(fill > (uint32_t)(bar_w - 2)) fill = bar_w - 2;
        canvas_draw_box(canvas, 3, bar_y + 1, fill, 4);
    }

    const char* status = m->buffering       ? "Buffering..." :
                         (m->state == 1)    ? "Playing" :
                         (m->state == 2)    ? "Paused" :
                                              "Stopped";
    canvas_draw_str(canvas, 2, 50, status);

    if(m->target[0]) {
        char tgt[sizeof(m->target)];
        strncpy(tgt, m->target, sizeof(tgt) - 1);
        tgt[sizeof(tgt) - 1] = '\0';
        tgt[13] = '\0';
        canvas_draw_str_aligned(canvas, 126, 50, AlignRight, AlignBottom, tgt);
    }

    if(m->seekable && !m->buffering) {
        elements_button_left(canvas, "-5s");
        elements_button_right(canvas, "+5s");
    }
    if(!m->buffering) {
        elements_button_center(canvas, m->state == 1 ? "Pause" : "Play");
    }
}

static bool player_view_input(InputEvent* event, void* context) {
    PlayerView* v = context;
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) {
        return false; /* let Back (long/short) fall through to navigation */
    }

    switch(event->key) {
    case InputKeyOk:
        if(v->callback) v->callback(PlayerViewEventPlayPause, v->callback_ctx);
        return true;
    case InputKeyUp:
        if(v->callback) v->callback(PlayerViewEventSeekBack, v->callback_ctx);
        return true;
    case InputKeyDown:
        if(v->callback) v->callback(PlayerViewEventSeekForward, v->callback_ctx);
        return true;
    case InputKeyBack:
        return false; /* navigation → previous scene */
    default:
        return true;
    }
}

PlayerView* player_view_alloc(void) {
    PlayerView* v = malloc(sizeof(PlayerView));
    v->callback = NULL;
    v->callback_ctx = NULL;
    v->view = view_alloc();
    view_allocate_model(v->view, ViewModelTypeLocking, sizeof(PlayerViewModel));
    view_set_context(v->view, v);
    view_set_draw_callback(v->view, player_view_draw);
    view_set_input_callback(v->view, player_view_input);
    return v;
}

void player_view_free(PlayerView* v) {
    furi_assert(v);
    view_free(v->view);
    free(v);
}

View* player_view_get_view(PlayerView* v) {
    furi_assert(v);
    return v->view;
}

void player_view_set_callback(PlayerView* v, PlayerViewCallback cb, void* ctx) {
    furi_assert(v);
    v->callback = cb;
    v->callback_ctx = ctx;
}

void player_view_update(
    PlayerView* v,
    const char* title,
    const char* target,
    uint32_t elapsed_ms,
    uint32_t duration_ms,
    uint8_t state,
    bool seekable,
    bool buffering) {
    furi_assert(v);
    with_view_model(
        v->view,
        PlayerViewModel * m,
        {
            strncpy(m->title, title ? title : "", sizeof(m->title) - 1);
            m->title[sizeof(m->title) - 1] = '\0';
            strncpy(m->target, target ? target : "", sizeof(m->target) - 1);
            m->target[sizeof(m->target) - 1] = '\0';
            m->elapsed_ms = elapsed_ms;
            m->duration_ms = duration_ms;
            m->state = state;
            m->seekable = seekable;
            m->buffering = buffering;
        },
        true);
}
