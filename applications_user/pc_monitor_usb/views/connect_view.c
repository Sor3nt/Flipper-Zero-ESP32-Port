#include "connect_view.h"

void draw_connect_view(Canvas* canvas) {
    // ESP32 port: "Waiting for connection..." is wider than the 128 px screen
    canvas_draw_str(canvas, 1, 10, "Waiting for PC...");
    canvas_draw_str(canvas, 1, 21, "Run the back-end on");
    canvas_draw_str(canvas, 1, 30, "the USB-connected PC");

    canvas_draw_str(canvas, 1, 46, "Download back-end");
    canvas_draw_str(canvas, 1, 56, "app for your PC from:");

    canvas_draw_icon(canvas, 128 - 34, 64 - 34, &I_qr_33px);
}
