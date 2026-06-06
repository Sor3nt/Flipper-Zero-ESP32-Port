#include "desktop_view_mesh_action.h"

#include <furi.h>
#include <input/input.h>
#include <gui/elements.h>
#include <string.h>

#include "../desktop_i.h" /* STATUS_BAR_Y_SHIFT */
#include "mesh_view_common.h"

#define ROW_H 13
#define ROW_COUNT 2

typedef struct {
    char client_name[MESH_NAME_MAX + 1];
    uint8_t channel;
    uint8_t selected; /* 0 = Device, 1 = Wifi */
    char label[24]; /* Idle / Action-Name */
    char overlay[24];
} MeshActionModel;

struct DesktopMeshActionView {
    View* view;
    DesktopMeshActionViewCallback callback;
    void* context;
};

static const char* const ROW_LABELS[ROW_COUNT] = {"Device", "Wifi"};

static void draw_callback(Canvas* canvas, void* model) {
    MeshActionModel* m = model;
    canvas_clear(canvas);

    mesh_view_draw_header(canvas, m->client_name, m->channel);

    canvas_set_font(canvas, FontSecondary);
    for(uint8_t i = 0; i < ROW_COUNT; ++i) {
        const int y_top = 17 + STATUS_BAR_Y_SHIFT + (i * ROW_H);
        const int y_text = y_top + (ROW_H / 2);
        bool sel = (i == m->selected);
        if(sel) {
            canvas_set_color(canvas, ColorBlack);
            canvas_draw_box(canvas, 0, y_top, 128, ROW_H);
            canvas_set_color(canvas, ColorWhite);
        } else {
            canvas_set_color(canvas, ColorBlack);
        }
        canvas_draw_str_aligned(canvas, 4, y_text, AlignLeft, AlignCenter, ROW_LABELS[i]);
    }
    canvas_set_color(canvas, ColorBlack);

    /* Aktuelle Action zentriert unten. */
    const char* label = m->label[0] ? m->label : "Idle";
    canvas_draw_str_aligned(canvas, 64, 60, AlignCenter, AlignBottom, label);

    mesh_view_draw_overlay(canvas, m->overlay);
}

static bool input_callback(InputEvent* event, void* context) {
    DesktopMeshActionView* v = context;
    bool consumed = false;
    bool update = false;
    DesktopEvent fire = 0;
    bool should_fire = false;

    with_view_model(
        v->view,
        MeshActionModel * m,
        {
            if(event->type == InputTypeShort || event->type == InputTypeRepeat) {
                if(event->key == InputKeyUp) {
                    m->selected = m->selected ? (uint8_t)(m->selected - 1) : (ROW_COUNT - 1);
                    update = true;
                    consumed = true;
                } else if(event->key == InputKeyDown) {
                    m->selected = (uint8_t)((m->selected + 1) % ROW_COUNT);
                    update = true;
                    consumed = true;
                } else if(event->key == InputKeyOk) {
                    fire = m->selected == 0 ? DesktopMeshActionEventDevice :
                                              DesktopMeshActionEventWifi;
                    should_fire = true;
                    consumed = true;
                } else if(event->key == InputKeyBack) {
                    fire = DesktopMeshActionEventBack;
                    should_fire = true;
                    consumed = true;
                }
            }
        },
        update);

    if(should_fire && v->callback) v->callback(fire, v->context);
    return consumed;
}

DesktopMeshActionView* desktop_mesh_action_alloc(void) {
    DesktopMeshActionView* v = malloc(sizeof(DesktopMeshActionView));
    v->view = view_alloc();
    v->callback = NULL;
    v->context = NULL;

    view_allocate_model(v->view, ViewModelTypeLocking, sizeof(MeshActionModel));
    view_set_context(v->view, v);
    view_set_draw_callback(v->view, draw_callback);
    view_set_input_callback(v->view, input_callback);

    with_view_model(v->view, MeshActionModel * m, { memset(m, 0, sizeof(*m)); }, true);
    return v;
}

void desktop_mesh_action_free(DesktopMeshActionView* v) {
    furi_assert(v);
    view_free(v->view);
    free(v);
}

View* desktop_mesh_action_get_view(DesktopMeshActionView* v) {
    furi_assert(v);
    return v->view;
}

void desktop_mesh_action_set_callback(
    DesktopMeshActionView* v,
    DesktopMeshActionViewCallback callback,
    void* context) {
    furi_assert(v);
    v->callback = callback;
    v->context = context;
}

void desktop_mesh_action_set_client(DesktopMeshActionView* v, const char* name) {
    furi_assert(v);
    with_view_model(
        v->view,
        MeshActionModel * m,
        {
            strncpy(m->client_name, name ? name : "", MESH_NAME_MAX);
            m->client_name[MESH_NAME_MAX] = '\0';
        },
        true);
}

void desktop_mesh_action_set_channel(DesktopMeshActionView* v, uint8_t channel) {
    furi_assert(v);
    with_view_model(v->view, MeshActionModel * m, { m->channel = channel; }, true);
}

void desktop_mesh_action_set_label(DesktopMeshActionView* v, const char* label) {
    furi_assert(v);
    with_view_model(
        v->view,
        MeshActionModel * m,
        {
            if(label) {
                strncpy(m->label, label, sizeof(m->label) - 1);
                m->label[sizeof(m->label) - 1] = '\0';
            } else {
                m->label[0] = '\0';
            }
        },
        true);
}

void desktop_mesh_action_set_overlay(DesktopMeshActionView* v, const char* text) {
    furi_assert(v);
    with_view_model(
        v->view,
        MeshActionModel * m,
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
