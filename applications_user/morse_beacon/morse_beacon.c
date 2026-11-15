/*
 * Morse Beacon - sub-GHz CW / MCW identifier for the Flipper Zero.
 *
 * Two jobs:
 *   1. Station ID - watch a channel and key a Morse identifier after the first
 *      transmission that follows a configurable stretch of dead air.
 *   2. Send arbitrary text as Morse on demand.
 */

#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/text_input.h>
#include <gui/modules/number_input.h>
#include <gui/elements.h>
#include <notification/notification_messages.h>
#include <furi_hal_board_info.h> // PORT: board support gate
#include <furi_hal_region.h> // PORT: region name for the About screen
#include <dialogs/dialogs.h> // PORT: "board not supported" dialog

#include "morse.h"
#include "radio.h"
#include "beacon.h"
#include "config.h"
#include "link.h"
#include "idlog.h"

#define TAG            "MorseBeacon"
#define MORSE_TICK_MS  100
#define FREQ_STEP_UNIT 100 // number input works in 100 Hz steps

typedef enum {
    MorseViewSubmenu,
    MorseViewBeacon,
    MorseViewSettings,
    MorseViewTextInput,
    MorseViewNumberInput,
    MorseViewSending,
    MorseViewLink,
    MorseViewAbout,
} MorseViewId;

typedef enum {
    SubmenuIndexBeacon,
    SubmenuIndexSendText,
    SubmenuIndexLink,
    SubmenuIndexSettings,
    SubmenuIndexAbout,
} SubmenuIndex;

/* Logical settings rows - identities, not positions. Rows appear and disappear
 * with the mode, the trigger and the remote source, so the list is built through
 * settings_add(), which records the mapping in both directions. The enum this
 * replaces was positional and had already drifted out of step with the list. */
typedef enum {
    SettingIdText,
    SettingFrequency,
    SettingPreset,
    SettingMode,
    SettingDeviation,
    SettingRxBw,
    SettingTone,
    SettingWpm,
    SettingFarnsworth,
    SettingSquelch,
    SettingTrigger,
    SettingPeriod,
    SettingAnchor,
    SettingQuiet,
    SettingInterval,
    SettingCourtesy,
    SettingPreamble,
    SettingTail,
    SettingRadio,
    SettingLink,
    SettingBaud,
    SettingIdOnStart,
    SettingRowCount,
} SettingRow;

typedef enum {
    MorseEventTxDone = 100,
    MorseEventRadioError,
    MorseEventSettingsDirty, // a change altered which rows are relevant
} MorseEvent;

typedef enum {
    TextTargetId,
    TextTargetFree,
} TextTarget;

typedef struct {
    BeaconStatus status;
    uint32_t frequency;
    uint32_t quiet_arm_ms;
    uint8_t mode;
    uint8_t trigger;
    bool radio_ok;
} BeaconViewModel;

typedef struct {
    char text[MORSE_MAX_TEXT + 1];
    uint8_t progress;
    bool failed;
} SendingViewModel;

typedef struct {
    LinkState state;
    uint8_t source;
    uint32_t baudrate;
    uint32_t rx_count;
    char last[MORSE_MAX_TEXT + 1];
    uint8_t progress;
    bool sending;
} LinkViewModel;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    NotificationApp* notifications;

    Submenu* submenu;
    VariableItemList* var_list;
    /* Presets the running firmware will actually transmit on - indices into
     * freq_presets, rebuilt with the settings list. Region-blocked presets
     * are not shown at all. Sized by the static assert next to the table. */
    uint8_t preset_map[24];
    uint8_t preset_count;
    TextInput* text_input;
    NumberInput* number_input;
    View* about_view;
    View* beacon_view;
    View* sending_view;
    View* link_view;

    CwRadio* radio;
    Beacon* beacon;
    MorseLink* link;
    MorseConfig config;
    volatile bool link_sending;

    char text_buffer[MORSE_MAX_TEXT + 1];
    TextTarget text_target;
    MorseViewId current_view;

    // settings list position <-> logical row, rebuilt with the list
    uint8_t row_of_index[SettingRowCount];
    uint8_t index_of_row[SettingRowCount];
    uint8_t row_count;

    FuriThread* tx_thread;
    volatile bool tx_running;
    char tx_text[MORSE_MAX_TEXT + 1];
} MorseApp;

/* ------------------------------------------------------------ value tables */

static const uint32_t freq_presets[] = {
    // GMRS 1-7: interstitial simplex channels
    462562500,
    462587500,
    462612500,
    462637500,
    462662500,
    462687500,
    462712500,
    // GMRS 15-22: main channels, and the repeater outputs RP15-RP22.
    // Channels 8-14 are omitted: they sit at 467 MHz, outside the band the
    // CC1101 is rated for. The 467 MHz repeater inputs are out for the same
    // reason.
    462550000,
    462575000,
    462600000,
    462625000,
    462650000,
    462675000,
    462700000,
    462725000,
    // Non-GMRS landmarks, low to high
    315000000, // US ISM, common Part 15 remote frequency - bench testing
    432300000, // 70cm propagation beacon subband (432.300-432.400, ARRL plan)
    433920000, // ISM / 70cm, the band the antenna is matched for
    446000000, // 70cm national FM simplex CALLING - a landmark, not a beacon spot
    915000000, // US ISM centre - bench testing
};
static const char* const freq_preset_names[] = {
    "GMRS 1",
    "GMRS 2",
    "GMRS 3",
    "GMRS 4",
    "GMRS 5",
    "GMRS 6",
    "GMRS 7",
    "GMRS 15",
    "GMRS 16",
    "GMRS 17",
    "GMRS 18",
    "GMRS 19",
    "GMRS 20",
    "GMRS 21",
    "GMRS 22",
    "315.000",
    "432.300",
    "433.920",
    "446.000",
    "915.000",
};
#define FREQ_PRESET_COUNT COUNT_OF(freq_presets)
_Static_assert(COUNT_OF(freq_presets) <= 24, "preset_map in MorseApp is too small");

static const char* const mode_names[] = {"MCW (FM)", "CW (OOK)", "SSB (USB)", "SSB (LSB)"};
static const char* const deviation_names[] = {"2.4 kHz", "4.8 kHz"};
static const char* const rx_bw_names[] = {"58 kHz", "101 kHz", "135 kHz", "270 kHz"};
static const char* const onoff_names[] = {"OFF", "ON"};
static const char* const radio_names[] = {"Internal", "External"};
static const char* const link_names[] = {"BLE", "UART 13/14"};
static const uint32_t baud_values[] = {9600, 19200, 38400, 57600, 115200};
static const char* const baud_names[] = {"9600", "19200", "38400", "57600", "115200"};

static const uint16_t quiet_values[] = {30, 60, 120, 180, 300, 600, 900, 1200, 1800};
static const char* const quiet_names[] =
    {"30 s", "1 min", "2 min", "3 min", "5 min", "10 min", "15 min", "20 min", "30 min"};

static const char* const trigger_names[] = {"On activity", "Interval", "Both"};

static const uint16_t period_values[] = {15, 30, 45, 60, 90, 120, 180, 300, 600, 900, 1800};
static const char* const period_names[] = {"15 s", "30 s",  "45 s",  "60 s",  "90 s", "2 min",
                                           "3 min", "5 min", "10 min", "15 min", "30 min"};

static const uint16_t interval_values[] = {0, 300, 600, 900, 1200, 1800, 3600};
static const char* const interval_names[] =
    {"OFF", "5 min", "10 min", "15 min", "20 min", "30 min", "60 min"};

/* ------------------------------------------------------------------ helpers */

static void morse_format_freq(char* out, size_t size, uint32_t hz) {
    snprintf(out, size, "%lu.%04lu", hz / 1000000UL, (hz % 1000000UL) / 100UL);
}

static void morse_format_span(char* out, size_t size, uint32_t ms) {
    uint32_t total = ms / 1000;
    snprintf(out, size, "%lu:%02lu", total / 60, total % 60);
}

static void morse_radio_params(const MorseConfig* config, CwRadioParams* params) {
    params->frequency = config->frequency;
    params->mode = config->mode;
    params->deviation = config->deviation;
    params->rx_bw = config->rx_bw;
    params->tone_hz = config->tone_hz;
}

/* ---------------------------------------------------------------- about view */

/* "\e#" at the start of a line selects the bold font for that line.
 * The stock text_scroll widget breaks lines on pixel width regardless of word
 * boundaries, so this view measures and wraps the text itself. */
static FuriString* about_string = NULL;
static const char* about_text = "";

/* Probe the running firmware for the spans it will actually transmit on,
 * rather than quoting figures that are only true for one firmware build. */
static void about_add_tx_ranges(FuriString* out) {
    const uint32_t step = 25000;
    uint32_t start = 0;
    bool open = false;
    bool any = false;

    for(uint32_t f = 280000000; f <= 970000000; f += step) {
        bool allowed = cw_radio_tx_allowed(f);
        if(allowed && !open) {
            start = f;
            open = true;
        } else if(!allowed && open) {
            uint32_t last = f - step;
            furi_string_cat_printf(
                out,
                "%lu.%03lu-%lu.%03lu MHz\n",
                start / 1000000,
                (start % 1000000) / 1000,
                last / 1000000,
                (last % 1000000) / 1000);
            open = false;
            any = true;
        }
    }
    if(open) {
        furi_string_cat_printf(
            out, "%lu.%03lu MHz and up\n", start / 1000000, (start % 1000000) / 1000);
        any = true;
    }
    if(!any) furi_string_cat_str(out, "none - this firmware blocks transmitting\n");
}

static void about_build(void) {
    about_string = furi_string_alloc();
    FuriString* s = about_string;

    furi_string_cat_str(
        s,
        "\e#Morse Beacon\n"
        "Sub-GHz CW / MCW station ID.\n\n"
        "\e#Beacon ID\n"
        "\ebOn activity:\eb keys after the first transmission that follows the "
        "quiet time.\n"
        "\ebInterval:\eb keys every Interval, busy channel or not - use this for a "
        "fox or a plain beacon.\n"
        "\ebBoth:\eb whichever comes first.\n"
        "\ebOK\eb keys an ID immediately.\n\n"
        "\e#Interval timing\n"
        "\ebMeasure from\eb decides what the period is measured between. "
        "\ebStart of TX\eb starts transmissions a fixed period apart, so the "
        "cadence is the period exactly - what a fox needs. \ebEnd of TX\eb "
        "makes the period the quiet gap between them, so the cycle grows by "
        "however long the ID takes.\n"
        "If the ID is longer than the interval, whole slots are dropped rather "
        "than keying without a break, and the log says so.\n\n"
        "\e#Modes\n"
        "MCW puts an audible tone on an FM carrier - this is what an FM radio "
        "hears. CW (OOK) keys the bare carrier: true CW, silent on an FM "
        "receiver but a tone on anything with a BFO. The SSB modes key the "
        "carrier offset from the dial by the Tone setting, so an SSB receiver "
        "tuned to the dial frequency in the matching sideband hears the tone "
        "at that pitch.\n\n"
        "\e#Remote text\n"
        "Pair a phone over BLE, or wire a device to pins 13/14, and every line "
        "you send is keyed as Morse. A newline ends a line, and so does a "
        "short pause.\n\n"
        "\e#Range\n"
        "What may be transmitted is decided by your firmware and its region, "
        "not by this app - it differs between firmware builds. Measured from "
        "the firmware running right now:\n");

    furi_string_cat_printf(s, "\ebRegion:\eb %s\n", furi_hal_region_get_name());
    about_add_tx_ranges(s);

    furi_string_cat_str(
        s,
        "The CC1101 chip itself is rated 300-348, 387-464 and 779-928 MHz, and "
        "the antenna is matched for 433, so output outside those is "
        "unspecified. About 10 mW.\n\n"
        "\e#Licensing\n"
        "Nothing here checks whether you are allowed to transmit. Key only "
        "where you are licensed to.\n");

    about_text = furi_string_get_cstr(s);

    // One line of provenance: what this firmware allowed, on this run.
    morse_log("about: region=%s", furi_hal_region_get_name());
}

static void about_teardown(void) {
    if(about_string) {
        furi_string_free(about_string);
        about_string = NULL;
        about_text = "";
    }
}

#define ABOUT_TEXT_WIDTH 118
#define ABOUT_LINE_H     10
#define ABOUT_VISIBLE    6
#define ABOUT_MAX_LINES  128

typedef struct {
    uint16_t start;
    uint16_t len;
    bool bold;
} AboutLine;

typedef struct {
    AboutLine line[ABOUT_MAX_LINES];
    uint16_t count; // 0 until the text has been wrapped
    uint16_t scroll;
} AboutViewModel;

static void about_wrap(Canvas* canvas, AboutViewModel* model) {
    const char* t = about_text;
    size_t pos = 0;
    model->count = 0;

    while(t[pos] != '\0' && model->count < ABOUT_MAX_LINES) {
        bool bold = false;
        if(t[pos] == '\e' && t[pos + 1] == '#') {
            bold = true;
            pos += 2;
        }
        canvas_set_font(canvas, bold ? FontPrimary : FontSecondary);

        size_t src_end = pos;
        while(t[src_end] != '\0' && t[src_end] != '\n') src_end++;

        if(src_end == pos) { // blank line, kept for spacing
            model->line[model->count].start = pos;
            model->line[model->count].len = 0;
            model->line[model->count].bold = bold;
            model->count++;
        }

        size_t cur = pos;
        while(cur < src_end && model->count < ABOUT_MAX_LINES) {
            uint16_t width = 0;
            size_t i = cur;
            size_t last_space = 0;
            bool have_space = false;

            while(i < src_end) {
                // "\eb" toggles inline bold; the marker itself takes no space
                if(t[i] == '\e' && (i + 1) < src_end && t[i + 1] == 'b') {
                    i += 2;
                    continue;
                }
                uint16_t w = canvas_glyph_width(canvas, t[i]);
                if(width + w > ABOUT_TEXT_WIDTH) break;
                width += w;
                if(t[i] == ' ') {
                    last_space = i;
                    have_space = true;
                }
                i++;
            }

            size_t line_end;
            if(i >= src_end) {
                line_end = src_end; // the rest fits
            } else if(have_space && last_space > cur) {
                line_end = last_space; // break at the last space that fitted
            } else {
                line_end = (i > cur) ? i : cur + 1; // a single over-long word
            }

            model->line[model->count].start = cur;
            model->line[model->count].len = (uint16_t)(line_end - cur);
            model->line[model->count].bold = bold;
            model->count++;

            cur = line_end;
            while(cur < src_end && t[cur] == ' ') cur++; // swallow the break space
        }

        pos = src_end;
        if(t[pos] == '\n') pos++;
    }
}

/* Draws one wrapped line. "\eb" emphasis markers are stripped and NOT
 * rendered: FontSecondary has one-pixel inter-glyph gaps, so a double-strike
 * "bold" fills them and merges the letters into a smear. The emphasised terms
 * all end in a colon, which carries the structure on its own. */
static void about_draw_line(Canvas* canvas, uint8_t y, const char* s, uint16_t len) {
    char plain[64];
    size_t n = 0;

    for(uint16_t i = 0; i < len && n < sizeof(plain) - 1; i++) {
        if(s[i] == '\e' && (i + 1) < len && s[i + 1] == 'b') {
            i++; // step over the 'b'
            continue;
        }
        plain[n++] = s[i];
    }
    plain[n] = '\0';
    canvas_draw_str(canvas, 2, y, plain);
}

static void about_view_draw(Canvas* canvas, void* model) {
    AboutViewModel* m = model;

    canvas_clear(canvas);
    if(m->count == 0) about_wrap(canvas, m);

    uint8_t y = ABOUT_LINE_H - 1;
    for(uint16_t i = m->scroll; i < m->count && y < 64 + ABOUT_LINE_H; i++) {
        canvas_set_font(canvas, m->line[i].bold ? FontPrimary : FontSecondary);
        about_draw_line(canvas, y, about_text + m->line[i].start, m->line[i].len);
        y += ABOUT_LINE_H;
    }

    if(m->count > ABOUT_VISIBLE) {
        elements_scrollbar(canvas, m->scroll, m->count - ABOUT_VISIBLE + 1);
    }
}

static bool about_view_input(InputEvent* event, void* context) {
    MorseApp* app = context;
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return false;

    if(event->key == InputKeyDown) {
        with_view_model(
            app->about_view,
            AboutViewModel * model,
            {
                if(model->count > ABOUT_VISIBLE &&
                   model->scroll < model->count - ABOUT_VISIBLE) {
                    model->scroll++;
                }
            },
            true);
        return true;
    }
    if(event->key == InputKeyUp) {
        with_view_model(
            app->about_view,
            AboutViewModel * model,
            {
                if(model->scroll > 0) model->scroll--;
            },
            true);
        return true;
    }
    return false;
}

/* -------------------------------------------------------------- beacon view */

static void beacon_view_draw(Canvas* canvas, void* model) {
    BeaconViewModel* m = model;
    char buf[32];

    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    morse_format_freq(buf, sizeof(buf), m->frequency);
    canvas_draw_str(canvas, 2, 10, buf);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 62, 10, mode_names[m->mode]);

    if(!m->radio_ok || m->status.state == BeaconStateError) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 34, AlignCenter, AlignCenter, "Radio unavailable");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(
            canvas, 64, 48, AlignCenter, AlignCenter, "Check frequency / region");
        return;
    }

    // signal bar: -110 dBm at the left edge, -40 at the right
    int32_t rssi = (int32_t)m->status.rssi;
    int32_t width = (rssi + 110) * 124 / 70;
    if(width < 0) width = 0;
    if(width > 124) width = 124;
    canvas_draw_frame(canvas, 2, 14, 124, 7);
    if(width > 1) canvas_draw_box(canvas, 3, 15, width > 122 ? 122 : width, 5);

    snprintf(buf, sizeof(buf), "%ld dBm", (long)rssi);
    canvas_draw_str(canvas, 2, 31, buf);

    const char* state = "LISTENING";
    switch(m->status.state) {
    case BeaconStateBusy:
        state = "TRAFFIC";
        break;
    case BeaconStatePending:
        state = "ID PENDING";
        break;
    case BeaconStateSending:
        state = "SENDING ID";
        break;
    case BeaconStateStopped:
        state = "STOPPED";
        break;
    default:
        break;
    }
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 126, 31, AlignRight, AlignBottom, state);
    canvas_set_font(canvas, FontSecondary);

    if(m->status.state == BeaconStateSending) {
        canvas_draw_frame(canvas, 2, 36, 124, 8);
        uint8_t fill = m->status.progress * 122 / 100;
        if(fill) canvas_draw_box(canvas, 3, 37, fill, 6);
    } else {
        if(m->trigger == BeaconTriggerActivity) {
            morse_format_span(buf, sizeof(buf), m->status.quiet_ms);
            canvas_draw_str(canvas, 2, 42, "Quiet");
            canvas_draw_str(canvas, 34, 42, buf);
        } else {
            morse_format_span(buf, sizeof(buf), m->status.next_id_ms);
            canvas_draw_str(canvas, 2, 42, "Next");
            canvas_draw_str(canvas, 34, 42, buf);
        }

        morse_format_span(buf, sizeof(buf), m->status.since_id_ms);
        canvas_draw_str(canvas, 68, 42, "Last ID");
        canvas_draw_str(canvas, 106, 42, buf);
    }

    snprintf(
        buf,
        sizeof(buf),
        "%s  IDs %lu  RX %lu",
        (m->trigger == BeaconTriggerInterval) ? "TIMED" :
                                                (m->status.armed ? "ARMED" : "waiting"),
        (unsigned long)m->status.id_count,
        (unsigned long)m->status.traffic_count);
    canvas_draw_str(canvas, 2, 53, buf);

    elements_button_center(canvas, "ID now");
}

static bool beacon_view_input(InputEvent* event, void* context) {
    MorseApp* app = context;
    if(event->type == InputTypeShort && event->key == InputKeyOk) {
        beacon_request_id(app->beacon);
        notification_message(app->notifications, &sequence_blink_blue_10);
        return true;
    }
    return false;
}

/* ------------------------------------------------------------ sending view */

static void sending_view_draw(Canvas* canvas, void* model) {
    SendingViewModel* m = model;

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(
        canvas, 64, 12, AlignCenter, AlignCenter, m->failed ? "Transmit failed" : "Transmitting");

    canvas_set_font(canvas, FontSecondary);
    elements_text_box(
        canvas, 2, 20, 124, 20, AlignCenter, AlignCenter, m->text, false);

    canvas_draw_frame(canvas, 2, 44, 124, 9);
    uint8_t fill = m->progress * 122 / 100;
    if(fill) canvas_draw_box(canvas, 3, 45, fill, 7);

    canvas_draw_str_aligned(canvas, 64, 61, AlignCenter, AlignBottom, "Back to stop");
}

static bool sending_view_input(InputEvent* event, void* context) {
    MorseApp* app = context;
    if(event->type == InputTypeShort && event->key == InputKeyBack) {
        cw_radio_abort(app->radio);
        return true;
    }
    return false;
}

/* ---------------------------------------------------------------- link view */

static void link_view_draw(Canvas* canvas, void* model) {
    LinkViewModel* m = model;
    char buf[40];

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Remote text");

    canvas_set_font(canvas, FontSecondary);
    if(m->source == LinkSourceUart) {
        snprintf(buf, sizeof(buf), "UART %lu 8N1", (unsigned long)m->baudrate);
    } else {
        snprintf(buf, sizeof(buf), "BLE serial");
    }
    canvas_draw_str_aligned(canvas, 126, 10, AlignRight, AlignBottom, buf);
    canvas_draw_line(canvas, 0, 13, 128, 13);

    const char* state = "stopped";
    switch(m->state) {
    case LinkStateAdvertising:
        state = (m->source == LinkSourceUart) ? "waiting for data" : "pair with the Flipper";
        break;
    case LinkStateConnected:
        state = (m->source == LinkSourceUart) ? "listening on 13/14" : "connected";
        break;
    case LinkStateError:
        state = "link unavailable";
        break;
    default:
        break;
    }
    canvas_draw_str(canvas, 2, 24, state);

    if(m->sending) {
        canvas_draw_str(canvas, 2, 36, "Keying...");
        canvas_draw_frame(canvas, 2, 40, 124, 8);
        uint8_t fill = m->progress * 122 / 100;
        if(fill) canvas_draw_box(canvas, 3, 41, fill, 6);
    } else {
        snprintf(buf, sizeof(buf), "Lines sent: %lu", (unsigned long)m->rx_count);
        canvas_draw_str(canvas, 2, 36, buf);
        if(m->last[0]) {
            elements_text_box(
                canvas, 2, 39, 124, 12, AlignLeft, AlignTop, m->last, false);
        }
    }

    canvas_draw_str_aligned(
        canvas, 64, 63, AlignCenter, AlignBottom, "Send a line of text to key it");
}

static bool link_view_input(InputEvent* event, void* context) {
    MorseApp* app = context;
    if(event->type == InputTypeShort && event->key == InputKeyOk && app->link_sending) {
        cw_radio_abort(app->radio);
        return true;
    }
    return false;
}

/* -------------------------------------------------------------- radio owner */

static bool morse_radio_acquire(MorseApp* app) {
    if(cw_radio_is_open(app->radio)) return true;
    // PORT: this firmware has no external CC1101. "cc1101_ext" is only an alias that drives the
    // internal chip and always reports connected; its begin()/end() re-run furi_hal_subghz_init()
    // and furi_hal_subghz_shutdown() on the internal radio. Always use the internal device (a
    // settings file saved with "External" by an older build is overridden here too).
    app->config.external_radio = false;
    if(cw_radio_open(app->radio, app->config.external_radio)) return true;
    morse_log("radio: acquire failed (external=%d)", app->config.external_radio ? 1 : 0);
    if(app->config.external_radio) {
        // fall back to the built-in CC1101 so the app still works
        return cw_radio_open(app->radio, false);
    }
    return false;
}

static void morse_radio_release(MorseApp* app) {
    cw_radio_close(app->radio);
}

/* -------------------------------------------------------------- text sender */

static int32_t morse_tx_thread(void* context) {
    MorseApp* app = context;

    MorseStream* stream = morse_stream_alloc(
        app->tx_text,
        app->config.wpm,
        app->config.farnsworth,
        (app->config.mode == CwModeMcw) ? app->config.preamble_ms : 0,
        (app->config.mode == CwModeMcw) ? app->config.tail_ms : 0);

    bool ok = false;
    if(stream) {
        CwRadioParams params;
        morse_radio_params(&app->config, &params);
        ok = cw_radio_send(app->radio, stream, &params);
        morse_stream_free(stream);
    }

    with_view_model(
        app->sending_view, SendingViewModel * model, { model->failed = !ok; }, true);

    app->tx_running = false;
    view_dispatcher_send_custom_event(app->view_dispatcher, MorseEventTxDone);
    return 0;
}

static void morse_start_text_tx(MorseApp* app, const char* text) {
    strncpy(app->tx_text, text, MORSE_MAX_TEXT);
    app->tx_text[MORSE_MAX_TEXT] = '\0';

    with_view_model(
        app->sending_view,
        SendingViewModel * model,
        {
            strncpy(model->text, app->tx_text, MORSE_MAX_TEXT);
            model->text[MORSE_MAX_TEXT] = '\0';
            model->progress = 0;
            model->failed = false;
        },
        true);

    if(!morse_radio_acquire(app)) {
        with_view_model(
            app->sending_view, SendingViewModel * model, { model->failed = true; }, true);
        app->current_view = MorseViewSending;
        view_dispatcher_switch_to_view(app->view_dispatcher, MorseViewSending);
        return;
    }

    app->tx_running = true;
    app->tx_thread = furi_thread_alloc_ex("MorseTx", 4096, morse_tx_thread, app);
    furi_thread_start(app->tx_thread);

    app->current_view = MorseViewSending;
    view_dispatcher_switch_to_view(app->view_dispatcher, MorseViewSending);
}

static void morse_finish_text_tx(MorseApp* app) {
    if(app->tx_thread) {
        furi_thread_join(app->tx_thread);
        furi_thread_free(app->tx_thread);
        app->tx_thread = NULL;
    }
    morse_radio_release(app);
}

/* ------------------------------------------------------------- link sending */

static void morse_link_line_received(const char* line, void* context) {
    MorseApp* app = context;

    MorseStream* stream = morse_stream_alloc(
        line,
        app->config.wpm,
        app->config.farnsworth,
        (app->config.mode == CwModeMcw) ? app->config.preamble_ms : 0,
        (app->config.mode == CwModeMcw) ? app->config.tail_ms : 0);

    if(!stream) {
        morse_link_reply(app->link, "err: nothing sendable\r\n");
        return;
    }

    CwRadioParams params;
    morse_radio_params(&app->config, &params);

    app->link_sending = true;
    bool ok = cw_radio_send(app->radio, stream, &params);
    app->link_sending = false;
    morse_stream_free(stream);

    morse_link_reply(app->link, ok ? "ok\r\n" : "err: transmit refused\r\n");
}

static void morse_link_enter(MorseApp* app) {
    bool radio_ok = morse_radio_acquire(app);
    bool link_ok = radio_ok && morse_link_start(
                                   app->link,
                                   (LinkSource)app->config.link_source,
                                   app->config.link_baudrate);

    with_view_model(
        app->link_view,
        LinkViewModel * model,
        {
            model->source = app->config.link_source;
            model->baudrate = app->config.link_baudrate;
            model->rx_count = 0;
            model->last[0] = '\0';
            model->progress = 0;
            model->sending = false;
            model->state = link_ok ? morse_link_state(app->link) : LinkStateError;
        },
        true);

    if(link_ok) {
        morse_link_reply(app->link, "Morse Beacon ready\r\n");
    }

    app->current_view = MorseViewLink;
    view_dispatcher_switch_to_view(app->view_dispatcher, MorseViewLink);
}

static void morse_link_leave(MorseApp* app) {
    cw_radio_abort(app->radio);
    morse_link_stop(app->link);
    morse_radio_release(app);
}

/* ----------------------------------------------------------------- settings */

static void number_input_callback(void* context, int32_t number);


static void setting_mode_changed(VariableItem* item) {
    MorseApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    app->config.mode = index;
    variable_item_set_current_value_text(item, mode_names[index]);
    /* Rebuilding here would free the array this very item lives in.
     * Post it instead: the dispatcher runs it after this returns. */
    view_dispatcher_send_custom_event(app->view_dispatcher, MorseEventSettingsDirty);
}

static void setting_deviation_changed(VariableItem* item) {
    MorseApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    app->config.deviation = index;
    variable_item_set_current_value_text(item, deviation_names[index]);
}

static void setting_preset_changed(VariableItem* item) {
    MorseApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    if(index >= app->preset_count) return;
    uint8_t p = app->preset_map[index];
    app->config.frequency = freq_presets[p];
    variable_item_set_current_value_text(item, (char*)freq_preset_names[p]);
    /* The Frequency row needs its text refreshed too. variable_item_list_get()
     * is an Unleashed extension, so rebuild the list instead - same deferred
     * pattern as the mode change, and the rebuild keeps the selected row. */
    view_dispatcher_send_custom_event(app->view_dispatcher, MorseEventSettingsDirty);
}

static void setting_rx_bw_changed(VariableItem* item) {
    MorseApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    app->config.rx_bw = index;
    variable_item_set_current_value_text(item, rx_bw_names[index]);
}

static void setting_tone_changed(VariableItem* item) {
    MorseApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    app->config.tone_hz = 400 + index * 50;
    char buf[16];
    snprintf(buf, sizeof(buf), "%u Hz", app->config.tone_hz);
    variable_item_set_current_value_text(item, buf);
}

static void setting_wpm_changed(VariableItem* item) {
    MorseApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    app->config.wpm = 5 + index;
    char buf[16];
    snprintf(buf, sizeof(buf), "%u wpm", app->config.wpm);
    variable_item_set_current_value_text(item, buf);
}

static void setting_farnsworth_changed(VariableItem* item) {
    MorseApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    app->config.farnsworth = index ? (4 + index) : 0;
    char buf[16];
    if(index == 0) {
        snprintf(buf, sizeof(buf), "OFF");
    } else {
        snprintf(buf, sizeof(buf), "%u wpm", app->config.farnsworth);
    }
    variable_item_set_current_value_text(item, buf);
}

static void setting_squelch_changed(VariableItem* item) {
    MorseApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    app->config.squelch_dbm = -110 + index * 5;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d dBm", app->config.squelch_dbm);
    variable_item_set_current_value_text(item, buf);
}

static void setting_trigger_changed(VariableItem* item) {
    MorseApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    app->config.beacon_trigger = index;
    variable_item_set_current_value_text(item, trigger_names[index]);
    /* Rebuilding here would free the array this very item lives in.
     * Post it instead: the dispatcher runs it after this returns. */
    view_dispatcher_send_custom_event(app->view_dispatcher, MorseEventSettingsDirty);
}

static void setting_period_changed(VariableItem* item) {
    MorseApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    app->config.period_s = period_values[index];
    variable_item_set_current_value_text(item, (char*)period_names[index]);
}

static void setting_quiet_changed(VariableItem* item) {
    MorseApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    app->config.quiet_time_s = quiet_values[index];
    variable_item_set_current_value_text(item, (char*)quiet_names[index]);
}

static void setting_interval_changed(VariableItem* item) {
    MorseApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    app->config.max_interval_s = interval_values[index];
    variable_item_set_current_value_text(item, (char*)interval_names[index]);
}

static void setting_courtesy_changed(VariableItem* item) {
    MorseApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    app->config.courtesy_delay_ms = index * 500;
    char buf[16];
    snprintf(
        buf,
        sizeof(buf),
        "%u.%u s",
        app->config.courtesy_delay_ms / 1000,
        (app->config.courtesy_delay_ms % 1000) / 100);
    variable_item_set_current_value_text(item, buf);
}

static void setting_preamble_changed(VariableItem* item) {
    MorseApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    app->config.preamble_ms = index * 100;
    char buf[16];
    snprintf(buf, sizeof(buf), "%u ms", app->config.preamble_ms);
    variable_item_set_current_value_text(item, buf);
}

static void setting_tail_changed(VariableItem* item) {
    MorseApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    app->config.tail_ms = index * 100;
    char buf[16];
    snprintf(buf, sizeof(buf), "%u ms", app->config.tail_ms);
    variable_item_set_current_value_text(item, buf);
}

static void setting_radio_changed(VariableItem* item) {
    MorseApp* app = variable_item_get_context(item);
    // PORT: internal radio only (see morse_radio_acquire); the row offers a single value.
    app->config.external_radio = false;
    variable_item_set_current_value_text(item, radio_names[0]);
}

static void setting_link_changed(VariableItem* item) {
    MorseApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    app->config.link_source = index;
    variable_item_set_current_value_text(item, link_names[index]);
    /* Rebuilding here would free the array this very item lives in.
     * Post it instead: the dispatcher runs it after this returns. */
    view_dispatcher_send_custom_event(app->view_dispatcher, MorseEventSettingsDirty);
}

static void setting_baud_changed(VariableItem* item) {
    MorseApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    app->config.link_baudrate = baud_values[index];
    variable_item_set_current_value_text(item, (char*)baud_names[index]);
}

static void setting_id_on_start_changed(VariableItem* item) {
    MorseApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    app->config.id_on_start = index == 1;
    variable_item_set_current_value_text(item, onoff_names[index]);
}


static const char* const anchor_names[] = {"End of TX", "Start of TX"};

static void setting_anchor_changed(VariableItem* item) {
    MorseApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    app->config.interval_anchor = index;
    variable_item_set_current_value_text(item, anchor_names[index]);
}

static void settings_enter_callback(void* context, uint32_t index) {
    MorseApp* app = context;

    // The list is index-addressed; everything else here thinks in logical rows.
    if(index >= app->row_count) return;
    SettingRow row = (SettingRow)app->row_of_index[index];

    if(row == SettingIdText) {
        app->text_target = TextTargetId;
        strncpy(app->text_buffer, app->config.id_text, MORSE_MAX_TEXT);
        app->text_buffer[MORSE_MAX_TEXT] = '\0';
        text_input_set_header_text(app->text_input, "Station ID / callsign");
        app->current_view = MorseViewTextInput;
        view_dispatcher_switch_to_view(app->view_dispatcher, MorseViewTextInput);
    } else if(row == SettingFrequency) {
        number_input_set_header_text(app->number_input, "Frequency, 100 Hz steps");
        number_input_set_result_callback(
            app->number_input,
            number_input_callback,
            app,
            app->config.frequency / FREQ_STEP_UNIT,
            2810000, // full tuning range the firmware allows, in 100 Hz steps
            9620000);
        app->current_view = MorseViewNumberInput;
        view_dispatcher_switch_to_view(app->view_dispatcher, MorseViewNumberInput);
    }
}

static uint8_t index_of_u16(const uint16_t* values, uint8_t count, uint16_t value) {
    for(uint8_t i = 0; i < count; i++) {
        if(values[i] == value) return i;
    }
    return 0;
}

/* Adds a row and records where it landed, so both the enter callback and the
 * post-rebuild cursor can work in logical rows rather than positions. */
static VariableItem* settings_add(
    MorseApp* app,
    SettingRow row,
    const char* label,
    uint8_t values_count,
    VariableItemChangeCallback change_callback) {
    app->index_of_row[row] = app->row_count;
    app->row_of_index[app->row_count] = row;
    app->row_count++;
    return variable_item_list_add(app->var_list, label, values_count, change_callback, app);
}

static void settings_rebuild(MorseApp* app) {
    VariableItemList* list = app->var_list;

    /* Remember the logical row under the cursor rather than its index - a row
     * above it may be about to disappear. */
    uint8_t prev_index = variable_item_list_get_selected_item_index(list);
    SettingRow prev_row = (app->row_count && prev_index < app->row_count) ?
                              (SettingRow)app->row_of_index[prev_index] :
                              SettingIdText;

    variable_item_list_reset(list);
    app->row_count = 0;
    memset(app->index_of_row, 0xFF, sizeof(app->index_of_row));
    memset(app->row_of_index, 0, sizeof(app->row_of_index));

    /* A row is shown when changing it would change what the beacon does.
     * Squelch and RX filter stay visible even in pure interval mode: nothing
     * keys off them there, but they still drive the carrier indicator on the
     * beacon screen, which is worth being able to set. */
    const bool mcw = app->config.mode == CwModeMcw;
    const bool use_interval = app->config.beacon_trigger == BeaconTriggerInterval ||
                              app->config.beacon_trigger == BeaconTriggerBoth;
    const bool use_activity = app->config.beacon_trigger == BeaconTriggerActivity ||
                              app->config.beacon_trigger == BeaconTriggerBoth;
    const bool uart = app->config.link_source == LinkSourceUart;

    char buf[24];
    VariableItem* item;

    item = settings_add(app, SettingIdText, "Station ID", 1, NULL);
    variable_item_set_current_value_text(item, app->config.id_text);

    item = settings_add(app, SettingFrequency, "Frequency", 1, NULL);
    morse_format_freq(buf, sizeof(buf), app->config.frequency);
    variable_item_set_current_value_text(item, buf);

    /* Offer only presets the running firmware will transmit on: what is
     * permitted differs by firmware build and provisioned region, and a
     * preset that can only ever be refused is noise. */
    app->preset_count = 0;
    uint8_t preset_index = 0;
    bool preset_match = false;
    for(uint8_t i = 0; i < FREQ_PRESET_COUNT; i++) {
        if(!cw_radio_frequency_supported(freq_presets[i]) ||
           !cw_radio_tx_allowed(freq_presets[i])) {
            continue;
        }
        if(freq_presets[i] == app->config.frequency) {
            preset_index = app->preset_count;
            preset_match = true;
        }
        app->preset_map[app->preset_count++] = i;
    }
    if(app->preset_count > 0) {
        item = settings_add(
            app, SettingPreset, "Preset", app->preset_count, setting_preset_changed);
        variable_item_set_current_value_index(item, preset_index);
        variable_item_set_current_value_text(
            item,
            preset_match ? (char*)freq_preset_names[app->preset_map[preset_index]] : "Custom");
    }

    item = settings_add(app, SettingMode, "Mode", CwModeCount, setting_mode_changed);
    variable_item_set_current_value_index(item, app->config.mode);
    variable_item_set_current_value_text(item, mode_names[app->config.mode]);

    /* CW keys the bare carrier. There is no tone to pitch, no deviation to
     * set, and no carrier for an unkeyed preamble or tail to hold up. The SSB
     * modes also key a bare carrier, but Tone stays visible for them: it sets
     * the carrier's offset from the dial, which IS the received pitch. */
    if(mcw) {
        item = settings_add(
            app, SettingDeviation, "Deviation", CwDeviationCount, setting_deviation_changed);
        variable_item_set_current_value_index(item, app->config.deviation);
        variable_item_set_current_value_text(item, deviation_names[app->config.deviation]);
    }

    item = settings_add(app, SettingRxBw, "RX filter", CwRxBwCount, setting_rx_bw_changed);
    variable_item_set_current_value_index(item, app->config.rx_bw);
    variable_item_set_current_value_text(item, rx_bw_names[app->config.rx_bw]);

    if(app->config.mode != CwModeOok) {
        item = settings_add(app, SettingTone, "Tone", 17, setting_tone_changed);
        variable_item_set_current_value_index(item, (app->config.tone_hz - 400) / 50);
        snprintf(buf, sizeof(buf), "%u Hz", app->config.tone_hz);
        variable_item_set_current_value_text(item, buf);
    }

    item = settings_add(app, SettingWpm, "Speed", 36, setting_wpm_changed);
    variable_item_set_current_value_index(item, app->config.wpm - 5);
    snprintf(buf, sizeof(buf), "%u wpm", app->config.wpm);
    variable_item_set_current_value_text(item, buf);

    item = settings_add(app, SettingFarnsworth, "Farnsworth", 17, setting_farnsworth_changed);
    variable_item_set_current_value_index(
        item, app->config.farnsworth ? app->config.farnsworth - 4 : 0);
    if(app->config.farnsworth) {
        snprintf(buf, sizeof(buf), "%u wpm", app->config.farnsworth);
    } else {
        snprintf(buf, sizeof(buf), "OFF");
    }
    variable_item_set_current_value_text(item, buf);

    item = settings_add(app, SettingSquelch, "Squelch", 13, setting_squelch_changed);
    variable_item_set_current_value_index(item, (app->config.squelch_dbm + 110) / 5);
    snprintf(buf, sizeof(buf), "%d dBm", app->config.squelch_dbm);
    variable_item_set_current_value_text(item, buf);

    item =
        settings_add(app, SettingTrigger, "Trigger", BeaconTriggerCount, setting_trigger_changed);
    variable_item_set_current_value_index(item, app->config.beacon_trigger);
    variable_item_set_current_value_text(item, trigger_names[app->config.beacon_trigger]);

    if(use_interval) {
        uint8_t pi = index_of_u16(period_values, COUNT_OF(period_values), app->config.period_s);
        item = settings_add(
            app, SettingPeriod, "Interval", COUNT_OF(period_values), setting_period_changed);
        variable_item_set_current_value_index(item, pi);
        variable_item_set_current_value_text(item, (char*)period_names[pi]);

        item = settings_add(
            app, SettingAnchor, "Measure from", IntervalAnchorCount, setting_anchor_changed);
        variable_item_set_current_value_index(item, app->config.interval_anchor);
        variable_item_set_current_value_text(item, anchor_names[app->config.interval_anchor]);
    }

    /* The activity state machine - how much dead air arms the ID, the backstop
     * gap, and the pause before keying - only runs when traffic can trigger. */
    if(use_activity) {
        item = settings_add(
            app, SettingQuiet, "Quiet time", COUNT_OF(quiet_values), setting_quiet_changed);
        uint8_t qi = index_of_u16(quiet_values, COUNT_OF(quiet_values), app->config.quiet_time_s);
        variable_item_set_current_value_index(item, qi);
        variable_item_set_current_value_text(item, (char*)quiet_names[qi]);

        item = settings_add(
            app,
            SettingInterval,
            "Max ID gap",
            COUNT_OF(interval_values),
            setting_interval_changed);
        uint8_t ii =
            index_of_u16(interval_values, COUNT_OF(interval_values), app->config.max_interval_s);
        variable_item_set_current_value_index(item, ii);
        variable_item_set_current_value_text(item, (char*)interval_names[ii]);

        item = settings_add(app, SettingCourtesy, "Courtesy", 11, setting_courtesy_changed);
        variable_item_set_current_value_index(item, app->config.courtesy_delay_ms / 500);
        snprintf(
            buf,
            sizeof(buf),
            "%u.%u s",
            app->config.courtesy_delay_ms / 1000,
            (app->config.courtesy_delay_ms % 1000) / 100);
        variable_item_set_current_value_text(item, buf);
    }

    if(mcw) {
        item = settings_add(app, SettingPreamble, "Preamble", 11, setting_preamble_changed);
        variable_item_set_current_value_index(item, app->config.preamble_ms / 100);
        snprintf(buf, sizeof(buf), "%u ms", app->config.preamble_ms);
        variable_item_set_current_value_text(item, buf);

        item = settings_add(app, SettingTail, "Tail", 11, setting_tail_changed);
        variable_item_set_current_value_index(item, app->config.tail_ms / 100);
        snprintf(buf, sizeof(buf), "%u ms", app->config.tail_ms);
        variable_item_set_current_value_text(item, buf);
    }

    // PORT: internal radio only (no external CC1101 on this firmware, see morse_radio_acquire).
    app->config.external_radio = false;
    item = settings_add(app, SettingRadio, "Radio", 1, setting_radio_changed);
    variable_item_set_current_value_index(item, 0);
    variable_item_set_current_value_text(item, radio_names[0]);

    // PORT: BLE-only on this firmware (no UART HAL). Offer a single source so "UART 13/14" can't be
    // selected; link_source is also clamped to BLE in config.c.
    app->config.link_source = LinkSourceBle;
    item = settings_add(app, SettingLink, "Remote", 1, setting_link_changed);
    variable_item_set_current_value_index(item, LinkSourceBle);
    variable_item_set_current_value_text(item, link_names[LinkSourceBle]);

    // Baud rate belongs to the GPIO UART; BLE has no such knob.
    if(uart) {
        uint8_t bi = 0;
        for(uint8_t i = 0; i < COUNT_OF(baud_values); i++) {
            if(baud_values[i] == app->config.link_baudrate) bi = i;
        }
        item = settings_add(
            app, SettingBaud, "UART baud", COUNT_OF(baud_values), setting_baud_changed);
        variable_item_set_current_value_index(item, bi);
        variable_item_set_current_value_text(item, (char*)baud_names[bi]);
    }

    item = settings_add(app, SettingIdOnStart, "ID on start", 2, setting_id_on_start_changed);
    variable_item_set_current_value_index(item, app->config.id_on_start ? 1 : 0);
    variable_item_set_current_value_text(item, onoff_names[app->config.id_on_start ? 1 : 0]);

    /* Stay on the row the cursor was on. If it just vanished, fall back to the
     * row that hid it, which is always the one being edited. */
    uint8_t target = app->index_of_row[prev_row];
    if(target == 0xFF) target = app->index_of_row[SettingTrigger];
    if(target == 0xFF || target >= app->row_count) target = 0;
    variable_item_list_set_selected_item(list, target);
}

/* -------------------------------------------------------------- navigation */

static void text_input_callback(void* context) {
    MorseApp* app = context;

    if(app->text_target == TextTargetId) {
        strncpy(app->config.id_text, app->text_buffer, MORSE_MAX_TEXT);
        app->config.id_text[MORSE_MAX_TEXT] = '\0';
        morse_config_save(&app->config);
        settings_rebuild(app);
        app->current_view = MorseViewSettings;
        view_dispatcher_switch_to_view(app->view_dispatcher, MorseViewSettings);
    } else {
        morse_start_text_tx(app, app->text_buffer);
    }
}

static void number_input_callback(void* context, int32_t number) {
    MorseApp* app = context;
    app->config.frequency = (uint32_t)number * FREQ_STEP_UNIT;
    morse_config_save(&app->config);
    settings_rebuild(app);
    app->current_view = MorseViewSettings;
    view_dispatcher_switch_to_view(app->view_dispatcher, MorseViewSettings);
}

static void submenu_callback(void* context, uint32_t index) {
    MorseApp* app = context;

    switch(index) {
    case SubmenuIndexBeacon: {
        morse_log("ui: entering beacon, %lu Hz", app->config.frequency);
        bool ok = morse_radio_acquire(app);
        with_view_model(
            app->beacon_view,
            BeaconViewModel * model,
            {
                model->radio_ok = ok;
                model->frequency = app->config.frequency;
                model->mode = app->config.mode;
                model->trigger = app->config.beacon_trigger;
                model->quiet_arm_ms = (uint32_t)app->config.quiet_time_s * 1000;
                memset(&model->status, 0, sizeof(BeaconStatus));
                model->status.state = ok ? BeaconStateListening : BeaconStateError;
            },
            true);
        if(ok) beacon_start(app->beacon, &app->config);
        app->current_view = MorseViewBeacon;
        view_dispatcher_switch_to_view(app->view_dispatcher, MorseViewBeacon);
        break;
    }
    case SubmenuIndexSendText:
        app->text_target = TextTargetFree;
        app->text_buffer[0] = '\0';
        text_input_set_header_text(app->text_input, "Text to send");
        app->current_view = MorseViewTextInput;
        view_dispatcher_switch_to_view(app->view_dispatcher, MorseViewTextInput);
        break;
    case SubmenuIndexLink:
        morse_link_enter(app);
        break;
    case SubmenuIndexSettings:
        settings_rebuild(app);
        app->current_view = MorseViewSettings;
        view_dispatcher_switch_to_view(app->view_dispatcher, MorseViewSettings);
        break;
    case SubmenuIndexAbout:
        with_view_model(
            app->about_view, AboutViewModel * model, { model->scroll = 0; }, false);
        app->current_view = MorseViewAbout;
        view_dispatcher_switch_to_view(app->view_dispatcher, MorseViewAbout);
        break;
    default:
        break;
    }
}

static bool morse_navigation_callback(void* context) {
    MorseApp* app = context;

    switch(app->current_view) {
    case MorseViewSubmenu:
        view_dispatcher_stop(app->view_dispatcher);
        return true;
    case MorseViewBeacon:
        beacon_stop(app->beacon);
        morse_radio_release(app);
        break;
    case MorseViewSending:
        if(app->tx_running) {
            cw_radio_abort(app->radio);
            return true; // the thread will post MorseEventTxDone
        }
        morse_finish_text_tx(app);
        break;
    case MorseViewLink:
        morse_link_leave(app);
        break;
    case MorseViewSettings:
        morse_config_save(&app->config);
        break;
    default:
        break;
    }

    app->current_view = MorseViewSubmenu;
    view_dispatcher_switch_to_view(app->view_dispatcher, MorseViewSubmenu);
    return true;
}

static bool morse_custom_event_callback(void* context, uint32_t event) {
    MorseApp* app = context;

    if(event == MorseEventSettingsDirty) {
        if(app->current_view == MorseViewSettings) settings_rebuild(app);
        return true;
    }

    if(event == MorseEventTxDone) {
        morse_finish_text_tx(app);
        app->current_view = MorseViewSubmenu;
        view_dispatcher_switch_to_view(app->view_dispatcher, MorseViewSubmenu);
        return true;
    }
    return false;
}

static void morse_tick_callback(void* context) {
    MorseApp* app = context;

    if(app->current_view == MorseViewBeacon) {
        BeaconStatus status;
        beacon_get_status(app->beacon, &status);
        with_view_model(
            app->beacon_view, BeaconViewModel * model, { model->status = status; }, true);
    } else if(app->current_view == MorseViewLink) {
        LinkState state = morse_link_state(app->link);
        uint32_t count = morse_link_rx_count(app->link);
        bool sending = app->link_sending;
        uint8_t progress = cw_radio_progress(app->radio);
        char last[MORSE_MAX_TEXT + 1];
        morse_link_last_line(app->link, last, sizeof(last));
        with_view_model(
            app->link_view,
            LinkViewModel * model,
            {
                if(model->state != LinkStateError) model->state = state;
                model->rx_count = count;
                model->sending = sending;
                model->progress = progress;
                strncpy(model->last, last, MORSE_MAX_TEXT);
                model->last[MORSE_MAX_TEXT] = '\0';
            },
            true);
    } else if(app->current_view == MorseViewSending && app->tx_running) {
        uint8_t progress = cw_radio_progress(app->radio);
        with_view_model(
            app->sending_view, SendingViewModel * model, { model->progress = progress; }, true);
    }
}

/* --------------------------------------------------------------- app plumbing */

/* PORT: Flipper's malloc returns zeroed memory and upstream relied on that for the view models
 * (text/last buffers, counters, flags). ESP-IDF's malloc does not zero, so a view could draw a
 * garbage, unterminated string before its first update. */
static void morse_zero_model(View* view, size_t size) {
    with_view_model(view, void* model, { memset(model, 0, size); }, false);
}

static MorseApp* morse_app_alloc(void) {
    MorseApp* app = malloc(sizeof(MorseApp));
    memset(app, 0, sizeof(MorseApp));

    morse_config_load(&app->config);

    app->gui = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);

    app->radio = cw_radio_alloc();
    app->beacon = beacon_alloc(app->radio);
    app->link = morse_link_alloc();
    morse_link_set_callback(app->link, morse_link_line_received, app);

    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, morse_navigation_callback);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, morse_custom_event_callback);
    view_dispatcher_set_tick_event_callback(app->view_dispatcher, morse_tick_callback, MORSE_TICK_MS);

    app->submenu = submenu_alloc();
    submenu_add_item(app->submenu, "Beacon ID", SubmenuIndexBeacon, submenu_callback, app);
    submenu_add_item(app->submenu, "Send text", SubmenuIndexSendText, submenu_callback, app);
    submenu_add_item(app->submenu, "Remote text", SubmenuIndexLink, submenu_callback, app);
    submenu_add_item(app->submenu, "Settings", SubmenuIndexSettings, submenu_callback, app);
    submenu_add_item(app->submenu, "About", SubmenuIndexAbout, submenu_callback, app);
    view_dispatcher_add_view(
        app->view_dispatcher, MorseViewSubmenu, submenu_get_view(app->submenu));

    app->var_list = variable_item_list_alloc();
    variable_item_list_set_enter_callback(app->var_list, settings_enter_callback, app);
    view_dispatcher_add_view(
        app->view_dispatcher, MorseViewSettings, variable_item_list_get_view(app->var_list));

    app->text_input = text_input_alloc();
    text_input_set_result_callback(
        app->text_input,
        text_input_callback,
        app,
        app->text_buffer,
        MORSE_MAX_TEXT,
        false);
    view_dispatcher_add_view(
        app->view_dispatcher, MorseViewTextInput, text_input_get_view(app->text_input));

    app->number_input = number_input_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, MorseViewNumberInput, number_input_get_view(app->number_input));

    app->beacon_view = view_alloc();
    view_set_context(app->beacon_view, app);
    view_allocate_model(app->beacon_view, ViewModelTypeLocking, sizeof(BeaconViewModel));
    morse_zero_model(app->beacon_view, sizeof(BeaconViewModel)); // PORT: ESP-IDF malloc does not zero
    view_set_draw_callback(app->beacon_view, beacon_view_draw);
    view_set_input_callback(app->beacon_view, beacon_view_input);
    view_dispatcher_add_view(app->view_dispatcher, MorseViewBeacon, app->beacon_view);

    app->sending_view = view_alloc();
    view_set_context(app->sending_view, app);
    view_allocate_model(app->sending_view, ViewModelTypeLocking, sizeof(SendingViewModel));
    morse_zero_model(app->sending_view, sizeof(SendingViewModel)); // PORT: ESP-IDF malloc does not zero
    view_set_draw_callback(app->sending_view, sending_view_draw);
    view_set_input_callback(app->sending_view, sending_view_input);
    view_dispatcher_add_view(app->view_dispatcher, MorseViewSending, app->sending_view);

    app->link_view = view_alloc();
    view_set_context(app->link_view, app);
    view_allocate_model(app->link_view, ViewModelTypeLocking, sizeof(LinkViewModel));
    morse_zero_model(app->link_view, sizeof(LinkViewModel)); // PORT: ESP-IDF malloc does not zero
    view_set_draw_callback(app->link_view, link_view_draw);
    view_set_input_callback(app->link_view, link_view_input);
    view_dispatcher_add_view(app->view_dispatcher, MorseViewLink, app->link_view);

    about_build();
    app->about_view = view_alloc();
    view_set_context(app->about_view, app);
    view_allocate_model(app->about_view, ViewModelTypeLocking, sizeof(AboutViewModel));
    morse_zero_model(app->about_view, sizeof(AboutViewModel)); // PORT: ESP-IDF malloc does not zero
    view_set_draw_callback(app->about_view, about_view_draw);
    view_set_input_callback(app->about_view, about_view_input);
    view_dispatcher_add_view(app->view_dispatcher, MorseViewAbout, app->about_view);

    app->current_view = MorseViewSubmenu;
    return app;
}

static void morse_app_free(MorseApp* app) {
    beacon_stop(app->beacon);
    morse_link_stop(app->link);
    morse_finish_text_tx(app);

    view_dispatcher_remove_view(app->view_dispatcher, MorseViewSubmenu);
    view_dispatcher_remove_view(app->view_dispatcher, MorseViewSettings);
    view_dispatcher_remove_view(app->view_dispatcher, MorseViewTextInput);
    view_dispatcher_remove_view(app->view_dispatcher, MorseViewNumberInput);
    view_dispatcher_remove_view(app->view_dispatcher, MorseViewBeacon);
    view_dispatcher_remove_view(app->view_dispatcher, MorseViewSending);
    view_dispatcher_remove_view(app->view_dispatcher, MorseViewLink);
    view_dispatcher_remove_view(app->view_dispatcher, MorseViewAbout);

    submenu_free(app->submenu);
    variable_item_list_free(app->var_list);
    text_input_free(app->text_input);
    number_input_free(app->number_input);
    view_free(app->beacon_view);
    view_free(app->sending_view);
    view_free(app->link_view);
    view_free(app->about_view);
    about_teardown();
    view_dispatcher_free(app->view_dispatcher);

    morse_link_free(app->link);
    beacon_free(app->beacon);
    cw_radio_free(app->radio);

    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);
    free(app);
}

// PORT: ported external apps run only on the opted-in ESP32-S3 boards (T-Embed CC1101, Paradoxt,
// ThorChimera, Thormini). Refuse elsewhere with a clear message instead of keying a radio on
// unverified pins/power.
static bool morse_board_supported(void) {
    if(furi_hal_board_supports_ported_apps()) return true;
    DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
    DialogMessage* message = dialog_message_alloc();
    dialog_message_set_header(message, "Board not supported", 64, 2, AlignCenter, AlignTop);
    dialog_message_set_text(
        message,
        "Runs on T-Embed, Paradoxt,\nThorChimera and Thormini\nonly.",
        64,
        34,
        AlignCenter,
        AlignCenter);
    dialog_message_set_buttons(message, NULL, "OK", NULL);
    dialog_message_show(dialogs, message);
    dialog_message_free(message);
    furi_record_close(RECORD_DIALOGS);
    return false;
}

int32_t morse_beacon_app(void* p) {
    UNUSED(p);
    if(!morse_board_supported()) return 0;

    MorseApp* app = morse_app_alloc();
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_switch_to_view(app->view_dispatcher, MorseViewSubmenu);
    view_dispatcher_run(app->view_dispatcher);

    morse_config_save(&app->config);
    morse_app_free(app);
    return 0;
}
