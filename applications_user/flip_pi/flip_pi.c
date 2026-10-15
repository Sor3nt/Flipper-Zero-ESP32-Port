/* SSH terminal display and controls for a Kali Pi companion. MIT license. */
#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_box.h>
#include <gui/modules/text_input.h>
#include <storage/storage.h>
#include <wifi/wifi.h>
#include <btshim.h>
#include <ble_serial.h>
#include <esp_http_client.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tool_templates.h"
#include "terminal_render.h"
#include <furi_hal_display.h>
#define CONFIG_PATH EXT_PATH("apps_data/flip_pi/config.txt")
#define CERT_PATH EXT_PATH("apps_data/flip_pi/pi.crt")
#define DIAG_PATH EXT_PATH("apps_data/flip_pi/last_run.txt")
#define WIRE_SIZE 2048
#define COMMAND_SIZE 512
#define STATUS_SIZE 256
#define TAG "FlipPi"
enum { MenuView, TerminalView, ToolsView, KeysView, InputView, OutputView };
enum { WifiAction=1, BleAction, InputAction, CloseAction, StopAction, ScrollUpAction, ScrollDownAction, PanLeftAction, PanRightAction, LiveAction };
enum { StatusEvent=1, EnteredEvent, MenuEventBase=1000 };
enum { ShowTerminal=20, ShowTools, ShowInput, ShowKeys, ShowMenu, ShowDiagnostics, ShowCompactPrompt };
#define TOOL_BASE 100
#define KEY_BASE 300

typedef enum { NoMode, WifiMode, BleMode } PiMode;
typedef struct { FuriHalBleProfileBase base; BleSerial* serial; } PiProfile;
typedef struct { uint32_t action; char data[COMMAND_SIZE]; } PiMessage;

typedef struct {
    Gui* gui;
    FuriPubSub* input_events;
    FuriPubSubSubscription* input_subscription;
    uint16_t* stripe;
    bool native_terminal;
    uint32_t drawn_revision;
    ViewDispatcher* dispatcher;
    Submenu *menu, *tools, *keys;
    TextBox* box;
    TextInput* input;
    View* terminal;
    FuriThread* worker;
    FuriMessageQueue* actions;
    FuriStreamBuffer* rx;
    FuriSemaphore* tx_done;
    FuriMutex* mutex;
    Wifi* wifi;
    Bt* bt;
    PiProfile* profile;
    PiMode mode;
    bool initial_wifi;
    BtSettings initial_bt;
    bool closing;
    unsigned current_view;
    int selected_tool, field_index;
    char fields[3][96];
    char input_text[COMMAND_SIZE];
    char host[128], token[65], session[33];
    char status[STATUS_SIZE], displayed[STATUS_SIZE];
    char* cert;
    char* wire;
    size_t wire_used;
    bool overflow;
    esp_http_client_handle_t http;
    int scroll, pan;
} PiApp;
static void profile_gap(GapConfig* config, FuriHalBleProfileParams params) {
    UNUSED(params);
    memset(config, 0, sizeof(*config));
    strcpy(config->adv_name, "T-Embed Flip Pi");
    config->bonding_mode = true;
    config->pairing_method = GapPairingPinCodeDisplayOnly;
}
static const FuriHalBleProfileTemplate pi_template;
static FuriHalBleProfileBase* profile_start(FuriHalBleProfileParams params) {
    UNUSED(params);
    PiProfile* p = calloc(1, sizeof(*p));
    if(!p) return NULL;
    p->base.config = &pi_template;
    BleSerialConfig cfg = {.bonding = true, .require_encryption = true,
                          .pairing = BleSerialPairingDisplayOnly};
    strcpy(cfg.device_name, "T-Embed Flip Pi");
    p->serial = ble_serial_alloc(&cfg);
    if(!p->serial) { free(p); return NULL; }
    return &p->base;
}
static void profile_stop(FuriHalBleProfileBase* base) {
    PiProfile* p = (PiProfile*)base;
    ble_serial_set_event_callback(p->serial, 0, NULL, NULL);
    ble_serial_free(p->serial);
    free(p);
}
static const FuriHalBleProfileTemplate pi_template = {
    .start = profile_start, .stop = profile_stop, .get_gap_config = profile_gap,
};
static uint16_t ble_event(SerialServiceEvent event, void* context) {
    PiApp* app = context;
    if(event.event == SerialServiceEventTypeDataReceived) {
        size_t sent = furi_stream_buffer_send(app->rx, event.data.buffer, event.data.size, 0);
        if(sent != event.data.size) __atomic_store_n(&app->overflow, true, __ATOMIC_RELEASE);
    } else if(event.event == SerialServiceEventTypeDataSent) {
        furi_semaphore_release(app->tx_done);
    }
    return 0;
}
static char* read_file(Storage* storage, const char* path, size_t max) {
    File* f = storage_file_alloc(storage);
    char* result = NULL;
    if(storage_file_open(f, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        uint64_t size = storage_file_size(f);
        if(size && size <= max) {
            result = calloc(1, (size_t)size + 1);
            if(result && storage_file_read(f, result, size) != size) {
                free(result); result = NULL;
            }
        }
    }
    storage_file_close(f); storage_file_free(f);
    return result;
}
static bool load_config(PiApp* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    char* config = read_file(storage, CONFIG_PATH, 512);
    app->cert = read_file(storage, CERT_PATH, 4096);
    furi_record_close(RECORD_STORAGE);
    if(!config) return false;
    char* save = NULL;
    for(char* line = strtok_r(config, "\r\n", &save); line;
        line = strtok_r(NULL, "\r\n", &save)) {
        if(strncmp(line, "host=", 5) == 0) strlcpy(app->host, line + 5, sizeof(app->host));
        if(strncmp(line, "token=", 6) == 0) strlcpy(app->token, line + 6, sizeof(app->token));
    }
    free(config);
    if(strlen(app->token) != 64) return false;
    for(size_t i = 0; i < 64; i++) {
        if(!((app->token[i] >= '0' && app->token[i] <= '9') ||
             (app->token[i] >= 'a' && app->token[i] <= 'f'))) return false;
    }
    return true;
}
static void result(PiApp* app, const char* text) {
    if(__atomic_load_n(&app->closing, __ATOMIC_ACQUIRE)) return;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    strlcpy(app->status, text, sizeof(app->status));
    furi_mutex_release(app->mutex);
    view_dispatcher_send_custom_event(app->dispatcher, StatusEvent);
}
/* Persist only execution phases, never commands, tokens or output. */
static void trace_phase(const char* phase) {
    FURI_LOG_I("FlipPi", "%s: free=%lu largest=%lu internal=%lu", phase,
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT),
        (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    if(file) {
        if(storage_file_open(file, DIAG_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS))
            storage_file_write(file, phase, strlen(phase));
        storage_file_close(file); storage_file_free(file);
    }
    furi_record_close(RECORD_STORAGE);
}
static void stop_ble(PiApp* app) {
    if(app->profile) {
        bt_disconnect(app->bt);
        bt_profile_restore_default(app->bt); /* destroys profile + unregisters callback */
        app->profile = NULL;
    }
}
static esp_err_t http_event(esp_http_client_event_t* event) {
    PiApp* app = event->user_data;
    if(event->event_id == HTTP_EVENT_ON_DATA) {
        if(event->data_len < 0 || app->wire_used + event->data_len >= WIRE_SIZE) {
            app->overflow = true;
            return ESP_FAIL;
        }
        memcpy(app->wire + app->wire_used, event->data, event->data_len);
        app->wire_used += event->data_len;
        app->wire[app->wire_used] = 0;
    }
    return ESP_OK;
}
static bool wifi_exchange(PiApp* app, const char* request) {
    if(!wifi_is_connected(app->wifi)) { result(app, "Wi-Fi disconnected. Reconnect in WLAN settings."); return false; }
    if(!app->cert || !app->host[0]) { result(app, "Missing host or pi.crt in apps_data/flip_pi."); return false; }
    char url[170];
    snprintf(url, sizeof(url), "https://%s:8765/command", app->host);
    esp_http_client_config_t cfg = {
        .url = url, .cert_pem = app->cert, .timeout_ms = 5000,
        .event_handler = http_event, .user_data = app,
        .disable_auto_redirect = true,
        .buffer_size = 512, .buffer_size_tx = 512, .keep_alive_enable = true,
    };
    if(!app->http) {
        trace_phase("HTTPS client init");
        app->http = esp_http_client_init(&cfg);
    }
    esp_http_client_handle_t client = app->http;
    if(!client) { result(app, "Cannot allocate HTTPS client."); return false; }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, request, strlen(request));
    esp_err_t error = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    if(error != ESP_OK || status != 200 || app->overflow) {
        esp_http_client_cleanup(client); app->http = NULL;
        char message[160];
        snprintf(message, sizeof(message), "HTTPS failed: %s / HTTP %d\nCheck Pi service, certificate and host.", esp_err_to_name(error), status);
        result(app, message); return false;
    }
    return true;
}
static bool ble_exchange(PiApp* app, const char* request) {
    if(!app->profile || !ble_serial_is_connected(app->profile->serial)) {
        result(app, "BLE not connected. Start the Pi bridge and pair with this T-Embed."); return false;
    }
    uint8_t discard[64];
    while(furi_stream_buffer_receive(app->rx, discard, sizeof(discard), 0)) {}
    ble_serial_notify_buffer_is_empty(app->profile->serial);
    while(furi_semaphore_acquire(app->tx_done, 0) == FuriStatusOk) {}
    size_t length = strlen(request);
    /* 20-byte ATT payload works even before MTU negotiation. Wait for each
       indication acknowledgement instead of overwriting the in-flight packet. */
    for(size_t offset = 0; offset <= length; ) {
        uint8_t packet[20];
        size_t n = length - offset;
        if(n > sizeof(packet)) n = sizeof(packet);
        if(n) memcpy(packet, request + offset, n);
        else { packet[0] = '\n'; n = 1; }
        if(!ble_serial_tx(app->profile->serial, packet, n) ||
           furi_semaphore_acquire(app->tx_done, furi_ms_to_ticks(2000)) != FuriStatusOk) {
            result(app, "BLE send failed. Check pairing and bridge subscription."); return false;
        }
        if(offset == length) break;
        offset += n;
    }
    uint32_t started = furi_get_tick();
    while(furi_get_tick() - started < furi_ms_to_ticks(30000) && !__atomic_load_n(&app->closing, __ATOMIC_ACQUIRE)) {
        char c;
        if(furi_stream_buffer_receive(app->rx, &c, 1, furi_ms_to_ticks(100)) != 1) {
            if(!ble_serial_is_connected(app->profile->serial)) break;
            continue;
        }
        ble_serial_notify_buffer_is_empty(app->profile->serial);
        if(__atomic_load_n(&app->overflow, __ATOMIC_ACQUIRE)) break;
        if(c == '\n') { app->wire[app->wire_used] = 0; return true; }
        if(app->wire_used + 1 >= WIRE_SIZE) break;
        app->wire[app->wire_used++] = c;
    }
    result(app, "BLE response incomplete or timed out. Reconnect the Pi bridge.");
    return false;
}

static void close_http(PiApp* app) {
    if(app->http) { esp_http_client_cleanup(app->http); app->http = NULL; }
}
static void terminal_label(PiApp* app, const char* title) {
    with_view_model(app->terminal, TerminalModel* model, {
        if(strcmp(model->title,title)) {
            strlcpy(model->title, title, sizeof(model->title));
            model->revision++;
        }
    }, true);
}
static const char* json_string(cJSON* object, const char* key) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}
static bool exchange_terminal(PiApp* app, const char* op, const char* data) {
    if(app->mode == NoMode) { result(app, "Choose Connect Wi-Fi or Connect BLE first."); return false; }
    char id[17], scroll[12], pan[12];
    snprintf(id, sizeof(id), "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
    snprintf(scroll, sizeof(scroll), "%d", app->scroll);
    snprintf(pan, sizeof(pan), "%d", app->pan);
    cJSON* request = cJSON_CreateObject();
    if(!request) { result(app, "Not enough memory for request."); return false; }
    bool valid = cJSON_AddStringToObject(request, "id", id) &&
        cJSON_AddStringToObject(request, "token", app->token) &&
        cJSON_AddStringToObject(request, "op", op) &&
        cJSON_AddStringToObject(request, "session", app->session) &&
        cJSON_AddStringToObject(request, "scroll", scroll) &&
        cJSON_AddStringToObject(request, "pan", pan) &&
        cJSON_AddStringToObject(request, "cols", "26") &&
        cJSON_AddStringToObject(request, "rows", "8") &&
        (!data || cJSON_AddStringToObject(request, "data", data));
    char* encoded = valid ? cJSON_PrintUnformatted(request) : NULL;
    cJSON_Delete(request);
    if(!encoded) { result(app, "Not enough memory to encode request."); return false; }
    app->wire = calloc(1, WIRE_SIZE);
    if(!app->wire) { free(encoded); result(app, "Not enough memory for response."); return false; }
    app->wire_used = 0;
    __atomic_store_n(&app->overflow, false, __ATOMIC_RELEASE);
    bool ok = app->mode == WifiMode ? wifi_exchange(app, encoded) : ble_exchange(app, encoded);
    free(encoded);
    if(!ok) { free(app->wire); app->wire = NULL; return false; }
    cJSON* response = cJSON_Parse(app->wire);
    free(app->wire); app->wire = NULL;
    const char *rid = json_string(response, "id"), *token = json_string(response, "token"),
        *output = json_string(response, "output"), *state = json_string(response, "state"),
        *session = json_string(response, "session"), *cursor = json_string(response, "cursor");
    if(!rid || strcmp(rid,id) || !token || strcmp(token,app->token) || !output || !state || !session) {
        result(app, "Invalid terminal response. Update the Pi using install_terminal.sh.");
        ok = false;
    } else if(!strcmp(state, "error")) {
        result(app, output); ok = false;
    } else {
        strlcpy(app->session, !strcmp(state,"open") ? session : "", sizeof(app->session));
        int cy=-1, cx=-1;
        if(cursor) sscanf(cursor, "%d,%d", &cy, &cx);
        with_view_model(app->terminal, TerminalModel* model, {
            TerminalModel previous=*model;
            const char* p=output;
            for(unsigned y=0; y<PI_TERM_ROWS; y++) {
                unsigned x=0;
                while(*p && *p!='\n') { if(x<PI_TERM_COLS) model->rows[y][x++]=*p; p++; }
                model->rows[y][x]=0;
                if(*p=='\n') p++;
            }
            model->cursor_row=cy; model->cursor_col=cx;
            snprintf(model->title,sizeof(model->title),"%s %s S:%d X:%d",
                app->mode==WifiMode ? "WIFI" : "BLE", !strcmp(state,"open")?"LIVE":"CLOSED", app->scroll, app->pan);
            if(memcmp(&previous,model,sizeof(previous)))model->revision++;
        }, true);
    }
    cJSON_Delete(response);
    return ok;
}

static int32_t worker(void* context) {
    PiApp* app=context;
    PiMessage message;
    while(true) {
        FuriStatus received=furi_message_queue_get(app->actions,&message,
            app->session[0] ? furi_ms_to_ticks(app->mode==WifiMode ? 400 : 900) : FuriWaitForever);
        if(received != FuriStatusOk) {
            if(app->session[0] && !exchange_terminal(app,"poll",NULL)) {
                app->session[0]=0; terminal_label(app,"Disconnected: reconnect");
            }
            continue;
        }
        if(message.action==StopAction || __atomic_load_n(&app->closing, __ATOMIC_ACQUIRE)) break;
        if(!app->token[0]) { result(app,"Missing configuration. Copy config.txt and pi.crt to SD/apps_data/flip_pi."); continue; }
        if(message.action==WifiAction || message.action==BleAction) {
            app->session[0]=0; close_http(app); stop_ble(app); app->mode=NoMode;
            app->scroll=0; app->pan=0;
            trace_phase("Select transport");
            if(message.action==WifiAction) {
                bt_stop_stack(app->bt);
                wifi_enable(app->wifi);
                for(unsigned i=0;i<100 && !__atomic_load_n(&app->closing, __ATOMIC_ACQUIRE) && !wifi_is_connected(app->wifi);i++) furi_delay_ms(100);
                if(!wifi_is_connected(app->wifi)) {result(app,"Connect and save the Pi's LAN in WLAN settings first.");continue;}
                app->mode=WifiMode;
            } else {
                wifi_disable(app->wifi);
                BtSettings settings; bt_get_settings(app->bt,&settings);
                settings.enabled=true; bt_set_settings(app->bt,&settings); bt_start_stack(app->bt);
                app->profile=(PiProfile*)bt_profile_start(app->bt,&pi_template,NULL);
                if(!app->profile) {result(app,"BLE profile could not start.");continue;}
                ble_serial_set_event_callback(app->profile->serial,512,ble_event,app);
                app->mode=BleMode;
                terminal_label(app,"Waiting for Pi BLE...");
                for(unsigned i=0;i<150 && !__atomic_load_n(&app->closing, __ATOMIC_ACQUIRE) && !ble_serial_is_connected(app->profile->serial);i++) furi_delay_ms(100);
            }
            trace_phase("Open SSH session");
            if(exchange_terminal(app,"open",NULL)) trace_phase("SSH connected");
        } else if(message.action==InputAction) {
            if(!app->session[0]) { result(app,"SSH is not connected. Choose Connect Wi-Fi or Connect BLE.");continue; }
            app->scroll=0;
            trace_phase("Send terminal input");
            if(!exchange_terminal(app,"input",message.data)) app->session[0]=0;
            else trace_phase("SSH connected");
            memset(message.data,0,sizeof(message.data));
        } else if(message.action==CloseAction) {
            if(app->session[0]) exchange_terminal(app,"close",NULL);
            app->session[0]=0; close_http(app); terminal_label(app,"SSH disconnected");
            trace_phase("Command finished");
        } else {
            if(message.action==ScrollUpAction && app->scroll<224) app->scroll++;
            if(message.action==ScrollDownAction && app->scroll>0) app->scroll--;
            if(message.action==PanLeftAction) app->pan=app->pan>=10 ? app->pan-10 : 0;
            if(message.action==PanRightAction) app->pan=app->pan<49 ? app->pan+10 : 59;
            if(message.action==LiveAction) {app->scroll=0;app->pan=0;}
            if(app->session[0] && !exchange_terminal(app,"poll",NULL)) app->session[0]=0;
        }
    }
    if(app->session[0]) exchange_terminal(app,"close",NULL);
    close_http(app); stop_ble(app);
    if(app->initial_wifi) wifi_enable(app->wifi);
    else if(wifi_is_enabled(app->wifi)) wifi_disable(app->wifi);
    bt_set_settings(app->bt,&app->initial_bt);
    if(app->initial_bt.enabled && !app->initial_wifi) bt_start_stack(app->bt);
    else if(!app->initial_wifi) bt_stop_stack(app->bt);
    trace_phase("Command finished");
    return 0;
}
static void terminal_native_set(PiApp* app,bool enabled) {
    if(enabled==__atomic_load_n(&app->native_terminal,__ATOMIC_ACQUIRE))return;
    if(enabled) {
        gui_direct_draw_acquire(app->gui);
        app->drawn_revision=UINT32_MAX;
        __atomic_store_n(&app->native_terminal,true,__ATOMIC_RELEASE);
    } else {
        __atomic_store_n(&app->native_terminal,false,__ATOMIC_RELEASE);
        gui_direct_draw_release(app->gui);
    }
}
static void show(PiApp* app,unsigned view) {
    if(view!=TerminalView)terminal_native_set(app,false);
    app->current_view=view;
    view_dispatcher_switch_to_view(app->dispatcher,view);
    if(view==TerminalView)terminal_native_set(app,true);
}
static void notice(PiApp* app,const char* message) {
    strlcpy(app->displayed,message,sizeof(app->displayed));
    text_box_set_text(app->box,app->displayed); show(app,OutputView);
}
static bool enqueue(PiApp* app,uint32_t action,const char* data) {
    PiMessage message={.action=action};
    if(data) strlcpy(message.data,data,sizeof(message.data));
    if(furi_message_queue_put(app->actions,&message,0)!=FuriStatusOk) {
        notice(app,"Terminal busy. Wait for the current request, then retry."); return false;
    }
    return true;
}
static void menu_callback(void* context,uint32_t index) {
    PiApp* app=context;
    view_dispatcher_send_custom_event(app->dispatcher,MenuEventBase+index);
}
static void entered(void* context) {
    PiApp* app=context;
    /* TextInput still holds its model lock: never switch views here. */
    view_dispatcher_send_custom_event(app->dispatcher,EnteredEvent);
}
static void edit_input(PiApp* app,const char* title,size_t limit) {
    text_input_set_header_text(app->input,title);
    text_input_set_result_callback(app->input,entered,app,app->input_text,limit,false);
    show(app,InputView);
}
static void next_field(PiApp* app) {
    const PiTool* tool=&pi_tools[app->selected_tool];
    if((unsigned)app->field_index<tool->count) {
        strlcpy(app->input_text,tool->defaults[app->field_index],sizeof(app->input_text));
        edit_input(app,tool->fields[app->field_index],96);
    } else {
        if(!pi_tool_command(tool,app->fields,app->input_text,sizeof(app->input_text)-1)) {
            notice(app,"Invalid setting or command too long. Use printable values without a leading dash.");return;
        }
        edit_input(app,"Review command; Save runs",sizeof(app->input_text)-1);
    }
}
static const struct {const char* name;const char* data;} keys[]={
    {"Ctrl+C / stop","\003"},{"Enter","\r"},{"Tab / completion","\t"},
    {"Escape","\033"},{"Up / history","\033[A"},{"Down / history","\033[B"},
    {"Left","\033[D"},{"Right","\033[C"},{"Backspace","\177"},
    {"Ctrl+D / EOF","\004"},{"Ctrl+Z / suspend","\032"},{"Ctrl+L / redraw","\014"},
    {"Home","\033[H"},{"End","\033[F"},{"Page Up","\033[5~"},{"Page Down","\033[6~"},
};
/* The GUI view exists for navigation; terminal pixels use native RGB stripes. */
static void terminal_draw(Canvas* canvas,void* context) {
    UNUSED(context);canvas_clear(canvas);
}
static void terminal_tick(void* context) {
    PiApp* app=context;
    if(!__atomic_load_n(&app->native_terminal,__ATOMIC_ACQUIRE))return;
    TerminalModel snapshot;
    with_view_model(app->terminal,TerminalModel* m,{snapshot=*m;},false);
    if(snapshot.revision==app->drawn_revision)return;
    for(unsigned y=0;y<PI_TERM_HEIGHT;y+=8) {
        pi_terminal_stripe(&snapshot,y,app->stripe);
        unsigned height=y+8>PI_TERM_HEIGHT?PI_TERM_HEIGHT-y:8;
        if(!furi_hal_display_draw_rgb565(0,y,PI_TERM_WIDTH,height,app->stripe))return;
    }
    app->drawn_revision=snapshot.revision;
}
static void terminal_native_input(const void* value,void* context) {
    PiApp* app=context;
    if(!__atomic_load_n(&app->native_terminal,__ATOMIC_ACQUIRE))return;
    const InputEvent* event=value;
    if(event->type!=InputTypeShort&&event->type!=InputTypeRepeat&&event->type!=InputTypeLong)return;
    uint32_t action=0;
    if(event->key==InputKeyBack)action=ShowMenu;
    else if(event->key==InputKeyUp)action=ScrollUpAction;
    else if(event->key==InputKeyDown)action=ScrollDownAction;
    else if(event->key==InputKeyLeft)action=PanLeftAction;
    else if(event->key==InputKeyRight)action=PanRightAction;
    else if(event->key==InputKeyOk)action=event->type==InputTypeLong?ShowInput:ShowMenu;
    if(action)view_dispatcher_send_custom_event(app->dispatcher,MenuEventBase+action);
}
static bool terminal_input(InputEvent* event,void* context) {
    UNUSED(event);UNUSED(context);return false;
}
static void menu_action(PiApp* app,uint32_t index) {
    if(index==ShowMenu) show(app,MenuView);
    else if(index==ShowTerminal) show(app,TerminalView);
    else if(index==ShowTools) show(app,ToolsView);
    else if(index==ShowKeys) show(app,KeysView);
    else if(index==ShowCompactPrompt) {
        if(enqueue(app,InputAction,"if [ -n \"$ZSH_VERSION\" ]; then PROMPT='kali@pi:%~%# '; RPROMPT=''; else PS1='kali@pi:\\w\\$ '; fi\r"))show(app,TerminalView);
    } else if(index==ShowInput) {
        app->selected_tool=-1; app->input_text[0]=0;
        edit_input(app,"Type input; Save sends Enter",sizeof(app->input_text)-1);
    } else if(index==ShowDiagnostics) {
        Storage* storage=furi_record_open(RECORD_STORAGE);
        char* phase=read_file(storage,DIAG_PATH,96);furi_record_close(RECORD_STORAGE);
        snprintf(app->displayed,sizeof(app->displayed),"Flip Pi terminal 2.1\nLast phase: %s\nFree heap: %lu\nLargest block: %lu\nWorker stack: 24 KB",phase?phase:"none",(unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT),(unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        free(phase); text_box_set_text(app->box,app->displayed);show(app,OutputView);
    } else if(index>=TOOL_BASE && index<TOOL_BASE+PI_TOOL_COUNT) {
        app->selected_tool=index-TOOL_BASE;app->field_index=0;memset(app->fields,0,sizeof(app->fields));next_field(app);
    } else if(index>=KEY_BASE && index<KEY_BASE+sizeof(keys)/sizeof(keys[0])) {
        if(enqueue(app,InputAction,keys[index-KEY_BASE].data)) show(app,TerminalView);
    } else if(index>=WifiAction && index<=LiveAction && index!=InputAction && index!=StopAction) {
        if(enqueue(app,index,NULL)) show(app,TerminalView);
    }
}
static bool custom_event(void* context,uint32_t event) {
    PiApp* app=context;
    if(app->closing) return false;
    if(event>=MenuEventBase) {menu_action(app,event-MenuEventBase);return true;}
    if(event==EnteredEvent) {
        if(app->selected_tool>=0 && (unsigned)app->field_index<pi_tools[app->selected_tool].count) {
            strlcpy(app->fields[app->field_index++],app->input_text,96);next_field(app);
        } else {
            size_t n=strlen(app->input_text);
            if(n+2>sizeof(app->input_text)) {notice(app,"Input too long.");return true;}
            app->input_text[n]='\r';app->input_text[n+1]=0;
            if(enqueue(app,InputAction,app->input_text)) show(app,TerminalView);
            memset(app->input_text,0,sizeof(app->input_text));app->selected_tool=-1;
        }
        return true;
    }
    if(event==StatusEvent) {
        furi_mutex_acquire(app->mutex,FuriWaitForever);
        strlcpy(app->displayed,app->status,sizeof(app->displayed));
        furi_mutex_release(app->mutex);
        text_box_set_text(app->box,app->displayed);show(app,OutputView);return true;
    }
    return false;
}
static bool back(void* context) {
    PiApp* app=context;
    if(app->current_view==MenuView) {__atomic_store_n(&app->closing,true,__ATOMIC_RELEASE);view_dispatcher_stop(app->dispatcher);}
    else show(app,MenuView);
    return true;
}
int32_t flip_pi_app(void* context) {
    UNUSED(context);
    PiApp* app=calloc(1,sizeof(*app));furi_check(app);
    app->selected_tool=-1;
    app->mutex=furi_mutex_alloc(FuriMutexTypeNormal);
    app->actions=furi_message_queue_alloc(3,sizeof(PiMessage));
    app->rx=furi_stream_buffer_alloc(512,1);app->tx_done=furi_semaphore_alloc(1,0);
    app->gui=furi_record_open(RECORD_GUI);app->wifi=furi_record_open(RECORD_WIFI);app->bt=furi_record_open(RECORD_BT);
    app->stripe=heap_caps_malloc(320*8*sizeof(uint16_t),MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA);
    furi_check(app->stripe);
    app->input_events=furi_record_open(RECORD_INPUT_EVENTS);
    app->initial_wifi=wifi_is_enabled(app->wifi);bt_get_settings(app->bt,&app->initial_bt);
    if(!load_config(app)) app->token[0]=0;
    app->dispatcher=view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->dispatcher,app);
    view_dispatcher_set_custom_event_callback(app->dispatcher,custom_event);
    view_dispatcher_set_navigation_event_callback(app->dispatcher,back);
    view_dispatcher_set_tick_event_callback(app->dispatcher,terminal_tick,80);
    app->menu=submenu_alloc();app->tools=submenu_alloc();app->keys=submenu_alloc();
    app->box=text_box_alloc();app->input=text_input_alloc();app->terminal=view_alloc();
    view_set_context(app->terminal,app);view_allocate_model(app->terminal,ViewModelTypeLocking,sizeof(TerminalModel));
    view_set_draw_callback(app->terminal,terminal_draw);view_set_input_callback(app->terminal,terminal_input);
    with_view_model(app->terminal,TerminalModel* model,{
        memset(model,0,sizeof(*model));model->cursor_row=-1;model->cursor_col=-1;
        strlcpy(model->title,"Choose Connect in menu",sizeof(model->title));
    },false);
    submenu_set_header(app->menu,"Flip Pi SSH terminal");
    submenu_add_item(app->menu,"Connect Wi-Fi",WifiAction,menu_callback,app);
    submenu_add_item(app->menu,"Connect BLE",BleAction,menu_callback,app);
    submenu_add_item(app->menu,"Terminal",ShowTerminal,menu_callback,app);
    submenu_add_item(app->menu,"Type input / command",ShowInput,menu_callback,app);
    submenu_add_item(app->menu,"Kali tools",ShowTools,menu_callback,app);
    submenu_add_item(app->menu,"Special keys",ShowKeys,menu_callback,app);
    submenu_add_item(app->menu,"Compact shell prompt",ShowCompactPrompt,menu_callback,app);
    submenu_add_item(app->menu,"Return to live view",LiveAction,menu_callback,app);
    submenu_add_item(app->menu,"Pan left",PanLeftAction,menu_callback,app);
    submenu_add_item(app->menu,"Pan right",PanRightAction,menu_callback,app);
    submenu_add_item(app->menu,"Disconnect SSH",CloseAction,menu_callback,app);
    submenu_add_item(app->menu,"Diagnostics",ShowDiagnostics,menu_callback,app);
    submenu_set_header(app->tools,"Kali command templates");
    for(unsigned i=0;i<PI_TOOL_COUNT;i++) submenu_add_item(app->tools,pi_tools[i].name,TOOL_BASE+i,menu_callback,app);
    submenu_set_header(app->keys,"Send terminal key");
    for(unsigned i=0;i<sizeof(keys)/sizeof(keys[0]);i++) submenu_add_item(app->keys,keys[i].name,KEY_BASE+i,menu_callback,app);
    view_dispatcher_add_view(app->dispatcher,MenuView,submenu_get_view(app->menu));
    view_dispatcher_add_view(app->dispatcher,ToolsView,submenu_get_view(app->tools));
    view_dispatcher_add_view(app->dispatcher,KeysView,submenu_get_view(app->keys));
    view_dispatcher_add_view(app->dispatcher,TerminalView,app->terminal);
    view_dispatcher_add_view(app->dispatcher,InputView,text_input_get_view(app->input));
    view_dispatcher_add_view(app->dispatcher,OutputView,text_box_get_view(app->box));
    view_dispatcher_attach_to_gui(app->dispatcher,app->gui,ViewDispatcherTypeFullscreen);
    app->input_subscription=furi_pubsub_subscribe(app->input_events,terminal_native_input,app);
    show(app,MenuView);
    app->worker=furi_thread_alloc_ex("FlipPiSSH",24576,worker,app);furi_thread_start(app->worker);
    view_dispatcher_run(app->dispatcher);
    terminal_native_set(app,false);
    furi_pubsub_unsubscribe(app->input_events,app->input_subscription);
    furi_record_close(RECORD_INPUT_EVENTS);
    PiMessage stop={.action=StopAction};furi_message_queue_put(app->actions,&stop,FuriWaitForever);
    furi_thread_join(app->worker);furi_thread_free(app->worker);
    for(unsigned v=MenuView;v<=OutputView;v++) view_dispatcher_remove_view(app->dispatcher,v);
    view_dispatcher_free(app->dispatcher);
    submenu_free(app->menu);submenu_free(app->tools);submenu_free(app->keys);
    view_free(app->terminal);text_box_free(app->box);text_input_free(app->input);
    furi_record_close(RECORD_GUI);furi_record_close(RECORD_WIFI);furi_record_close(RECORD_BT);
    furi_message_queue_free(app->actions);furi_stream_buffer_free(app->rx);
    furi_semaphore_free(app->tx_done);furi_mutex_free(app->mutex);
    memset(app->token,0,sizeof(app->token));free(app->cert);free(app->wire);free(app->stripe);free(app);
    return 0;
}
