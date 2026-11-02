#include "status_view.h"

void draw_status_view(Canvas* canvas, void* ctx) {
    PcMonitorApp* app = ctx;

    if(app->usb_state == UsbStateBusy || app->usb_state == UsbStateError) {
        // ESP32 port: two short lines, the screen is 128 px wide
        const bool busy = app->usb_state == UsbStateBusy;
        canvas_draw_str_aligned(
            canvas, 64, 22, AlignCenter, AlignCenter, busy ? "USB port is busy" : "USB failed");
        canvas_draw_str_aligned(
            canvas,
            64,
            36,
            AlignCenter,
            AlignCenter,
            busy ? "Stop qFlipper / USB" : "Replug cable, retry");
        return;
    }

    canvas_draw_str_aligned(
        canvas,
        64,
        32,
        AlignCenter,
        AlignCenter,
        app->usb_state == UsbStateWaiting ? "Waiting for connection..." :
        app->usb_state == UsbStateInactive ? "USB Inactive!" :
        app->usb_state == UsbStateLost     ? "Connection lost!" :
                                           "No data!");
}
