#include "desktop_view_mesh_handshake.h"

#include <furi.h>
#include <input/input.h>
#include <gui/elements.h>
#include <string.h>

#include "../desktop_i.h" /* STATUS_BAR_Y_SHIFT */
#include "mesh_view_common.h"

#define ROW_H        13
#define CH_MIN       1
#define CH_MAX       13
#define SEL_CHANNEL  0
#define SEL_BUTTON   1

typedef struct {
    char client_name[MESH_NAME_MAX + 1];
    uint8_t channel; /* Header: Buddy-Kanal */
    uint8_t cap_channel; /* 1..13 ausgewählter Capture-Kanal */
    uint8_t selected; /* 0 = Channel-Zeile, 1 = Start/Stop-Button */
    bool editing; /* Channel im Edit-Modus */
    bool capturing;
    char overlay[24];
} MeshHandshakeModel;

struct DesktopMeshHandshakeView {
    View* view;
    DesktopMeshHandshakeViewCallback callback;
    void* context;
};

static void draw_callback(Canvas* canvas, void* model) {
    MeshHandshakeModel* m = model;
    canvas_clear(canvas);

    mesh_view_draw_header(canvas, m->client_name, m->channel);
    canvas_set_font(canvas, FontSecondary);

    /* Channel-Zeile. */
    const int y_top = 17 + STATUS_BAR_Y_SHIFT;
    const int y_text = y_top + (ROW_H / 2);
    bool ch_sel = (m->selected == SEL_CHANNEL);
    if(ch_sel) {
        canvas_set_color(canvas, ColorBlack);
        canvas_draw_box(canvas, 0, y_top, 128, ROW_H);
        canvas_set_color(canvas, ColorWhite);
    } else {
        canvas_set_color(canvas, ColorBlack);
    }
    canvas_draw_str_aligned(canvas, 4, y_text, AlignLeft, AlignCenter, "Channel");
    char ch[8];
    snprintf(ch, sizeof(ch), m->editing ? "[%u]" : "<%u>", m->cap_channel);
    canvas_draw_str_aligned(canvas, 124, y_text, AlignRight, AlignCenter, ch);
    canvas_set_color(canvas, ColorBlack);

    /* Zentrierter Start/Stop-Button. */
    const int bw = 56, bh = 16;
    const int bx = (128 - bw) / 2;
    const int by = 44;
    bool bt_sel = (m->selected == SEL_BUTTON);
    if(bt_sel) {
        canvas_set_color(canvas, ColorBlack);
        canvas_draw_rbox(canvas, bx, by, bw, bh, 3);
        canvas_set_color(canvas, ColorWhite);
    } else {
        canvas_set_color(canvas, ColorBlack);
        canvas_draw_rframe(canvas, bx, by, bw, bh, 3);
    }
    canvas_draw_str_aligned(
        canvas, 64, by + bh / 2, AlignCenter, AlignCenter, m->capturing ? "Stop" : "Start");
    canvas_set_color(canvas, ColorBlack);

    mesh_view_draw_overlay(canvas, m->overlay);
}

static bool input_callback(InputEvent* event, void* context) {
    DesktopMeshHandshakeView* v = context;
    bool consumed = false;
    bool update = false;
    DesktopEvent fire = 0;
    bool should_fire = false;

    with_view_model(
        v->view,
        MeshHandshakeModel * m,
        {
            if(event->type == InputTypeShort || event->type == InputTypeRepeat) {
                if(m->editing) {
                    if(event->key == InputKeyUp) {
                        m->cap_channel = (m->cap_channel <= CH_MIN) ? CH_MAX :
                                                                      (uint8_t)(m->cap_channel - 1);
                        update = true;
                        consumed = true;
                    } else if(event->key == InputKeyDown) {
                        m->cap_channel = (m->cap_channel >= CH_MAX) ? CH_MIN :
                                                                      (uint8_t)(m->cap_channel + 1);
                        update = true;
                        consumed = true;
                    } else if(event->key == InputKeyOk || event->key == InputKeyBack) {
                        m->editing = false; /* übernehmen (Channel wird erst bei Start gesendet) */
                        update = true;
                        consumed = true;
                    }
                } else {
                    if(event->key == InputKeyUp || event->key == InputKeyDown) {
                        m->selected = (m->selected == SEL_CHANNEL) ? SEL_BUTTON : SEL_CHANNEL;
                        update = true;
                        consumed = true;
                    } else if(event->key == InputKeyOk) {
                        if(m->selected == SEL_CHANNEL) {
                            m->editing = true;
                            update = true;
                        } else {
                            fire = DesktopMeshHandshakeEventToggle;
                            should_fire = true;
                        }
                        consumed = true;
                    } else if(event->key == InputKeyBack) {
                        fire = DesktopMeshHandshakeEventBack;
                        should_fire = true;
                        consumed = true;
                    }
                }
            }
        },
        update);

    if(should_fire && v->callback) v->callback(fire, v->context);
    return consumed;
}

DesktopMeshHandshakeView* desktop_mesh_handshake_alloc(void) {
    DesktopMeshHandshakeView* v = malloc(sizeof(DesktopMeshHandshakeView));
    v->view = view_alloc();
    v->callback = NULL;
    v->context = NULL;

    view_allocate_model(v->view, ViewModelTypeLocking, sizeof(MeshHandshakeModel));
    view_set_context(v->view, v);
    view_set_draw_callback(v->view, draw_callback);
    view_set_input_callback(v->view, input_callback);

    with_view_model(
        v->view,
        MeshHandshakeModel * m,
        {
            memset(m, 0, sizeof(*m));
            m->cap_channel = 1;
        },
        true);
    return v;
}

void desktop_mesh_handshake_free(DesktopMeshHandshakeView* v) {
    furi_assert(v);
    view_free(v->view);
    free(v);
}

View* desktop_mesh_handshake_get_view(DesktopMeshHandshakeView* v) {
    furi_assert(v);
    return v->view;
}

void desktop_mesh_handshake_set_callback(
    DesktopMeshHandshakeView* v,
    DesktopMeshHandshakeViewCallback callback,
    void* context) {
    furi_assert(v);
    v->callback = callback;
    v->context = context;
}

void desktop_mesh_handshake_set_client(DesktopMeshHandshakeView* v, const char* name) {
    furi_assert(v);
    with_view_model(
        v->view,
        MeshHandshakeModel * m,
        {
            strncpy(m->client_name, name ? name : "", MESH_NAME_MAX);
            m->client_name[MESH_NAME_MAX] = '\0';
        },
        true);
}

void desktop_mesh_handshake_set_channel(DesktopMeshHandshakeView* v, uint8_t channel) {
    furi_assert(v);
    with_view_model(v->view, MeshHandshakeModel * m, { m->channel = channel; }, true);
}

void desktop_mesh_handshake_set_capture_channel(DesktopMeshHandshakeView* v, uint8_t channel) {
    furi_assert(v);
    if(channel < CH_MIN || channel > CH_MAX) return;
    with_view_model(
        v->view,
        MeshHandshakeModel * m,
        {
            if(!m->editing) m->cap_channel = channel;
        },
        true);
}

uint8_t desktop_mesh_handshake_get_capture_channel(DesktopMeshHandshakeView* v) {
    furi_assert(v);
    uint8_t ch = 1;
    with_view_model(v->view, MeshHandshakeModel * m, { ch = m->cap_channel; }, false);
    return ch;
}

void desktop_mesh_handshake_set_capturing(DesktopMeshHandshakeView* v, bool capturing) {
    furi_assert(v);
    with_view_model(v->view, MeshHandshakeModel * m, { m->capturing = capturing; }, true);
}

void desktop_mesh_handshake_set_overlay(DesktopMeshHandshakeView* v, const char* text) {
    furi_assert(v);
    with_view_model(
        v->view,
        MeshHandshakeModel * m,
        {
            if(text) {
                strncpy(m->overlay, text, sizeof(m->overlay) - 1);
                m->overlay[sizeof(m->overlay) - 1] = '\0';
            } else {
                m->overlay[0] = '\0';
            }
        },
        true);
}
