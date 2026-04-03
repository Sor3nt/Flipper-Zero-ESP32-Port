#include "crawler_view.h"
#include <gui/canvas.h>
#include <gui/view_dispatcher.h>
#include <input/input.h>
#include <stdio.h>

static ViewDispatcher* s_crawler_vd = NULL;

static void crawler_view_draw_callback(Canvas* canvas, void* _model) {
    CrawlerModel* model = _model;
    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    char header[32];
    snprintf(header, sizeof(header), "Crawl: %.20s", model->domain);
    canvas_draw_str(canvas, 2, 10, header);
    canvas_draw_line(canvas, 0, 12, 128, 12);

    canvas_set_font(canvas, FontSecondary);
    char buf[48];

    uint32_t total_found = model->page_count + model->image_count + model->queue_pending;
    uint32_t total_done = model->page_count + model->image_count;
    snprintf(buf, sizeof(buf), "Done: %lu/%lu",
             (unsigned long)total_done, (unsigned long)total_found);
    canvas_draw_str(canvas, 2, 24, buf);

    snprintf(buf, sizeof(buf), "Images: %lu", (unsigned long)model->image_count);
    canvas_draw_str(canvas, 2, 34, buf);

    snprintf(buf, sizeof(buf), "Queue: %lu", (unsigned long)model->queue_pending);
    canvas_draw_str(canvas, 2, 44, buf);

    if(model->total_bytes < 1024) {
        snprintf(buf, sizeof(buf), "Size: %lu B", (unsigned long)model->total_bytes);
    } else {
        snprintf(buf, sizeof(buf), "Size: %lu KB", (unsigned long)(model->total_bytes / 1024));
    }
    canvas_draw_str(canvas, 2, 54, buf);

    // Current URL (truncated)
    if(model->current_url[0]) {
        canvas_draw_str(canvas, 68, 24, model->current_url);
    }

    canvas_set_font(canvas, FontPrimary);
    if(model->running) {
        canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, "OK: Stop");
    } else {
        canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, "Done");
    }
}

static bool crawler_view_input_callback(InputEvent* event, void* context) {
    UNUSED(context);
    if(event->type == InputTypeShort && event->key == InputKeyOk) {
        if(s_crawler_vd) {
            view_dispatcher_send_custom_event(s_crawler_vd, InputKeyOk);
        }
        return true;
    }
    return false;
}

View* crawler_view_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(CrawlerModel));
    view_set_draw_callback(view, crawler_view_draw_callback);
    view_set_input_callback(view, crawler_view_input_callback);
    return view;
}

void crawler_view_free(View* view) {
    view_free(view);
}

void crawler_view_set_view_dispatcher(ViewDispatcher* vd) {
    s_crawler_vd = vd;
}
