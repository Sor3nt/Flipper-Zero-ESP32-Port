#include "nrf24tool.h"
#include "libnrf24/nrf24.h"
#include "tx/tx.h"
#include "sniff/sniff.h"
#include "badmouse/badmouse.h"
#include "rx/rx.h"
#include "radio/radio_suite.h"

void app_menu_enter_callback(void* context, uint32_t index) {
    Nrf24Tool* app = (Nrf24Tool*)context;

    switch(index) {
    case 1001:
    case 1002:
        view_dispatcher_switch_to_view(app->view_dispatcher, index);
        break;
    case 1000:
        view_dispatcher_switch_to_view(app->view_dispatcher, 1000);
        break;
    case MENU_RX:
        view_dispatcher_switch_to_view(app->view_dispatcher, VIEW_RX_SETTINGS);
        break;

    case MENU_TX:
        view_dispatcher_switch_to_view(app->view_dispatcher, VIEW_TX_SETTINGS);
        break;

    case MENU_SNIFFER:
        view_dispatcher_switch_to_view(app->view_dispatcher, VIEW_SNIFF_SETTINGS);
        break;

    case MENU_BADMOUSE:
        bm_read_address(app);
        bm_read_layouts(app);
        view_dispatcher_switch_to_view(app->view_dispatcher, VIEW_BM_SETTINGS);
        break;

    case MENU_RADIO_SPECTRUM:
        radio_suite_enter(app, RadioToolSpectrum);
        break;

    case MENU_RADIO_CHANSCAN:
        radio_suite_enter(app, RadioToolChannelScan);
        break;

    case MENU_RADIO_WIFI:
        radio_suite_enter(app, RadioToolWifi);
        break;

    case MENU_RADIO_BLE:
        radio_suite_enter(app, RadioToolBle);
        break;

    case MENU_RADIO_JAMMER:
        radio_suite_enter(app, RadioToolJammer);
        break;

    case MENU_RADIO_DIAG:
        radio_suite_enter(app, RadioToolDiag);
        break;

    case MENU_RADIO_MJSCAN:
        radio_suite_enter(app, RadioToolMjScan);
        break;

    case MENU_RADIO_REPLAY:
        radio_suite_enter(app, RadioToolReplay);
        break;

    case MENU_RADIO_FUZZER:
        radio_suite_enter(app, RadioToolFuzzer);
        break;

    case MENU_RADIO_RSSI:
        radio_suite_enter(app, RadioToolRssi);
        break;

    default:
        return;
    }
}

uint32_t app_menu_exit_callback(void* context) {
    UNUSED(context);
    return VIEW_NONE;
}

void no_nrf24_result_callback(DialogExResult result, void* context) {
    Nrf24Tool* app = (Nrf24Tool*)context;
    if(result == DialogExResultLeft) {
        view_dispatcher_switch_to_view(app->view_dispatcher, app->previous_view);
    } else if(result == DialogExResultRight) {
        if(nrf24_check_connected()) {
            view_dispatcher_switch_to_view(app->view_dispatcher, app->next_view);
        }
    }
}

void rx_thread_state_callback(FuriThread* thread, FuriThreadState state, void* context) {
    UNUSED(thread);
    Nrf24Tool* app = (Nrf24Tool*)context;

    with_view_model(app->rx_run, RxStatus* status, status->thread_state = state;, true);
}

void tx_thread_state_callback(FuriThread* thread, FuriThreadState state, void* context) {
    UNUSED(thread);
    Nrf24Tool* app = (Nrf24Tool*)context;

    with_view_model(app->tx_run, TxStatus* status, status->thread_state = state;, true);
}

void sniff_thread_state_callback(FuriThread* thread, FuriThreadState state, void* context) {
    UNUSED(thread);
    Nrf24Tool* app = (Nrf24Tool*)context;

    with_view_model(app->sniff_run, SniffStatus* status, status->thread_state = state;, true);
}

void bm_thread_state_callback(FuriThread* thread, FuriThreadState state, void* context) {
    UNUSED(thread);
    Nrf24Tool* app = (Nrf24Tool*)context;

    with_view_model(app->badmouse_run, BadMouseStatus* status, status->thread_state = state;
                    , true);
}
