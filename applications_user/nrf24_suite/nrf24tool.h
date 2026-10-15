#pragma once

#include <furi.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/dialog_ex.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/byte_input.h>
#include <gui/modules/text_input.h>
#include <storage/storage.h>
#include <notification/notification.h>
#include <stream/stream.h>
#include <furi/core/thread.h>
#include <dialogs/dialogs.h>

#include "libnrf24/nrf24.h"

#include "settings.h"

#define LOG_TAG  "NRF24TOOL"
#define TOOL_QTY 4

typedef enum {
    MODE_MENU,
    MODE_RX_SETTINGS,
    MODE_RX_RUN,
    MODE_TX_SETTINGS,
    MODE_TX_RUN,
    MODE_SNIFF_SETTINGS,
    MODE_SNIFF_RUN,
    MODE_BADMOUSE_SETTINGS,
    MODE_BADMOUSE_RUN
} Mode;

typedef enum {
    MENU_RX = 1,
    MENU_TX,
    MENU_SNIFFER,
    MENU_BADMOUSE,
    /* extra pentest tools provided by radio/radio_suite.c */
    MENU_RADIO_SPECTRUM,
    MENU_RADIO_CHANSCAN,
    MENU_RADIO_WIFI,
    MENU_RADIO_BLE,
    MENU_RADIO_JAMMER,
    MENU_RADIO_DIAG,
    MENU_RADIO_MJSCAN,
    MENU_RADIO_REPLAY,
    MENU_RADIO_FUZZER,
    MENU_RADIO_RSSI,
} MenuIndex;

typedef enum {
    VIEW_MENU = 1,
    VIEW_NRF24_NOT_CONNECTED,
    VIEW_RX_SETTINGS,
    VIEW_RX_RUN,
    VIEW_TX_SETTINGS,
    VIEW_TX_RUN,
    VIEW_SNIFF_SETTINGS,
    VIEW_SNIFF_RUN,
    VIEW_BM_SETTINGS,
    VIEW_BM_RUN,
    VIEW_BM_NO_CHANNEL,
    VIEW_BM_BROWSE_FILE,
    VIEW_RX_ADDR_INPUT,
    VIEW_TX_ADDR_INPUT,
    VIEW_TX_PAYLOAD_TYPE_SELECT,
    VIEW_TX_PAYLOAD_ASCII_INPUT,
    VIEW_TX_PAYLOAD_HEX_INPUT,
    VIEW_TX_BROWSE_FILE,
    /* extra pentest tool screens (radio/radio_suite.c) */
    VIEW_RADIO_SPECTRUM,
    VIEW_RADIO_CHANSCAN,
    VIEW_RADIO_WIFI,
    VIEW_RADIO_BLE,
    VIEW_RADIO_JAMMER,
    VIEW_RADIO_DIAG,
    VIEW_RADIO_MJSCAN,
    VIEW_RADIO_REPLAY,
    VIEW_RADIO_FUZZER,
    VIEW_RADIO_RSSI
} AppView;

struct BadMouse;
struct RadioSuite;

/* Application context structure */
typedef struct Nrf24Tool {
    ViewDispatcher* view_dispatcher;
    Submenu* app_menu;
    Submenu* scan_menu;
    Submenu* test_menu;
    View* help_view;
    VariableItemList* rx_settings;
    VariableItemList* tx_settings;
    VariableItemList* sniff_settings;
    VariableItemList* badmouse_settings;
    View* rx_run;
    View* tx_run;
    View* sniff_run;
    View* badmouse_run;
    DialogEx* nrf24_disconnect;
    DialogEx* bm_no_channel;
    DialogEx* tx_payload_type_select;
    FileBrowser* bm_script_browser;
    FileBrowser* tx_payload_browser;
    ByteInput* rx_addr_input;
    ByteInput* tx_addr_input;
    ByteInput* tx_payload_hex_input;
    TextInput* tx_payload_text_input;
    uint32_t previous_view;
    uint32_t next_view;
    struct BadMouse* bm_instance;
    struct RadioSuite* radio; /* extra pentest tools (radio/radio_suite.c) */
    FuriMessageQueue* event_queue;
    Storage* storage;
    Stream* stream;
    NotificationApp* notification;
    FuriThread* tool_thread;
    Settings* settings;
    bool app_running;
    _Atomic bool tool_running;
} Nrf24Tool;

bool nrf24_test_connection(Nrf24Tool* app, uint32_t from_view, uint32_t to_view);

void app_menu_enter_callback(void* context, uint32_t index);
uint32_t app_menu_exit_callback(void* context);
void no_nrf24_result_callback(DialogExResult result, void* context);
void rx_thread_state_callback(FuriThread* thread, FuriThreadState state, void* context);
void tx_thread_state_callback(FuriThread* thread, FuriThreadState state, void* context);
void sniff_thread_state_callback(FuriThread* thread, FuriThreadState state, void* context);
void bm_thread_state_callback(FuriThread* thread, FuriThreadState state, void* context);
