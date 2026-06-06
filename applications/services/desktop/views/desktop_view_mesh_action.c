#include "desktop_view_mesh_action.h"

#include <furi.h>
#include <input/input.h>
#include <gui/elements.h>
#include <string.h>

#include "../desktop_i.h" /* STATUS_BAR_Y_SHIFT */

#define LIST_VISIBLE   3
#define ROW_H          12
#define NAME_MAX_CHARS 14

typedef struct {
    char client_name[MESH_NAME_MAX + 1];
    MeshFeature features[MESH_FEATURES_MAX];
    uint8_t feature_count;
    uint8_t selected; /* 0 = Channel-Zeile, 1..feature_count = Feature */
    uint8_t top;
    bool loading;
    uint8_t channel; /* 0 = Auto */
    uint32_t running_mask; /* Bit i = Feature-ID i läuft */
    char status[40];
} MeshActionModel;

struct DesktopMeshActionView {
    View* view;
    DesktopMeshActionViewCallback callback;
    void* context;
};

/* Gesamtzeilen = 1 (Channel) + Features. */
static uint8_t row_count(const MeshActionModel* m) {
    return (uint8_t)(1 + m->feature_count);
}

static void scroll_to(MeshActionModel* m, uint8_t idx) {
    if(idx < m->top) {
        m->top = idx;
    } else if(idx >= m->top + LIST_VISIBLE) {
        m->top = idx - LIST_VISIBLE + 1;
    }
}

static void draw_callback(Canvas* canvas, void* model) {
    MeshActionModel* m = model;
    canvas_clear(canvas);

    /* Header: Client-Name */
    canvas_set_font(canvas, FontPrimary);
    char hdr[NAME_MAX_CHARS + 1];
    size_t hl = strnlen(m->client_name, NAME_MAX_CHARS);
    memcpy(hdr, m->client_name, hl);
    hdr[hl] = '\0';
    canvas_draw_str_aligned(canvas, 64, 2 + STATUS_BAR_Y_SHIFT, AlignCenter, AlignTop, hdr);
    canvas_draw_line(canvas, 8, 14 + STATUS_BAR_Y_SHIFT, 120, 14 + STATUS_BAR_Y_SHIFT);

    canvas_set_font(canvas, FontSecondary);

    if(m->loading) {
        canvas_draw_str_aligned(
            canvas, 64, 34 + STATUS_BAR_Y_SHIFT, AlignCenter, AlignCenter, "Loading features...");
        return;
    }

    uint8_t rows = row_count(m);
    for(uint8_t r = 0; r < LIST_VISIBLE; ++r) {
        uint8_t i = m->top + r;
        if(i >= rows) break;

        const int y_top = 17 + STATUS_BAR_Y_SHIFT + (r * ROW_H);
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
            /* Channel-Zeile (1..13) */
            char ch[20];
            snprintf(ch, sizeof(ch), "Channel: %u", m->channel);
            canvas_draw_str_aligned(canvas, 4, y_text, AlignLeft, AlignCenter, ch);
        } else {
            const MeshFeature* f = &m->features[i - 1];
            char nm[NAME_MAX_CHARS + 1];
            size_t nl = strnlen(f->name, NAME_MAX_CHARS);
            memcpy(nm, f->name, nl);
            nm[nl] = '\0';
            canvas_draw_str_aligned(canvas, 4, y_text, AlignLeft, AlignCenter, nm);
            const char* act = (m->running_mask & (1u << f->id)) ? "[stop]" : "[start]";
            canvas_draw_str_aligned(canvas, 124, y_text, AlignRight, AlignCenter, act);
        }
    }
    canvas_set_color(canvas, ColorBlack);

    /* Footer: Status-Text wenn etwas läuft, sonst Hinweis. */
    const char* footer = m->status[0] ? m->status : "OK: start/stop";
    canvas_draw_str_aligned(canvas, 64, 60, AlignCenter, AlignBottom, footer);
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
                uint8_t rows = row_count(m);
                if(event->key == InputKeyUp && rows > 0) {
                    if(m->selected == 0)
                        m->selected = rows - 1;
                    else
                        m->selected--;
                    scroll_to(m, m->selected);
                    update = true;
                    consumed = true;
                } else if(event->key == InputKeyDown && rows > 0) {
                    if(m->selected + 1 >= rows)
                        m->selected = 0;
                    else
                        m->selected++;
                    scroll_to(m, m->selected);
                    update = true;
                    consumed = true;
                } else if(event->key == InputKeyOk && !m->loading) {
                    if(m->selected == 0) {
                        /* Channel cyclen — rein intern (1..13). */
                        m->channel = (m->channel >= 13) ? 1 : (uint8_t)(m->channel + 1);
                        update = true;
                    } else {
                        fire = DesktopMeshActionEventToggle;
                        should_fire = true;
                    }
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

    with_view_model(
        v->view,
        MeshActionModel * m,
        {
            memset(m, 0, sizeof(*m));
            m->running_mask = 0;
            m->loading = true;
            m->channel = 1; /* fester Kanal 1..13 (Master folgt dem Buddy dorthin) */
        },
        true);
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

void desktop_mesh_action_set_features(
    DesktopMeshActionView* v,
    const MeshFeature* features,
    size_t count) {
    furi_assert(v);
    if(count > MESH_FEATURES_MAX) count = MESH_FEATURES_MAX;
    with_view_model(
        v->view,
        MeshActionModel * m,
        {
            for(size_t i = 0; i < count; ++i) m->features[i] = features[i];
            m->feature_count = (uint8_t)count;
            m->loading = false;
            uint8_t rows = row_count(m);
            if(m->selected >= rows) m->selected = rows ? rows - 1 : 0;
            scroll_to(m, m->selected);
        },
        true);
}

void desktop_mesh_action_set_loading(DesktopMeshActionView* v, bool loading) {
    furi_assert(v);
    with_view_model(v->view, MeshActionModel * m, { m->loading = loading; }, true);
}

void desktop_mesh_action_set_running_mask(DesktopMeshActionView* v, uint32_t mask) {
    furi_assert(v);
    with_view_model(v->view, MeshActionModel * m, { m->running_mask = mask; }, true);
}

void desktop_mesh_action_set_status(DesktopMeshActionView* v, const char* status) {
    furi_assert(v);
    with_view_model(
        v->view,
        MeshActionModel * m,
        {
            if(status) {
                strncpy(m->status, status, sizeof(m->status) - 1);
                m->status[sizeof(m->status) - 1] = '\0';
            } else {
                m->status[0] = '\0';
            }
        },
        true);
}

int desktop_mesh_action_get_selected_feature(DesktopMeshActionView* v) {
    furi_assert(v);
    int idx = -1;
    with_view_model(
        v->view,
        MeshActionModel * m,
        {
            if(m->selected > 0 && (m->selected - 1) < m->feature_count) idx = m->selected - 1;
        },
        false);
    return idx;
}

uint8_t desktop_mesh_action_get_channel(DesktopMeshActionView* v) {
    furi_assert(v);
    uint8_t ch = 0;
    with_view_model(v->view, MeshActionModel * m, { ch = m->channel; }, false);
    return ch;
}

void desktop_mesh_action_set_channel(DesktopMeshActionView* v, uint8_t channel) {
    furi_assert(v);
    if(channel < 1 || channel > 13) return;
    with_view_model(v->view, MeshActionModel * m, { m->channel = channel; }, true);
}
