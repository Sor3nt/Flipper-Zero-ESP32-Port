#include "badmouse.h"
#include "../helper.h"
#include "../libnrf24/nrf24.h"
#include "notification/notification_messages.h"

NRF24L01_Config bm_nrf24_config = {
    .channel = LOGITECH_MIN_CHANNEL,
    .data_rate = DATA_RATE_2MBPS,
    .tx_power = TX_POWER_0DBM,
    .crc_length = CRC_2_BYTES,
    .mac_len = ADDR_WIDTH_5_BYTES,
    .arc = 15,
    .ard = 500,
    .auto_ack = {true, false, false, false, false, false},
    .dynamic_payload = {true, false, false, false, false, false},
    .ack_payload = false,
    .tx_no_ack = false,
    .tx_addr = NULL,
    .rx_addr = {NULL, NULL, NULL, NULL, NULL, NULL},
    .payload_size = {MAX_PAYLOAD_SIZE, 0, 0, 0, 0, 0}};

Setting badmouse_defaults[] = {
    {.name = "ADDR Index", .type = SETTING_TYPE_UINT8, .value.u8 = 0, .min = 0, .max = 0, .step = 1},
    {.name = "KB. Layout", .type = SETTING_TYPE_UINT8, .value.u8 = 0, .min = 0, .max = 0, .step = 1},
    {.name = "Data Rate",
     .type = SETTING_TYPE_DATA_RATE,
     .value.d_r = DATA_RATE_2MBPS,
     .min = DATA_RATE_1MBPS,
     .max = DATA_RATE_250KBPS,
     .step = 1},
    {.name = "Tx Power",
     .type = SETTING_TYPE_TX_POWER,
     .value.t_p = TX_POWER_0DBM,
     .min = TX_POWER_M18DBM,
     .max = TX_POWER_0DBM,
     .step = 1},
    {.name = "Tx Retry",
     .type = SETTING_TYPE_UINT8,
     .value.u8 = DEFAULT_TX_RETRY,
     .min = 0,
     .max = 250,
     .step = 1},
    {.name = "Key Press Delay",
     .type = SETTING_TYPE_UINT8,
     .value.u8 = 5,
     .min = 0,
     .max = 250,
     .step = 1}};

static bool open_payload_file(Nrf24Tool* app) {
    app->storage = furi_record_open(RECORD_STORAGE);
    app->bm_instance->payload_stream = file_stream_alloc(app->storage);

    if(file_stream_open(
           app->bm_instance->payload_stream,
           furi_string_get_cstr(app->bm_instance->file_path),
           FSAM_READ,
           FSOM_OPEN_EXISTING)) {
        app->bm_instance->state.line_nb = 0;
        stream_seek(app->bm_instance->payload_stream, 0, StreamOffsetFromStart);
        // count payload lines
        uint8_t buffer[512];
        size_t bytes_read;
        while((bytes_read =
                   stream_read(app->bm_instance->payload_stream, buffer, sizeof(buffer))) > 0) {
            for(size_t i = 0; i < bytes_read; i++) {
                if(buffer[i] == '\n') {
                    app->bm_instance->state.line_nb++;
                }
            }
        }

        stream_seek(app->bm_instance->payload_stream, 0, StreamOffsetFromStart);
        with_view_model(
            app->badmouse_run, BadMouseStatus * status, status->state = app->bm_instance->state;
            , true);
        return true;
    }
    file_stream_close(app->bm_instance->payload_stream);
    furi_record_close(RECORD_STORAGE);
    return false;
}

static void update_view(Nrf24Tool* app) {
    with_view_model(
        app->badmouse_run, BadMouseStatus * status, status->state = app->bm_instance->state;
        status->execState = app->bm_instance->execState;
        , true)
}

int32_t nrf24_badmouse(void* ctx) {
    Nrf24Tool* app = (Nrf24Tool*)ctx;
    Setting* settings = app->settings->badmouse_settings;

    // open script file
    app->bm_instance = badmouse_script_open(bm_payload_path);
    app->bm_instance->nrf24_config = &bm_nrf24_config;
    app->bm_instance->app_instance = app;
    app->bm_instance->state.line_nb = 0;
    app->bm_instance->state.line_cur = 0;
    app->bm_instance->defdelay = SCRIPT_DEFAULT_DELAY;
    app->bm_instance->stringdelay = 0;
    app->bm_instance->defstringdelay = 0;
    app->bm_instance->repeat_cnt = 0;
    app->bm_instance->key_hold_nb = 0;

    // actualize view
    update_view(app);

    // Set NRF24 settings
    bm_nrf24_config.data_rate = settings[BADMOUSE_SETTING_DATA_RATE].value.d_r;
    bm_nrf24_config.tx_power = settings[BADMOUSE_SETTING_TX_POWER].value.t_p;
    static uint8_t tx_addr[MAX_MAC_SIZE];
    unhexlify(addr_list[settings[BADMOUSE_SETTING_ADDR_INDEX].value.u8], MAX_MAC_SIZE, tx_addr);
    bm_nrf24_config.tx_addr = tx_addr;
    bm_nrf24_config.rx_addr[0] = tx_addr;
    nrf24_configure(&bm_nrf24_config);

    // test if a channel is founded
    if(!find_channel(&bm_nrf24_config)) {
        app->tool_running = false;
        app->bm_instance->execState = BadMouseState_Idle;
        update_view(app);
        // display error message
        view_dispatcher_switch_to_view(app->view_dispatcher, VIEW_BM_NO_CHANNEL);
        return 1;
    } else {
        with_view_model(app->badmouse_run,
                        BadMouseStatus * view_status,
                        view_status->channel = bm_nrf24_config.channel;
                        , true);
    }

    // set keyboard layout
    FuriString* layout_path = furi_string_alloc_set_str(FOLDER_PATH_LAYOUT);
    furi_string_cat_printf(
        layout_path,
        "/%s%s",
        layout_list[app->settings->badmouse_settings[BADMOUSE_SETTING_KB_LAYOUT].value.u8],
        LAYOUT_EXTENSION);
    badmouse_script_set_keyboard_layout(app->bm_instance, layout_path);
    furi_string_free(layout_path);

    // open payload file
    if(!open_payload_file(app)) {
        app->tool_running = false;
        ducky_error(app->bm_instance, "Error opening payload file");
        update_view(app);
        badmouse_script_close(app->bm_instance);
        return 1;
    }

    // MAIN RUN LOOP
    BadMouse* bm = app->bm_instance;
    FuriString* payload_line = furi_string_alloc();
    int32_t return_val = 0;
    uint32_t delay = bm->defdelay;

    while(app->tool_running) {
        switch(bm->execState) {
        case BadMouseState_Init:
            if(bm->state.line_nb == 0) {
                ducky_error(bm, "Empty payload file");
            } else {
                bm->execState = BadMouseState_Run;
                // send "HELLO" packet
                bm_start_transmission(bm->app_instance);
            }
            break;

        case BadMouseState_Run:
            if(delay > 0) {
                delay--;
                break;
            }
            if(bm->repeat_cnt == 0) {
                // get next line
                if(stream_eof(bm->payload_stream)) { // end of file
                    bm->execState = BadMouseState_Done;
                    notification_message(app->notification, &sequence_single_vibro);
                    break;
                } else {
                    stream_read_line(bm->payload_stream, payload_line);
                    furi_string_trim(payload_line);
                    bm->state.line_cur++;
                    return_val = ducky_script_execute_line(bm, payload_line);
                    if(return_val == SCRIPT_STATE_EMPTY_LINE) {
                        break;
                    }
                    furi_string_set(bm->line_prev, bm->line);
                    furi_string_set(bm->line, payload_line);
                    strncpy(bm->state.line_cur_str, furi_string_get_cstr(bm->line), 64);
                    bm->state.line_cur_str[sizeof(bm->state.line_cur_str) - 1] = '\0';
                    update_view(app);
                    if(return_val == SCRIPT_STATE_STRING_START) {
                        bm->execState = BadMouseState_StringPrint;
                        update_view(app);
                        break;
                    } else if(return_val == SCRIPT_STATE_ALTSTRING_START) {
                        bm->execState = BadMouseState_AltStringPrint;
                        update_view(app);
                        break;
                    } else if(return_val == SCRIPT_STATE_WAIT_FOR_BTN) {
                        bm->execState = BadMouseState_WaitForBtn;
                        update_view(app);
                        break;
                    } else if(return_val == SCRIPT_STATE_ERROR) {
                        bm->execState = BadMouseState_Error;
                        break;
                    }
                    // delay before next command
                    delay = bm->defdelay + ((return_val > 0) ? return_val : 0);
                }
            } else {
                bm->repeat_cnt--;
                if(!furi_string_empty(bm->line_prev)) {
                    return_val = ducky_script_execute_line(bm, bm->line_prev);
                }
                if(return_val == SCRIPT_STATE_STRING_START) {
                    bm->execState = BadMouseState_StringPrint;
                    break;
                } else if(return_val == SCRIPT_STATE_ALTSTRING_START) {
                    bm->execState = BadMouseState_AltStringPrint;
                    break;
                } else if(return_val == SCRIPT_STATE_WAIT_FOR_BTN) {
                    bm->execState = BadMouseState_WaitForBtn;
                    break;
                }
                delay = bm->defdelay;
            }
            break;

        case BadMouseState_StringPrint:
        case BadMouseState_AltStringPrint:
            if(delay > 0) {
                delay--;
                break;
            }
            bool ret_string_next;
            if(bm->execState == BadMouseState_StringPrint) {
                ret_string_next = ducky_string_next(bm);
            } else {
                ret_string_next = ducky_altstring_next(bm);
            }
            if(!ret_string_next) {
                delay = bm->defstringdelay + bm->stringdelay;
            } else {
                bm->stringdelay = 0;
                bm->execState = BadMouseState_Run;
                delay = bm->defdelay;
            }
            break;

        case BadMouseState_WaitForBtn:
            delay = KEEP_ALIVE_DELAY;
            break;

        case BadMouseState_Idle:
        case BadMouseState_Done:
        case BadMouseState_Error:
            app->tool_running = false;
            break;
        }
        if(delay >= KEEP_ALIVE_DELAY) {
            bm_send_keep_alive(app);
            delay -= KEEP_ALIVE_DELAY;
        }
    }
    nrf24_set_mode(NRF24_MODE_POWER_DOWN);

    bm_release_all(app);
    bm_end_transmission(app);
    update_view(app);

    furi_string_free(payload_line);

    file_stream_close(app->bm_instance->payload_stream);
    stream_free(app->bm_instance->payload_stream);
    badmouse_script_close(app->bm_instance);

    return 0;
}
