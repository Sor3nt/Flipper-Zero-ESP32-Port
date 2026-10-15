#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/number_input.h>
#include <dialogs/dialogs.h>
#include "../subghz/views/subghz_spectrum.h"
#include "../subghz/helpers/subghz_spectrum_math.h"

/* Bruce-beta interaction mapped onto local Furi services, not its Arduino HAL. */
enum { WaterfallMenu, WaterfallFrequency, WaterfallPlot };
enum { WaterfallStart = 1, WaterfallSetStart, WaterfallSetEnd, WaterfallStop = 100 };
typedef struct {
    Gui* gui;
    DialogsApp* dialogs;
    ViewDispatcher* dispatcher;
    Submenu* menu;
    NumberInput* number;
    SubGhzSpectrum* plot;
    uint32_t start, end;
    uint8_t page;
    bool edit_start;
} WaterfallApp;

static void waterfall_show_menu(WaterfallApp* app);

static void waterfall_error(WaterfallApp* app, const char* text) {
    DialogMessage* message = dialog_message_alloc();
    dialog_message_set_header(message, "RF Waterfall", 64, 3, AlignCenter, AlignTop);
    dialog_message_set_text(message, text, 64, 32, AlignCenter, AlignCenter);
    dialog_message_set_buttons(message, "Back", NULL, NULL);
    dialog_message_show(app->dialogs, message);
    dialog_message_free(message);
}

static void waterfall_select(void* context, uint32_t index) {
    WaterfallApp* app = context;
    view_dispatcher_send_custom_event(app->dispatcher, index);
}

static void waterfall_show_menu(WaterfallApp* app) {
    char label[40];
    app->page = WaterfallMenu;
    submenu_reset(app->menu);
    submenu_set_header(app->menu, "RF Waterfall (CC1101)");
    submenu_add_item(app->menu, "Start Waterfall", WaterfallStart, waterfall_select, app);
    snprintf(label, sizeof(label), "Start: %lu.%03lu MHz", (unsigned long)(app->start / 1000000),
        (unsigned long)(app->start % 1000000 / 1000));
    submenu_add_item(app->menu, label, WaterfallSetStart, waterfall_select, app);
    snprintf(label, sizeof(label), "End: %lu.%03lu MHz", (unsigned long)(app->end / 1000000),
        (unsigned long)(app->end % 1000000 / 1000));
    submenu_add_item(app->menu, label, WaterfallSetEnd, waterfall_select, app);
    view_dispatcher_switch_to_view(app->dispatcher, WaterfallMenu);
}

static void waterfall_frequency_done(void* context, int32_t khz) {
    WaterfallApp* app = context;
    uint32_t hz = (uint32_t)khz * 1000;
    if(!subghz_spectrum_frequency_supported(hz)) {
        waterfall_error(app, "Use 300-348, 387-464\nor 779-928 MHz.");
        return;
    }
    if(app->edit_start) app->start = hz;
    else app->end = hz;
    waterfall_show_menu(app);
}

static void waterfall_plot_event(SubGhzCustomEvent event, void* context) {
    UNUSED(event);
    WaterfallApp* app = context;
    view_dispatcher_send_custom_event(app->dispatcher, WaterfallStop);
}

static bool waterfall_event(void* context, uint32_t event) {
    WaterfallApp* app = context;
    if(event == WaterfallStop) {
        subghz_spectrum_stop(app->plot);
        waterfall_show_menu(app);
    } else if(event == WaterfallStart) {
        if(app->start > app->end) {
            uint32_t swap = app->start; app->start = app->end; app->end = swap;
        }
        if(!subghz_spectrum_configure_window(app->plot, app->start, app->end)) {
            waterfall_error(app, "Choose two distinct\nsupported frequencies.");
            return true;
        }
        app->page = WaterfallPlot;
        view_dispatcher_switch_to_view(app->dispatcher, WaterfallPlot);
        if(!subghz_spectrum_start(app->plot)) {
            waterfall_error(app, "Not enough memory.\nClose other tools.");
            waterfall_show_menu(app);
        }
    } else if(event == WaterfallSetStart || event == WaterfallSetEnd) {
        app->edit_start = event == WaterfallSetStart;
        app->page = WaterfallFrequency;
        number_input_set_header_text(app->number, app->edit_start ? "Start frequency (kHz)" : "End frequency (kHz)");
        number_input_set_result_callback(app->number, waterfall_frequency_done, app,
            (app->edit_start ? app->start : app->end) / 1000, 300000, 928000);
        view_dispatcher_switch_to_view(app->dispatcher, WaterfallFrequency);
    } else return false;
    return true;
}

static bool waterfall_back(void* context) {
    WaterfallApp* app = context;
    if(app->page == WaterfallMenu) return false;
    if(app->page == WaterfallPlot) subghz_spectrum_stop(app->plot);
    waterfall_show_menu(app);
    return true;
}

int32_t rf_waterfall_app(void* argument) {
    UNUSED(argument);
    WaterfallApp* app = malloc(sizeof(*app));
    furi_check(app);
    app->start = 433000000; app->end = 435000000;
    app->gui = furi_record_open(RECORD_GUI);
    app->dialogs = furi_record_open(RECORD_DIALOGS);
    app->dispatcher = view_dispatcher_alloc();
    app->menu = submenu_alloc();
    app->number = number_input_alloc();
    app->plot = subghz_spectrum_alloc();
    subghz_spectrum_set_spectrogram(app->plot, true);
    subghz_spectrum_set_callback(app->plot, waterfall_plot_event, app);
    view_dispatcher_set_event_callback_context(app->dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->dispatcher, waterfall_event);
    view_dispatcher_set_navigation_event_callback(app->dispatcher, waterfall_back);
    view_dispatcher_add_view(app->dispatcher, WaterfallMenu, submenu_get_view(app->menu));
    view_dispatcher_add_view(app->dispatcher, WaterfallFrequency, number_input_get_view(app->number));
    view_dispatcher_add_view(app->dispatcher, WaterfallPlot, subghz_spectrum_get_view(app->plot));
    view_dispatcher_attach_to_gui(app->dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    waterfall_show_menu(app);
    view_dispatcher_run(app->dispatcher);
    subghz_spectrum_stop(app->plot);
    view_dispatcher_switch_to_view(app->dispatcher, VIEW_NONE);
    view_dispatcher_remove_view(app->dispatcher, WaterfallPlot);
    view_dispatcher_remove_view(app->dispatcher, WaterfallFrequency);
    view_dispatcher_remove_view(app->dispatcher, WaterfallMenu);
    view_dispatcher_free(app->dispatcher);
    subghz_spectrum_free(app->plot);
    number_input_free(app->number);
    submenu_free(app->menu);
    furi_record_close(RECORD_DIALOGS);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}
