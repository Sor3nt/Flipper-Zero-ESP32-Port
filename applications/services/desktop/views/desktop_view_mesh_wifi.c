#include "desktop_view_mesh_wifi.h"

#include <furi.h>
#include <input/input.h>
#include <gui/elements.h>
#include <string.h>

#include "../desktop_i.h" /* STATUS_BAR_Y_SHIFT */
#include "mesh_view_common.h"

#define ROW_H 13
#define ROW_COUNT 1

typedef struct {
    char client_name[MESH_NAME_MAX + 1];
    uint8_t channel;
    uint8_t selected; /* 0 = Capture Handshake */
    char overlay[24];
} MeshWifiModel;

struct DesktopMeshWifiView {
    View* view;
    DesktopMeshWifiViewCallback callback;
    void* context;
};

static void draw_callback(Canvas* canvas, void* model) {
    MeshWifiModel* m = model;
    canvas_clear(canvas);

    mesh_view_draw_header(canvas, m->client_name, m->channel);
    canvas_set_font(canvas, FontSecondary);

    const int y_top = 17 + STATUS_BAR_Y_SHIFT;
    const int y_text = y_top + (ROW_H / 2);
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_box(canvas, 0, y_top, 128, ROW_H);
    canvas_set_color(canvas, ColorWhite);
    canvas_draw_str_aligned(canvas, 4, y_text, AlignLeft, AlignCenter, "Capture Handshake");
    canvas_set_color(canvas, ColorBlack);

    mesh_view_draw_overlay(canvas, m->overlay);
}

static bool input_callback(InputEvent* event, void* context) {
    DesktopMeshWifiView* v = context;
    bool consumed = false;
    DesktopEvent fire = 0;
    bool should_fire = false;

    if(event->type == InputTypeShort || event->type == InputTypeRepeat) {
        if(event->key == InputKeyOk) {
            fire = DesktopMeshWifiEventCaptureHs;
            should_fire = true;
            consumed = true;
        } else if(event->key == InputKeyBack) {
            fire = DesktopMeshWifiEventBack;
            should_fire = true;
            consumed = true;
        } else if(event->key == InputKeyUp || event->key == InputKeyDown) {
            consumed = true; /* nur eine Zeile */
        }
    }

    if(should_fire && v->callback) v->callback(fire, v->context);
    return consumed;
}

DesktopMeshWifiView* desktop_mesh_wifi_alloc(void) {
    DesktopMeshWifiView* v = malloc(sizeof(DesktopMeshWifiView));
    v->view = view_alloc();
    v->callback = NULL;
    v->context = NULL;

    view_allocate_model(v->view, ViewModelTypeLocking, sizeof(MeshWifiModel));
    view_set_context(v->view, v);
    view_set_draw_callback(v->view, draw_callback);
    view_set_input_callback(v->view, input_callback);

    with_view_model(v->view, MeshWifiModel * m, { memset(m, 0, sizeof(*m)); }, true);
    return v;
}

void desktop_mesh_wifi_free(DesktopMeshWifiView* v) {
    furi_assert(v);
    view_free(v->view);
    free(v);
}

View* desktop_mesh_wifi_get_view(DesktopMeshWifiView* v) {
    furi_assert(v);
    return v->view;
}

void desktop_mesh_wifi_set_callback(
    DesktopMeshWifiView* v,
    DesktopMeshWifiViewCallback callback,
    void* context) {
    furi_assert(v);
    v->callback = callback;
    v->context = context;
}

void desktop_mesh_wifi_set_client(DesktopMeshWifiView* v, const char* name) {
    furi_assert(v);
    with_view_model(
        v->view,
        MeshWifiModel * m,
        {
            strncpy(m->client_name, name ? name : "", MESH_NAME_MAX);
            m->client_name[MESH_NAME_MAX] = '\0';
        },
        true);
}

void desktop_mesh_wifi_set_channel(DesktopMeshWifiView* v, uint8_t channel) {
    furi_assert(v);
    with_view_model(v->view, MeshWifiModel * m, { m->channel = channel; }, true);
}

void desktop_mesh_wifi_set_overlay(DesktopMeshWifiView* v, const char* text) {
    furi_assert(v);
    with_view_model(
        v->view,
        MeshWifiModel * m,
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
