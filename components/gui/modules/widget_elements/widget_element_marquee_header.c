#include "widget_element_i.h"

/* One header line: a title on the left and a fixed text right-aligned at
 * right_x (e.g. SSID + "-60 dBm"). The right text is always fully drawn; a
 * title that does not fit in the space left over scrolls instead of running
 * into it. The scroll position is derived from the system tick at draw time,
 * so the owner only has to request a redraw periodically. */

#define MARQUEE_STEP_MS  250U /* one character per step */
#define MARQUEE_PAUSE    4U /* steps the title rests at its start each cycle */
#define MARQUEE_GAP      4 /* px between title and right text */
#define MARQUEE_SEPARATOR "   " /* blank between the end and the restart */
#define MARQUEE_BUF_SIZE 96U

typedef struct {
    uint8_t x;
    uint8_t y;
    uint8_t right_x;
    Font title_font;
    Font right_font;
    FuriString* title;
    FuriString* right_text;
    uint32_t start_tick;
} GuiMarqueeHeaderModel;

static inline bool marquee_is_continuation(char c) {
    return ((uint8_t)c & 0xC0U) == 0x80U;
}

/* Number of UTF-8 characters in str. */
static size_t marquee_char_count(const char* str) {
    size_t count = 0;
    for(; *str; str++) {
        if(!marquee_is_continuation(*str)) count++;
    }
    return count;
}

/* Byte length of the UTF-8 character that starts at str. */
static size_t marquee_char_len(const char* str) {
    size_t len = 1;
    while(marquee_is_continuation(str[len])) len++;
    return len;
}

/* Fill buf with as many characters as fit into width, walking the cyclic
 * string "title + separator" from character index `offset`. */
static void marquee_build_window(
    Canvas* canvas,
    const char* title,
    size_t title_chars,
    size_t offset,
    uint16_t width,
    char* buf) {
    const char* separator = MARQUEE_SEPARATOR;
    const size_t separator_chars = marquee_char_count(separator);
    const size_t period = title_chars + separator_chars;

    size_t used = 0;
    buf[0] = '\0';

    /* Cap at one full cycle so a degenerate width cannot loop forever. */
    for(size_t i = 0; i < period; i++) {
        size_t idx = (offset + i) % period;
        const char* src;
        if(idx < title_chars) {
            src = title;
        } else {
            src = separator;
            idx -= title_chars;
        }
        /* Seek to the idx-th character of src. */
        while(idx--) src += marquee_char_len(src);

        const size_t len = marquee_char_len(src);
        if(used + len + 1 > MARQUEE_BUF_SIZE) break;
        memcpy(buf + used, src, len);
        buf[used + len] = '\0';
        if(canvas_string_width(canvas, buf) > width) {
            buf[used] = '\0';
            break;
        }
        used += len;
    }
}

static void gui_marquee_header_draw(Canvas* canvas, WidgetElement* element) {
    furi_assert(canvas);
    furi_assert(element);
    GuiMarqueeHeaderModel* model = element->model;

    uint16_t right_width = 0;
    if(furi_string_size(model->right_text)) {
        canvas_set_font(canvas, model->right_font);
        right_width = canvas_string_width(canvas, furi_string_get_cstr(model->right_text));
        canvas_draw_str_aligned(
            canvas,
            model->right_x,
            model->y,
            AlignRight,
            AlignBottom,
            furi_string_get_cstr(model->right_text));
    }

    if(!furi_string_size(model->title)) return;

    const int32_t reserved = right_width ? right_width + MARQUEE_GAP : 0;
    const int32_t avail = (int32_t)model->right_x - model->x - reserved;
    if(avail <= 0) return;

    canvas_set_font(canvas, model->title_font);
    const char* title = furi_string_get_cstr(model->title);

    if(canvas_string_width(canvas, title) <= avail) {
        canvas_draw_str(canvas, model->x, model->y, title);
        return;
    }

    const size_t title_chars = marquee_char_count(title);
    const size_t period = title_chars + marquee_char_count(MARQUEE_SEPARATOR);
    const uint32_t elapsed_ms =
        (uint32_t)((uint64_t)(furi_get_tick() - model->start_tick) * 1000U /
                   furi_kernel_get_tick_frequency());
    const size_t step = (elapsed_ms / MARQUEE_STEP_MS) % (MARQUEE_PAUSE + period);
    const size_t offset = step < MARQUEE_PAUSE ? 0 : step - MARQUEE_PAUSE;

    char window[MARQUEE_BUF_SIZE];
    marquee_build_window(canvas, title, title_chars, offset, avail, window);
    canvas_draw_str(canvas, model->x, model->y, window);
}

static void gui_marquee_header_free(WidgetElement* element) {
    furi_assert(element);

    GuiMarqueeHeaderModel* model = element->model;
    furi_string_free(model->title);
    furi_string_free(model->right_text);
    free(element->model);
    free(element);
}

WidgetElement* widget_element_marquee_header_create(
    uint8_t x,
    uint8_t y,
    uint8_t right_x,
    Font title_font,
    const char* title,
    Font right_font,
    const char* right_text) {
    furi_assert(title);
    furi_assert(right_text);

    GuiMarqueeHeaderModel* model = malloc(sizeof(GuiMarqueeHeaderModel));
    model->x = x;
    model->y = y;
    model->right_x = right_x;
    model->title_font = title_font;
    model->right_font = right_font;
    model->title = furi_string_alloc_set(title);
    model->right_text = furi_string_alloc_set(right_text);
    model->start_tick = furi_get_tick();

    WidgetElement* element = malloc(sizeof(WidgetElement));
    element->parent = NULL;
    element->input = NULL;
    element->draw = gui_marquee_header_draw;
    element->free = gui_marquee_header_free;
    element->model = model;

    return element;
}
