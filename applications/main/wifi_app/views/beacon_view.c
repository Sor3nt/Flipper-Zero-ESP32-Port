#include "beacon_view.h"
#include <furi.h>
#include <gui/elements.h>

typedef struct {
    uint32_t frame_count;
    char status[32];
} BeaconViewModel;

struct BeaconView {
    View* view;
};

static void beacon_view_draw_callback(Canvas* canvas, void* model) {
    BeaconViewModel* m = model;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 10, AlignCenter, AlignTop, "Beacon Spam");
    canvas_set_font(canvas, FontSecondary);
    
    char frame_str[32];
    snprintf(frame_str, sizeof(frame_str), "Frames: %lu", m->frame_count);
    canvas_draw_str(canvas, 10, 30, m->status);
    canvas_draw_str(canvas, 10, 50, frame_str);
    elements_button_center(canvas, "Stop");
}

static bool beacon_view_input_callback(InputEvent* event, void* context) {
    ViewDispatcher* vd = context;
    if(event->type == InputTypePress && event->key == InputKeyOk) {
        view_dispatcher_send_custom_event(vd, InputKeyOk);
        return true;
    }
    return false;
}

BeaconView* beacon_view_alloc(void) {
    BeaconView* beacon_view = malloc(sizeof(BeaconView));
    beacon_view->view = view_alloc();
    view_allocate_model(beacon_view->view, ViewModelTypeLockFree, sizeof(BeaconViewModel));
    view_set_draw_callback(beacon_view->view, beacon_view_draw_callback);
    view_set_input_callback(beacon_view->view, beacon_view_input_callback);
    
    BeaconViewModel* model = view_get_model(beacon_view->view);
    model->frame_count = 0;
    strcpy(model->status, "Ready");
    view_commit_model(beacon_view->view, false);
    
    return beacon_view;
}

void beacon_view_free(BeaconView* view) {
    furi_assert(view);
    view_free(view->view);
    free(view);
}

View* beacon_view_get_view(BeaconView* view) {
    return view->view;
}

void beacon_view_set_frame_count(BeaconView* view, uint32_t count) {
    with_view_model(view->view, BeaconViewModel * model, {
        model->frame_count = count;
    }, true);
}

void beacon_view_set_status(BeaconView* view, const char* status) {
    with_view_model(view->view, BeaconViewModel * model, {
        strncpy(model->status, status, sizeof(model->status) - 1);
    }, true);
}