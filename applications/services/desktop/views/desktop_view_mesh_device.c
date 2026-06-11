#include "desktop_view_mesh_device.h"

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
    uint8_t selected; /* 0 = Identify, 1 = Disconnect */
    bool identify_running; /* aus running_mask */
    char overlay[24];
} MeshDeviceModel;

struct DesktopMeshDeviceView {
    View* view;
    DesktopMeshDeviceViewCallback callback;
    void* context;
};

static void draw_callback(Canvas* canvas, void* model) {
    MeshDeviceModel* m = model;
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

        if(i == 0) {
            canvas_draw_str_aligned(canvas, 4, y_text, AlignLeft, AlignCenter, "Identify");
            /* Einfacher Toggle: rechts der Zustand (OK schaltet direkt um). */
            canvas_draw_str_aligned(
                canvas, 124, y_text, AlignRight, AlignCenter, m->identify_running ? "ON" : "OFF");
        } else {
            canvas_draw_str_aligned(canvas, 4, y_text, AlignLeft, AlignCenter, "Disconnect");
        }
    }
    canvas_set_color(canvas, ColorBlack);

    mesh_view_draw_overlay(canvas, m->overlay);
}

static bool input_callback(InputEvent* event, void* context) {
    DesktopMeshDeviceView* v = context;
    bool consumed = false;
    bool update = false;
    DesktopEvent fire = 0;
    bool should_fire = false;

    with_view_model(
        v->view,
        MeshDeviceModel * m,
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
                    if(m->selected == 0) {
                        /* Identify: einfacher Toggle — direkt start/stop. */
                        fire = m->identify_running ? DesktopMeshDeviceEventIdentifyStop :
                                                     DesktopMeshDeviceEventIdentifyStart;
                    } else {
                        fire = DesktopMeshDeviceEventDisconnect;
                    }
                    should_fire = true;
                    consumed = true;
                } else if(event->key == InputKeyBack) {
                    fire = DesktopMeshDeviceEventBack;
                    should_fire = true;
                    consumed = true;
                }
            }
        },
        update);

    if(should_fire && v->callback) v->callback(fire, v->context);
    return consumed;
}

DesktopMeshDeviceView* desktop_mesh_device_alloc(void) {
    DesktopMeshDeviceView* v = malloc(sizeof(DesktopMeshDeviceView));
    v->view = view_alloc();
    v->callback = NULL;
    v->context = NULL;

    view_allocate_model(v->view, ViewModelTypeLocking, sizeof(MeshDeviceModel));
    view_set_context(v->view, v);
    view_set_draw_callback(v->view, draw_callback);
    view_set_input_callback(v->view, input_callback);

    with_view_model(v->view, MeshDeviceModel * m, { memset(m, 0, sizeof(*m)); }, true);
    return v;
}

void desktop_mesh_device_free(DesktopMeshDeviceView* v) {
    furi_assert(v);
    view_free(v->view);
    free(v);
}

View* desktop_mesh_device_get_view(DesktopMeshDeviceView* v) {
    furi_assert(v);
    return v->view;
}

void desktop_mesh_device_set_callback(
    DesktopMeshDeviceView* v,
    DesktopMeshDeviceViewCallback callback,
    void* context) {
    furi_assert(v);
    v->callback = callback;
    v->context = context;
}

void desktop_mesh_device_set_client(DesktopMeshDeviceView* v, const char* name) {
    furi_assert(v);
    with_view_model(
        v->view,
        MeshDeviceModel * m,
        {
            strncpy(m->client_name, name ? name : "", MESH_NAME_MAX);
            m->client_name[MESH_NAME_MAX] = '\0';
        },
        true);
}

void desktop_mesh_device_set_channel(DesktopMeshDeviceView* v, uint8_t channel) {
    furi_assert(v);
    with_view_model(v->view, MeshDeviceModel * m, { m->channel = channel; }, true);
}

void desktop_mesh_device_set_identify_running(DesktopMeshDeviceView* v, bool running) {
    furi_assert(v);
    with_view_model(v->view, MeshDeviceModel * m, { m->identify_running = running; }, true);
}

void desktop_mesh_device_set_overlay(DesktopMeshDeviceView* v, const char* text) {
    furi_assert(v);
    with_view_model(
        v->view,
        MeshDeviceModel * m,
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
