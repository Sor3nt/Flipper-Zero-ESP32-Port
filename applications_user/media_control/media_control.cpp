#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>
#include <BleKeyboard.h>

// Initialize BLE Keyboard Instance
static BleKeyboard bleKeyboard("T-Embed Remote", "Sor3nt", 100);

typedef enum {
    MEDIA_BTN_OK = 0,
    MEDIA_BTN_UP = 1,
    MEDIA_BTN_DOWN = 2,
    MEDIA_BTN_LEFT = 3,
    MEDIA_BTN_RIGHT = 4
} MediaButton;

typedef struct {
    MediaButton selected;
    bool is_playing;
    bool is_connected;
    int action_progress; // Percentage feedback (0 to 100%)
    bool is_processing;
} MediaAppState;

// Helper to draw clean directional arrows inside the widget
static void draw_arrow_widget(Canvas* canvas, int x, int y, const char* label, bool is_selected) {
    if (is_selected) {
        // Highlighting Box Widget
        canvas_draw_box(canvas, x - 14, y - 11, 28, 22);
        canvas_set_color(canvas, ColorBlack);
    } else {
        canvas_draw_frame(canvas, x - 14, y - 11, 28, 22);
        canvas_set_color(canvas, ColorWhite);
    }
    canvas_draw_str_aligned(canvas, x, y, AlignCenter, AlignCenter, label);
    canvas_set_color(canvas, ColorWhite);
}

// GUI Rendering Callback
static void media_control_render_callback(Canvas* canvas, void* ctx) {
    MediaAppState* state = (MediaAppState*)ctx;

    // Dark Background Theme Widget
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_box(canvas, 0, 0, 320, 170);

    // Bluetooth Section Header
    canvas_set_color(canvas, ColorWhite);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 160, 10, AlignCenter, AlignTop, "BLUETOOTH MEDIA CONTROL");

    // Dynamic Connection Status
    canvas_set_font(canvas, FontSecondary);
    state->is_connected = bleKeyboard.isConnected();
    if (state->is_connected) {
        canvas_draw_str_aligned(canvas, 160, 26, AlignCenter, AlignTop, "Connected successfully!");
    } else {
        canvas_draw_str_aligned(canvas, 160, 26, AlignCenter, AlignTop, "Pairing... (Select 'T-Embed Remote')");
    }

    const int cx = 160;
    const int cy = 90;

    // 1. UP ARROW WIDGET (Vol +)
    draw_arrow_widget(canvas, cx, cy - 32, "^", state->selected == MEDIA_BTN_UP);

    // 2. DOWN ARROW WIDGET (Vol -)
    draw_arrow_widget(canvas, cx, cy + 32, "v", state->selected == MEDIA_BTN_DOWN);

    // 3. LEFT ARROW WIDGET (Prev Track)
    draw_arrow_widget(canvas, cx - 45, cy, "<", state->selected == MEDIA_BTN_LEFT);

    // 4. RIGHT ARROW WIDGET (Next Track)
    draw_arrow_widget(canvas, cx + 45, cy, ">", state->selected == MEDIA_BTN_RIGHT);

    // 5. CENTER OK BUTTON WIDGET (Select / Play / Pause / Open)
    if (state->selected == MEDIA_BTN_OK) {
        canvas_draw_box(canvas, cx - 22, cy - 11, 44, 22);
        canvas_set_color(canvas, ColorBlack);
    } else {
        canvas_draw_frame(canvas, cx - 22, cy - 11, 44, 22);
        canvas_set_color(canvas, ColorWhite);
    }
    canvas_draw_str_aligned(canvas, cx, cy, AlignCenter, AlignCenter, "OK");
    canvas_set_color(canvas, ColorWhite);

    // Fast Connection/Action Progress Bar Widget
    if (state->is_processing) {
        canvas_draw_frame(canvas, 60, 140, 200, 14);
        canvas_draw_box(canvas, 62, 142, (state->action_progress * 196) / 100, 10);
        
        char pct_str[16];
        snprintf(pct_str, sizeof(pct_str), "Sending: %d%%", state->action_progress);
        canvas_draw_str_aligned(canvas, 160, 158, AlignCenter, AlignTop, pct_str);
    } else {
        canvas_draw_str_aligned(canvas, 160, 145, AlignCenter, AlignTop, "Rotate: Navigate | Press OK: Select");
    }
}

static void media_control_input_callback(InputEvent* input_event, void* ctx) {
    FuriMessageQueue* event_queue = (FuriMessageQueue*)ctx;
    furi_message_queue_put(event_queue, input_event, FuriWaitForever);
}

// App Entry Point
extern "C" int32_t media_control_app_main(void* p) {
    UNUSED(p);
    FuriMessageQueue* event_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    
    MediaAppState state = {
        .selected = MEDIA_BTN_OK,
        .is_playing = false,
        .is_connected = false,
        .action_progress = 0,
        .is_processing = false
    };

    bleKeyboard.begin();

    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, media_control_render_callback, &state);
    view_port_input_callback_set(view_port, media_control_input_callback, event_queue);

    Gui* gui = (Gui*)furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    InputEvent event;
    bool running = true;

    while (running) {
        if (furi_message_queue_get(event_queue, &event, 50) == FuriStatusOk) {
            if (event.type == InputTypePress || event.type == InputTypeRepeat) {
                switch (event.key) {
                    // Bezel Rotate Right
                    case InputKeyRight:
                        state.selected = (MediaButton)((state.selected + 1) % 5);
                        break;
                    // Bezel Rotate Left
                    case InputKeyLeft:
                        state.selected = (MediaButton)((state.selected + 4) % 5);
                        break;
                    // Center Button Press
                    case InputKeyOk:
                        if (bleKeyboard.isConnected()) {
                            // Fast percentage animation (0% -> 100%)
                            state.is_processing = true;
                            for (int i = 0; i <= 100; i += 25) {
                                state.action_progress = i;
                                view_port_update(view_port);
                                furi_delay_ms(15); // Fast speed delay
                            }

                            // Trigger HID Command based on Widget selection
                            switch (state.selected) {
                                case MEDIA_BTN_OK:
                                    state.is_playing = !state.is_playing;
                                    bleKeyboard.write(KEY_MEDIA_PLAY_PAUSE);
                                    break;
                                case MEDIA_BTN_UP:
                                    bleKeyboard.write(KEY_MEDIA_VOLUME_UP);
                                    break;
                                case MEDIA_BTN_DOWN:
                                    bleKeyboard.write(KEY_MEDIA_VOLUME_DOWN);
                                    break;
                                case MEDIA_BTN_LEFT:
                                    bleKeyboard.write(KEY_MEDIA_PREVIOUS_TRACK);
                                    break;
                                case MEDIA_BTN_RIGHT:
                                    bleKeyboard.write(KEY_MEDIA_NEXT_TRACK);
                                    break;
                            }

                            furi_delay_ms(50);
                            state.is_processing = false;
                        }
                        break;
                    case InputKeyBack:
                        running = false;
                        break;
                    default:
                        break;
                }
            }
        }
        view_port_update(view_port);
    }

    // Clean exit
    bleKeyboard.end();
    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_message_queue_free(event_queue);
    furi_record_close(RECORD_GUI);

    return 0;
}
