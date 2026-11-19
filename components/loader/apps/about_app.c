#include <furi.h>
#include <gui.h>
#include <furi_hal_board_info.h>
#include <furi_hal_diag.h>
#include <toolbox/fw_version.h>
#include <view_dispatcher.h>
#include <view.h>
#include <elements.h>
#include <text_box.h>

#include <sdkconfig.h>
#include <esp_idf_version.h>
#include <stdio.h>
#include <string.h>

/* About: four pages, flipped with Up/Down or Left/Right (encoder turn, D-pad,
 * touch swipe) or OK. Dots at the bottom show the current page.
 *  1. Project:     name, author, firmware version, GitHub link
 *  2. Hardware:    board, MCU model + revision, PCB version (if known), MAC
 *  3. Firmware:    version, build time, ESP-IDF version
 *  4. Diagnostics: uptime (live), reset reason, last crash; OK opens the crash log
 *
 * Nothing is placed at fixed x positions: lines are laid out from measured string
 * widths. Board names reach ~142 px in FontSecondary (wider than the 128 px screen)
 * and asset packs may swap the fonts, so a value that does not fit next to its
 * label moves to the line below, a too long line is split at a space or after a
 * '/', and only as a last resort is it cut with "...".
 *
 * Cost: everything except the uptime is read once when the app opens. The
 * Diagnostics page is redrawn once a second for the uptime, and only while it is
 * on screen. The crash log comes from RAM (furi_hal_diag), never from flash. */

/* Generated at build time by the top-level CMakeLists.txt (furi_build_time.c). */
extern const char furi_build_time[];

#define ABOUT_STR_(x) #x
#define ABOUT_STR(x)  ABOUT_STR_(x)
#define ABOUT_IDF_VERSION                                                        \
    "v" ABOUT_STR(ESP_IDF_VERSION_MAJOR) "." ABOUT_STR(ESP_IDF_VERSION_MINOR) "." \
        ABOUT_STR(ESP_IDF_VERSION_PATCH)

#define ABOUT_MARGIN    2 /* side margin of the body text */
#define ABOUT_LABEL_GAP 6 /* minimum space between a label and its value */
#define ABOUT_HEADER_Y  1 /* top of the page title */
#define ABOUT_BODY_GAP  3 /* rule under the title -> first body line */
#define ABOUT_LINE_MAX  64 /* longest line head copied out for measuring */
#define ABOUT_DOT_PITCH 6 /* page dots: 3x3 px, this far apart */
#define ABOUT_TICK_MS   1000 /* uptime refresh */

typedef enum {
    AboutPageProject,
    AboutPageHardware,
    AboutPageFirmware,
    AboutPageDiagnostics,
    AboutPageCount,
} AboutPage;

typedef enum {
    AboutViewPages,
    AboutViewCrashLog,
} AboutView;

/* Read once when the app opens. */
typedef struct {
    char mcu[32]; /* "ESP32-S3 rev v0.2" */
    char mac[18]; /* "AA:BB:CC:DD:EE:FF" */
    const char* pcb; /* NULL: board has no known PCB version */
    const char* reset; /* reset reason of this boot */
    char crash[48]; /* last crash, short */
    bool has_crash;
} AboutInfo;

typedef struct {
    uint8_t page;
    const AboutInfo* info;
} AboutModel;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    View* view;
    TextBox* text_box;
    FuriString* crash_log;
    AboutInfo info;
} AboutApp;

/* ------------------------------------------------------------------------ */
/* Layout helpers                                                           */
/* ------------------------------------------------------------------------ */

/* First n chars of s as a terminated string in buf (ABOUT_LINE_MAX bytes). */
static const char* about_slice(char* buf, const char* s, size_t n) {
    if(n > ABOUT_LINE_MAX - 1) n = ABOUT_LINE_MAX - 1;
    memcpy(buf, s, n);
    buf[n] = '\0';
    return buf;
}

/* Where to break text into two lines: the head must fit head_width, the tail
 * tail_width. Breaks at a space (dropped) or after a '/' (kept on the head line,
 * for the URL). Takes the latest break that works, so the head is as full as it
 * can be. False when no break fits. */
static bool about_split(
    Canvas* canvas,
    const char* text,
    uint16_t head_width,
    uint16_t tail_width,
    size_t* head_len,
    const char** tail) {
    char buf[ABOUT_LINE_MAX];
    bool found = false;

    for(size_t i = 0; text[i]; i++) {
        size_t len;
        if(text[i] == ' ') {
            len = i;
        } else if(text[i] == '/') {
            len = i + 1;
        } else {
            continue;
        }
        const char* rest = &text[i + 1];
        if(len == 0 || rest[0] == '\0') continue;
        if(len >= ABOUT_LINE_MAX) break;
        /* The head only grows from here on: once it is too wide, stop. */
        if(canvas_string_width(canvas, about_slice(buf, text, len)) > head_width) break;
        if(canvas_string_width(canvas, rest) > tail_width) continue;
        *head_len = len;
        *tail = rest;
        found = true;
    }

    return found;
}

/* One line no wider than width, cut with "..." when it does not fit. */
static void about_draw_fit(
    Canvas* canvas,
    int32_t x,
    int32_t y,
    Align horizontal,
    const char* text,
    uint16_t width) {
    if(canvas_string_width(canvas, text) <= width) {
        canvas_draw_str_aligned(canvas, x, y, horizontal, AlignTop, text);
        return;
    }
    FuriString* line = furi_string_alloc_set(text);
    elements_string_fit_width(canvas, line, width);
    canvas_draw_str_aligned(canvas, x, y, horizontal, AlignTop, furi_string_get_cstr(line));
    furi_string_free(line);
}

/* Page title centred on top with a rule under it. Returns the y of the body. */
static int32_t about_draw_header(Canvas* canvas, const char* title) {
    const int32_t width = canvas_width(canvas);
    const CanvasFontParameters* font = canvas_get_font_params(canvas, FontPrimary);

    canvas_set_font(canvas, FontPrimary);
    about_draw_fit(canvas, width / 2, ABOUT_HEADER_Y, AlignCenter, title, width);

    const int32_t rule_y = ABOUT_HEADER_Y + font->height + font->descender + 1;
    canvas_draw_line(canvas, 0, rule_y, width - 1, rule_y);
    return rule_y + ABOUT_BODY_GAP;
}

/* Body line distance: the font's tightest leading plus 1 px of air. */
static uint8_t about_body_pitch(Canvas* canvas) {
    return canvas_get_font_params(canvas, FontSecondary)->leading_min + 1;
}

/* Centred text on one line, or split over two. Returns the lines used. */
static uint8_t about_draw_centered(Canvas* canvas, int32_t y, uint8_t pitch, const char* text) {
    const int32_t center = canvas_width(canvas) / 2;
    const uint16_t width = canvas_width(canvas) - 2 * ABOUT_MARGIN;
    size_t head_len;
    const char* tail;

    if(canvas_string_width(canvas, text) > width &&
       about_split(canvas, text, width, width, &head_len, &tail)) {
        char buf[ABOUT_LINE_MAX];
        canvas_draw_str_aligned(
            canvas, center, y, AlignCenter, AlignTop, about_slice(buf, text, head_len));
        canvas_draw_str_aligned(canvas, center, y + pitch, AlignCenter, AlignTop, tail);
        return 2;
    }

    about_draw_fit(canvas, center, y, AlignCenter, text, width);
    return 1;
}

/* Room for a value right of the label. */
static uint16_t about_beside_width(Canvas* canvas, const char* label) {
    const uint16_t full = canvas_width(canvas) - 2 * ABOUT_MARGIN;
    const uint16_t label_width = canvas_string_width(canvas, label);
    return (label_width + ABOUT_LABEL_GAP < full) ? full - label_width - ABOUT_LABEL_GAP : 0;
}

/* "Label          value" row, value right-aligned. A value too wide for the room
 * next to the label goes right-aligned on the line below; one too wide even for
 * a full line is split over both lines. Returns the lines used. */
static uint8_t
    about_draw_field(Canvas* canvas, int32_t y, uint8_t pitch, const char* label, const char* value) {
    const int32_t right = canvas_width(canvas) - ABOUT_MARGIN;
    const uint16_t full = canvas_width(canvas) - 2 * ABOUT_MARGIN;
    const uint16_t beside = about_beside_width(canvas, label);
    const uint16_t value_width = canvas_string_width(canvas, value);

    canvas_draw_str_aligned(canvas, ABOUT_MARGIN, y, AlignLeft, AlignTop, label);

    if(value_width <= beside) {
        canvas_draw_str_aligned(canvas, right, y, AlignRight, AlignTop, value);
        return 1;
    }

    if(value_width <= full) {
        canvas_draw_str_aligned(canvas, right, y + pitch, AlignRight, AlignTop, value);
        return 2;
    }

    size_t head_len;
    const char* tail;
    if(about_split(canvas, value, beside, full, &head_len, &tail)) {
        char buf[ABOUT_LINE_MAX];
        canvas_draw_str_aligned(
            canvas, right, y, AlignRight, AlignTop, about_slice(buf, value, head_len));
        canvas_draw_str_aligned(canvas, right, y + pitch, AlignRight, AlignTop, tail);
        return 2;
    }

    about_draw_fit(canvas, right, y + pitch, AlignRight, value, full);
    return 2;
}

/* "Label          value" on exactly one line: the value is cut with "..." when
 * it does not fit (the Diagnostics page has no line to spare). */
static void about_draw_field_line(Canvas* canvas, int32_t y, const char* label, const char* value) {
    canvas_draw_str_aligned(canvas, ABOUT_MARGIN, y, AlignLeft, AlignTop, label);
    about_draw_fit(
        canvas,
        canvas_width(canvas) - ABOUT_MARGIN,
        y,
        AlignRight,
        value,
        about_beside_width(canvas, label));
}

/* Page indicator at the bottom centre: filled dot = current page. */
static void about_draw_page_dots(Canvas* canvas, uint8_t page) {
    const int32_t y = canvas_height(canvas) - 3;
    const int32_t x0 = canvas_width(canvas) / 2 - (AboutPageCount - 1) * ABOUT_DOT_PITCH / 2;

    for(uint8_t i = 0; i < AboutPageCount; i++) {
        const int32_t x = x0 + i * ABOUT_DOT_PITCH;
        if(i == page) {
            canvas_draw_box(canvas, x - 1, y - 1, 3, 3);
        } else {
            canvas_draw_frame(canvas, x - 1, y - 1, 3, 3);
        }
    }
}

/* Writes value in decimal, zero-padded to min_digits. No printf: this runs in
 * the GUI thread's draw callback every second. */
static char* about_put_uint(char* out, uint32_t value, uint8_t min_digits) {
    char digits[10];
    uint8_t count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while(value && count < sizeof(digits));
    while(count < min_digits && count < sizeof(digits)) {
        digits[count++] = '0';
    }
    while(count) {
        *out++ = digits[--count];
    }
    return out;
}

/* "3d 14h 22m 05s". buffer: at least 32 bytes. */
static void about_format_duration(char* buffer, uint64_t seconds) {
    char* out = buffer;
    out = about_put_uint(out, (uint32_t)(seconds / 86400), 1);
    *out++ = 'd';
    *out++ = ' ';
    out = about_put_uint(out, (uint32_t)((seconds / 3600) % 24), 2);
    *out++ = 'h';
    *out++ = ' ';
    out = about_put_uint(out, (uint32_t)((seconds / 60) % 60), 2);
    *out++ = 'm';
    *out++ = ' ';
    out = about_put_uint(out, (uint32_t)(seconds % 60), 2);
    *out++ = 's';
    *out = '\0';
}

/* ------------------------------------------------------------------------ */
/* Pages                                                                    */
/* ------------------------------------------------------------------------ */

static void about_draw_project(Canvas* canvas) {
    int32_t y = about_draw_header(canvas, FURI_ESP32_PROJECT_NAME);

    canvas_set_font(canvas, FontSecondary);
    const uint8_t pitch = about_body_pitch(canvas);
    y += pitch * about_draw_centered(canvas, y, pitch, FURI_ESP32_PROJECT_AUTHOR);
    y += pitch * about_draw_centered(canvas, y, pitch, "FW: " FURI_ESP32_VERSION);
    y += 2; /* set the link apart from the lines above */
    about_draw_centered(canvas, y, pitch, FURI_ESP32_PROJECT_URL);
}

static void about_draw_hardware(Canvas* canvas, const AboutInfo* info) {
    int32_t y = about_draw_header(canvas, "Hardware");

    canvas_set_font(canvas, FontSecondary);
    const uint8_t pitch = about_body_pitch(canvas);
    y += pitch * about_draw_field(canvas, y, pitch, "Board", furi_hal_board_get_name());
    y += pitch * about_draw_field(canvas, y, pitch, "MCU", info->mcu);
    if(info->pcb) y += pitch * about_draw_field(canvas, y, pitch, "PCB", info->pcb);
    about_draw_field(canvas, y, pitch, "MAC", info->mac);
}

static void about_draw_firmware(Canvas* canvas) {
    int32_t y = about_draw_header(canvas, "Firmware");

    canvas_set_font(canvas, FontSecondary);
    const uint8_t pitch = about_body_pitch(canvas);
    y += pitch * about_draw_field(canvas, y, pitch, "Version", FURI_ESP32_VERSION);
    y += pitch * about_draw_field(canvas, y, pitch, "Built", furi_build_time);
    about_draw_field(canvas, y, pitch, "ESP-IDF", ABOUT_IDF_VERSION);
}

static void about_draw_diagnostics(Canvas* canvas, const AboutInfo* info) {
    int32_t y = about_draw_header(canvas, "Diagnostics");

    canvas_set_font(canvas, FontSecondary);
    const uint8_t pitch = about_body_pitch(canvas);

    char uptime[32];
    about_format_duration(uptime, furi_hal_diag_get_uptime_us() / 1000000);
    about_draw_field_line(canvas, y, "Uptime", uptime);
    y += pitch;
    about_draw_field_line(canvas, y, "Reset", info->reset);
    y += pitch;
    about_draw_field_line(canvas, y, "Last crash", info->crash);
    y += pitch;
    if(info->has_crash) about_draw_centered(canvas, y, pitch, "OK: crash log");
}

static void about_draw_callback(Canvas* canvas, void* _model) {
    const AboutModel* model = _model;

    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);

    switch(model->page) {
    case AboutPageHardware:
        about_draw_hardware(canvas, model->info);
        break;
    case AboutPageFirmware:
        about_draw_firmware(canvas);
        break;
    case AboutPageDiagnostics:
        about_draw_diagnostics(canvas, model->info);
        break;
    default:
        about_draw_project(canvas);
        break;
    }

    about_draw_page_dots(canvas, model->page);
}

/* ------------------------------------------------------------------------ */
/* App                                                                      */
/* ------------------------------------------------------------------------ */

static void about_info_load(AboutApp* app) {
    AboutInfo* info = &app->info;

    furi_hal_board_get_mcu_description(info->mcu, sizeof(info->mcu));

    uint8_t mac[6];
    if(furi_hal_board_get_factory_mac(mac)) {
        snprintf(
            info->mac,
            sizeof(info->mac),
            "%02X:%02X:%02X:%02X:%02X:%02X",
            mac[0],
            mac[1],
            mac[2],
            mac[3],
            mac[4],
            mac[5]);
    } else {
        strlcpy(info->mac, "unknown", sizeof(info->mac));
    }

    info->pcb = furi_hal_board_get_hw_version();
    info->reset = furi_hal_diag_get_reset_reason_name(furi_hal_diag_get_reset_reason());

    FuriHalDiagCrash crash;
    info->has_crash = furi_hal_diag_get_last_crash(&crash) && (crash.has_panic || crash.has_other);
    if(info->has_crash) {
        furi_hal_diag_format_summary(&crash, info->crash, sizeof(info->crash));
        app->crash_log = furi_string_alloc();
        furi_hal_diag_format_log(&crash, app->crash_log);
    } else {
        strlcpy(info->crash, "None", sizeof(info->crash));
    }
}

static bool about_input_callback(InputEvent* event, void* context) {
    AboutApp* app = context;

    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return false;

    int8_t step;
    switch(event->key) {
    case InputKeyUp:
    case InputKeyLeft:
        step = -1;
        break;
    case InputKeyDown:
    case InputKeyRight:
        step = 1;
        break;
    case InputKeyOk:
        step = 0;
        break;
    default:
        return false; /* Back: leave the app */
    }

    bool open_log = false;
    with_view_model(
        app->view,
        AboutModel * model,
        {
            if(step == 0) {
                if(model->page == AboutPageDiagnostics && app->info.has_crash) {
                    open_log = event->type == InputTypeShort;
                } else {
                    model->page = (model->page + 1) % AboutPageCount; /* wraps round */
                }
            } else if(step < 0 && model->page > 0) {
                model->page--;
            } else if(step > 0 && model->page + 1 < AboutPageCount) {
                model->page++;
            }
        },
        !open_log);

    if(open_log) {
        text_box_set_focus(app->text_box, TextBoxFocusStart);
        text_box_set_text(app->text_box, furi_string_get_cstr(app->crash_log));
        view_dispatcher_switch_to_view(app->view_dispatcher, AboutViewCrashLog);
    }

    return true;
}

/* Once a second: redraw for the uptime, only while Diagnostics is shown (a
 * hidden view is not redrawn, see view_dispatcher_update()). */
static void about_tick_callback(void* context) {
    AboutApp* app = context;
    bool diagnostics = false;
    with_view_model(
        app->view,
        AboutModel * model,
        { diagnostics = model->page == AboutPageDiagnostics; },
        false);
    if(diagnostics) {
        with_view_model(app->view, AboutModel * model, { UNUSED(model); }, true);
    }
}

static uint32_t about_app_exit(void* context) {
    UNUSED(context);
    return VIEW_NONE;
}

static uint32_t about_crash_log_back(void* context) {
    UNUSED(context);
    return AboutViewPages;
}

int32_t about_app(void* p) {
    UNUSED(p);

    AboutApp* app = malloc(sizeof(AboutApp));
    memset(app, 0, sizeof(AboutApp));
    about_info_load(app);

    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_tick_event_callback(
        app->view_dispatcher, about_tick_callback, furi_ms_to_ticks(ABOUT_TICK_MS));

    app->view = view_alloc();
    view_set_context(app->view, app);
    view_set_draw_callback(app->view, about_draw_callback);
    view_set_input_callback(app->view, about_input_callback);
    view_set_previous_callback(app->view, about_app_exit);
    /* The model is not zeroed by the allocator: set it explicitly. */
    view_allocate_model(app->view, ViewModelTypeLocking, sizeof(AboutModel));
    with_view_model(
        app->view,
        AboutModel * model,
        {
            model->page = AboutPageProject;
            model->info = &app->info;
        },
        false);
    view_dispatcher_add_view(app->view_dispatcher, AboutViewPages, app->view);

    app->text_box = text_box_alloc();
    text_box_set_font(app->text_box, TextBoxFontText);
    View* text_box_view = text_box_get_view(app->text_box);
    view_set_previous_callback(text_box_view, about_crash_log_back);
    view_dispatcher_add_view(app->view_dispatcher, AboutViewCrashLog, text_box_view);

    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_switch_to_view(app->view_dispatcher, AboutViewPages);
    view_dispatcher_run(app->view_dispatcher);

    view_dispatcher_remove_view(app->view_dispatcher, AboutViewCrashLog);
    text_box_free(app->text_box);
    view_dispatcher_remove_view(app->view_dispatcher, AboutViewPages);
    view_free(app->view);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);
    if(app->crash_log) furi_string_free(app->crash_log);
    free(app);

    return 0;
}
