#include <furi.h>
#include <gui/gui.h>
#include <gui/view_port.h>

static void draw_callback(Canvas* canvas, void* ctx) {
    (void)ctx;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 10, 30, "Hello FAP!");
}

int32_t hello_fap_app(void* p) {
    (void)p;

    Gui* gui = furi_record_open(RECORD_GUI);
    ViewPort* vp = view_port_alloc();
    view_port_draw_callback_set(vp, draw_callback, NULL);
    gui_add_view_port(gui, vp, GuiLayerFullscreen);
    furi_delay_ms(5000);
    gui_remove_view_port(gui, vp);
    view_port_free(vp);
    furi_record_close(RECORD_GUI);
    return 0;
}
