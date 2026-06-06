#include "desktop_view_mesh_clients.h"

#include <furi.h>
#include <input/input.h>
#include <gui/elements.h>
#include <string.h>

#include "../desktop_i.h" /* STATUS_BAR_Y_SHIFT */
#include "mesh_view_common.h"

#define LIST_VISIBLE 3
#define ROW_H        12 /* Zeilenhöhe in der Liste */
#define NAME_MAX_CHARS 9 /* gekürzt damit Status/Action rechts passt */

#define STATUS_MAX_CHARS 12

typedef struct {
    MeshPeer peer;
    bool paired;
    char status[STATUS_MAX_CHARS + 1]; /* nur für gepairte Einträge */
} MeshClientsRow;

typedef struct {
    MeshClientsRow rows[MESH_CLIENTS_MAX];
    uint8_t count;
    uint8_t selected;
    uint8_t top; /* Scroll-Offset */
    bool pairing;
    char overlay[24];
} MeshClientsModel;

struct DesktopMeshClientsView {
    View* view;
    DesktopMeshClientsViewCallback callback;
    void* context;
};

static void mesh_clients_scroll_to(MeshClientsModel* m, uint8_t idx) {
    if(idx < m->top) {
        m->top = idx;
    } else if(idx >= m->top + LIST_VISIBLE) {
        m->top = idx - LIST_VISIBLE + 1;
    }
}

static void draw_callback(Canvas* canvas, void* model) {
    MeshClientsModel* m = model;

    canvas_clear(canvas);

    /* Header */
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(
        canvas, 64, 2 + STATUS_BAR_Y_SHIFT, AlignCenter, AlignTop, "Clients");
    canvas_draw_line(
        canvas, 8, 14 + STATUS_BAR_Y_SHIFT, 120, 14 + STATUS_BAR_Y_SHIFT);

    /* Liste */
    canvas_set_font(canvas, FontSecondary);
    if(m->count == 0) {
        canvas_draw_str_aligned(
            canvas,
            64,
            32 + STATUS_BAR_Y_SHIFT,
            AlignCenter,
            AlignCenter,
            "(no clients yet)");
    } else {
        for(uint8_t row = 0; row < LIST_VISIBLE; ++row) {
            uint8_t i = m->top + row;
            if(i >= m->count) break;

            const int y_top = 17 + STATUS_BAR_Y_SHIFT + (row * ROW_H);
            const int y_text = y_top + (ROW_H / 2);

            bool sel = (i == m->selected);
            if(sel) {
                canvas_set_color(canvas, ColorBlack);
                canvas_draw_box(canvas, 0, y_top, 128, ROW_H);
                canvas_set_color(canvas, ColorWhite);
            } else {
                canvas_set_color(canvas, ColorBlack);
            }

            /* Name links — Quelle ist max MESH_NAME_MAX (32) lang, wir kürzen
             * mit memcpy auf NAME_MAX_CHARS damit "[remove]" rechts passt. */
            char name_buf[NAME_MAX_CHARS + 1];
            size_t src_len = strnlen(m->rows[i].peer.name, MESH_NAME_MAX);
            size_t cp = src_len > NAME_MAX_CHARS ? NAME_MAX_CHARS : src_len;
            memcpy(name_buf, m->rows[i].peer.name, cp);
            name_buf[cp] = '\0';
            canvas_draw_str_aligned(canvas, 4, y_text, AlignLeft, AlignCenter, name_buf);

            /* Rechts: gepairt → Status (Idle/Action-Name), sonst "[pair]". */
            const char* right;
            if(m->rows[i].paired) {
                right = m->rows[i].status[0] ? m->rows[i].status : "Idle";
            } else {
                right = "[pair]";
            }
            canvas_draw_str_aligned(canvas, 124, y_text, AlignRight, AlignCenter, right);
        }
        canvas_set_color(canvas, ColorBlack);
    }

    /* Footer: nur während Pairing ("Discovery"-Anzeige im Idle entfernt). */
    if(m->pairing) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 60, AlignCenter, AlignBottom, "Wait for Accept");
    }

    mesh_view_draw_overlay(canvas, m->overlay);
}

static bool input_callback(InputEvent* event, void* context) {
    DesktopMeshClientsView* v = context;
    bool consumed = false;
    bool update = false;
    DesktopEvent fire = 0;
    bool should_fire = false;

    with_view_model(
        v->view,
        MeshClientsModel * m,
        {
            bool short_rep = (event->type == InputTypeShort || event->type == InputTypeRepeat);
            if(short_rep && event->key == InputKeyUp && m->count > 0) {
                if(m->selected == 0) m->selected = m->count - 1;
                else m->selected--;
                mesh_clients_scroll_to(m, m->selected);
                update = true;
                consumed = true;
            } else if(short_rep && event->key == InputKeyDown && m->count > 0) {
                if(m->selected + 1 >= m->count) m->selected = 0;
                else m->selected++;
                mesh_clients_scroll_to(m, m->selected);
                update = true;
                consumed = true;
            } else if(
                event->type == InputTypeShort && event->key == InputKeyOk && m->count > 0 &&
                !m->pairing) {
                /* OK kurz: gepairt → Action-Scene, ungepairt → pairen. */
                fire = m->rows[m->selected].paired ? DesktopMeshClientsEventOpenAction :
                                                     DesktopMeshClientsEventPair;
                should_fire = true;
                consumed = true;
            } else if(
                event->type == InputTypeLong && event->key == InputKeyOk && m->count > 0 &&
                !m->pairing) {
                /* OK lang: gepairten Client entfernen. */
                if(m->rows[m->selected].paired) {
                    fire = DesktopMeshClientsEventRemove;
                    should_fire = true;
                }
                consumed = true;
            } else if(event->type == InputTypeShort && event->key == InputKeyBack) {
                fire = DesktopMeshClientsEventBack;
                should_fire = true;
                consumed = true;
            }
        },
        update);

    if(should_fire && v->callback) v->callback(fire, v->context);
    return consumed;
}

DesktopMeshClientsView* desktop_mesh_clients_alloc(void) {
    DesktopMeshClientsView* v = malloc(sizeof(DesktopMeshClientsView));
    v->view = view_alloc();
    v->callback = NULL;
    v->context = NULL;

    view_allocate_model(v->view, ViewModelTypeLocking, sizeof(MeshClientsModel));
    view_set_context(v->view, v);
    view_set_draw_callback(v->view, draw_callback);
    view_set_input_callback(v->view, input_callback);

    with_view_model(
        v->view,
        MeshClientsModel * m,
        {
            memset(m, 0, sizeof(*m));
        },
        true);
    return v;
}

void desktop_mesh_clients_free(DesktopMeshClientsView* v) {
    furi_assert(v);
    view_free(v->view);
    free(v);
}

View* desktop_mesh_clients_get_view(DesktopMeshClientsView* v) {
    furi_assert(v);
    return v->view;
}

void desktop_mesh_clients_set_callback(
    DesktopMeshClientsView* v,
    DesktopMeshClientsViewCallback callback,
    void* context) {
    furi_assert(v);
    v->callback = callback;
    v->context = context;
}

void desktop_mesh_clients_set_peers(
    DesktopMeshClientsView* v,
    const MeshPeer* peers,
    const bool* paired,
    const char* const* status,
    size_t count) {
    furi_assert(v);
    if(count > MESH_CLIENTS_MAX) count = MESH_CLIENTS_MAX;
    with_view_model(
        v->view,
        MeshClientsModel * m,
        {
            for(size_t i = 0; i < count; ++i) {
                m->rows[i].peer = peers[i];
                m->rows[i].paired = paired[i];
                const char* st = (status && status[i]) ? status[i] : "";
                strncpy(m->rows[i].status, st, STATUS_MAX_CHARS);
                m->rows[i].status[STATUS_MAX_CHARS] = '\0';
            }
            m->count = (uint8_t)count;
            if(m->selected >= m->count) m->selected = m->count ? m->count - 1 : 0;
            mesh_clients_scroll_to(m, m->selected);
        },
        true);
}

void desktop_mesh_clients_set_pairing(DesktopMeshClientsView* v, bool in_progress) {
    furi_assert(v);
    with_view_model(
        v->view, MeshClientsModel * m, { m->pairing = in_progress; }, true);
}

void desktop_mesh_clients_set_overlay(DesktopMeshClientsView* v, const char* text) {
    furi_assert(v);
    with_view_model(
        v->view,
        MeshClientsModel * m,
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

int desktop_mesh_clients_get_selected_idx(DesktopMeshClientsView* v) {
    furi_assert(v);
    int idx = -1;
    with_view_model(
        v->view,
        MeshClientsModel * m,
        {
            if(m->count > 0) idx = m->selected;
        },
        false);
    return idx;
}
