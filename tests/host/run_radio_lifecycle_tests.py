#!/usr/bin/env python3
"""Compile the real core handoff functions against fault-injecting host backends.

Function bodies are read from production sources, not reimplemented in tests.
These tests verify state/cleanup contracts; they do not emulate Espressif radio RF.
"""
from pathlib import Path
from host_build import run as run_native
import subprocess
ROOT=Path(__file__).resolve().parents[2]
OUTPUT=ROOT/'build_t_embed/host_tests'

def function(source,name):
    # The selected C functions have no brace characters in string literals.
    import re
    match=re.search(r'^((?:static )?[\w *]+\b'+name+r'\([^;]*?\)\s*\{)',source,re.M)
    if not match: raise ValueError(name)
    begin=match.start(); opening=source.index('{',match.start())
    depth=1; end=opening+1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}'); end+=1
    return source[begin:end]

def run(name,code,extra_sources=()):
    source=OUTPUT/(name+'.c'); binary=OUTPUT/name
    source.write_text(code)
    run_native(['gcc','-std=c17','-Wall','-Wextra','-Werror','-O1','-g',
        '-fsanitize=address,undefined','-I'+str(ROOT/'tests/host/wardriver_host'),
        '-I'+str(ROOT/'applications_user/wardriver'),str(source),*map(str,extra_sources),'-o',str(binary)],check=True)
    run_native([str(binary)],check=True)

common=r'''
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define UNUSED(x) (void)(x)
#define TAG "test"
#define ESP_LOGI(...) ((void)0)
#define FURI_LOG_I(...) ((void)0)
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); abort(); } } while(0)
enum { ESP_BT_CONTROLLER_STATUS_IDLE, ESP_BT_CONTROLLER_STATUS_INITED, ESP_BT_CONTROLLER_STATUS_ENABLED };
enum { ESP_BLUEDROID_STATUS_UNINITIALIZED, ESP_BLUEDROID_STATUS_INITIALIZED, ESP_BLUEDROID_STATUS_ENABLED };
static int controller,host;
static int esp_bt_controller_get_status(void) { return controller; }
static int esp_bluedroid_get_status(void) { return host; }
static void furi_delay_ms(unsigned x) { (void)x; }
'''
bt=r'''
typedef struct Profile Profile;
typedef struct { void (*stop)(Profile*); } ProfileConfig;
struct Profile { ProfileConfig* config; };
typedef struct { Profile* current_profile; int status; } Bt;
enum { BtStatusOff, BtStatusAdvertising };
static unsigned stop_calls,disable_calls,deinit_calls,resets;
static void bt_close_rpc_connection(Bt* bt) { (void)bt; }
static void furi_hal_bt_stop_advertising(void) {}
static void ble_serial_reset_initialized(void) { ++resets; }
static void furi_hal_bt_reinit(void) {}
static void esp_bluedroid_disable(void) { CHECK(host==ESP_BLUEDROID_STATUS_ENABLED); host=ESP_BLUEDROID_STATUS_INITIALIZED; ++disable_calls; }
static void esp_bluedroid_deinit(void) { CHECK(host==ESP_BLUEDROID_STATUS_INITIALIZED); host=ESP_BLUEDROID_STATUS_UNINITIALIZED; ++deinit_calls; }
static void esp_bt_controller_disable(void) { CHECK(controller==ESP_BT_CONTROLLER_STATUS_ENABLED); controller=ESP_BT_CONTROLLER_STATUS_INITED; ++disable_calls; }
static void esp_bt_controller_deinit(void) { CHECK(controller==ESP_BT_CONTROLLER_STATUS_INITED); controller=ESP_BT_CONTROLLER_STATUS_IDLE; ++deinit_calls; }
static void stop_profile(Profile* p) { (void)p; ++stop_calls; }
'''
bt_source=(ROOT/'components/btshim/btshim.c').read_text(encoding='utf-8')
run('bt_handoff_test',common+bt+function(bt_source,'bt_handle_stop_stack')+r'''
int main(void) {
    Bt b={0};
    /* The advertising icon is OFF, but controller/host are still enabled. */
    controller=ESP_BT_CONTROLLER_STATUS_ENABLED; host=ESP_BLUEDROID_STATUS_ENABLED;
    bt_handle_stop_stack(&b);
    CHECK(!controller && !host && disable_calls==2 && deinit_calls==2 && resets==1);
    /* Disabled (INITED) controller must still be deinitialized. */
    controller=ESP_BT_CONTROLLER_STATUS_INITED;
    bt_handle_stop_stack(&b); CHECK(!controller && disable_calls==2 && deinit_calls==3);
    unsigned before=resets; bt_handle_stop_stack(&b); CHECK(resets==before);
    ProfileConfig config={.stop=stop_profile}; Profile p={.config=&config};
    b.current_profile=&p; b.status=BtStatusAdvertising;
    controller=ESP_BT_CONTROLLER_STATUS_ENABLED; host=ESP_BLUEDROID_STATUS_ENABLED;
    bt_handle_stop_stack(&b);
    CHECK(stop_calls==1 && !b.current_profile && b.status==BtStatusOff && !controller && !host);
    puts("PASS: actual Bluetooth shutdown releases nonadvertising and INITED controllers; repeated stop is idempotent");
    return 0;
}
''')

wifi=r'''
#include <stdint.h>
#define ESP_FAIL (-1)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
static const char* esp_err_to_name(esp_err_t e) { (void)e; return "err"; }
typedef int wifi_mode_t;
enum { WIFI_MODE_NULL, WIFI_MODE_STA, WIFI_MODE_AP, WIFI_IF_STA };
typedef struct { char credentials[64]; } wifi_config_t;
typedef struct { int unused; } wifi_ap_record_t;
typedef int Bt;
#define RECORD_BT "bt"
#define WLAN_CMD_SYNC_TIMEOUT_MS 5000U
#define WLAN_RUNFN_TIMEOUT_MS 20000U
typedef void (*WlanHalWorkerFn)(void*);
enum { WCMD_INIT_START, WCMD_STOP_DEINIT, WCMD_RUN_FN };
typedef struct {
    int type; bool survey_owned;
    struct { WlanHalWorkerFn fn; void* arg; } run_fn;
} WlanCmd;

/* Production state (same names as components/wifi/wlan_hal.c). */
static bool s_started,s_bt_was_on,s_light_sleep_inhibited;
static volatile bool s_channel_locked;
static bool s_wifi_connected,s_wifi_auto_reconnect,s_auth_fail_latched;
static volatile bool s_survey_active;
static bool s_survey_bt_restore,s_survey_resume_wifi,s_survey_inhibited;
static const char* s_survey_status="RADIO READY";
static wifi_config_t s_survey_sta_config;
static bool s_survey_sta_config_valid,s_survey_sta_reconnect,s_survey_auth_fail_latched;
static esp_err_t s_survey_sta_error;

/* Fault-injecting SDK/OS backend. */
static bool worker_ok=true,other_app;
static wifi_mode_t mode=WIFI_MODE_STA;
static wifi_config_t driver_config;
static esp_err_t set_config_error;
static unsigned wifi_starts,wifi_stops,survey_starts,survey_stops,rejected_cmds;
static unsigned reconnects,bt_starts,bt_stops,fail_start_number;
static int inhibit_depth;
static bool driver_up;
static void furi_hal_power_light_sleep_inhibit_enter(void) { ++inhibit_depth; }
static void furi_hal_power_light_sleep_inhibit_exit(void) { CHECK(inhibit_depth>0); --inhibit_depth; }
static bool wlan_connection_sleep_inhibit_mutex_init(void) { return true; }
static void wlan_set_connection_sleep_inhibited(bool inhibited) { (void)inhibited; }
static bool wlan_hal_beacon_spam_is_running(void) { return other_app; }
static bool wlan_hal_evil_portal_is_running(void) { return false; }
static bool wlan_ensure_worker(void) { return worker_ok; }
static Bt* furi_record_open(const char* record) { CHECK(!strcmp(record,RECORD_BT)); static Bt b; return &b; }
static void furi_record_close(const char* record) { CHECK(!strcmp(record,RECORD_BT)); }
static bool bt_is_enabled(Bt* b) { (void)b; return controller==ESP_BT_CONTROLLER_STATUS_ENABLED; }
static void bt_stop_stack(Bt* b) { (void)b; ++bt_stops; controller=0; host=0; }
static void bt_start_stack(Bt* b) { (void)b; ++bt_starts; controller=2; host=2; }
static esp_err_t esp_wifi_get_mode(wifi_mode_t* p) { *p=mode; return ESP_OK; }
static esp_err_t esp_wifi_get_config(int i,wifi_config_t* p) { CHECK(driver_up&&i==WIFI_IF_STA); *p=driver_config; return ESP_OK; }
static esp_err_t esp_wifi_set_config(int i,const wifi_config_t* p) {
    CHECK(driver_up&&i==WIFI_IF_STA); if(set_config_error) return set_config_error; driver_config=*p; return ESP_OK;
}
static esp_err_t esp_wifi_connect(void) { CHECK(driver_up&&!s_survey_active); ++reconnects; s_wifi_connected=true; return ESP_OK; }
static bool survey_active(void);
/* The worker: rejects foreign commands during a survey, like wlan_worker_task. */
static bool wlan_cmd_exec(WlanCmd* cmd,uint32_t timeout_ms,bool* out_result,wifi_ap_record_t** r,uint16_t* n) {
    (void)timeout_ms; (void)r; (void)n;
    if(out_result) *out_result=false;
    if(survey_active() && !cmd->survey_owned) { ++rejected_cmds; return true; }
    switch(cmd->type) {
    case WCMD_INIT_START:
        CHECK(!driver_up);
        if(cmd->survey_owned) ++survey_starts; else ++wifi_starts;
        if(++fail_start_number==1) return true; /* armed by setting to 0 */
        driver_up=true; if(out_result) *out_result=true; return true;
    case WCMD_STOP_DEINIT:
        if(cmd->survey_owned) ++survey_stops; else ++wifi_stops;
        s_wifi_auto_reconnect=false; s_wifi_connected=false; driver_up=false; return true;
    case WCMD_RUN_FN:
        cmd->run_fn.fn(cmd->run_fn.arg); return true;
    }
    return false;
}
static void wlan_send_cmd_sync(WlanCmd* cmd) { wlan_cmd_exec(cmd,WLAN_CMD_SYNC_TIMEOUT_MS,NULL,NULL,NULL); }
bool wlan_hal_start(void);
void wlan_hal_stop(void);
bool wlan_hal_run_in_worker(WlanHalWorkerFn fn,void* arg);
'''
wifi_source=(ROOT/'components/wifi/wlan_hal.c').read_text(encoding='utf-8')
functions=['survey_save_station','survey_restore_station','wlan_hal_survey_status','survey_status',
    'survey_active','survey_resume_normal_wifi','survey_radio_start','survey_radio_stop',
    'wlan_hal_survey_begin','wlan_hal_survey_end','wlan_start_failed_cleanup','wlan_hal_start',
    'wlan_hal_stop','wlan_hal_run_in_worker']
run('wifi_handoff_test',common+wifi+'\n'.join(function(wifi_source,n) for n in functions)+r'''
#define NO_FAIL 2 /* fail_start_number is pre-incremented: 0 fails the next start */
static void reset(void) {
    s_started=s_bt_was_on=s_light_sleep_inhibited=s_channel_locked=false;
    s_wifi_connected=s_wifi_auto_reconnect=s_auth_fail_latched=false;
    s_survey_active=s_survey_bt_restore=s_survey_resume_wifi=s_survey_inhibited=false;
    s_survey_status="RADIO READY"; s_survey_sta_config_valid=s_survey_sta_reconnect=false;
    s_survey_auth_fail_latched=false; s_survey_sta_error=ESP_OK;
    memset(&s_survey_sta_config,0,sizeof(s_survey_sta_config));
    controller=host=0; worker_ok=true; other_app=false; mode=WIFI_MODE_STA; driver_up=false;
    set_config_error=ESP_OK; inhibit_depth=0; rejected_cmds=0; fail_start_number=NO_FAIL;
    strcpy(driver_config.credentials,"test prior station configuration");
}
static void connected_station(void) {
    CHECK(wlan_hal_start() && s_started && inhibit_depth==1);
    s_wifi_connected=s_wifi_auto_reconnect=true;
    wifi_starts=wifi_stops=survey_starts=survey_stops=reconnects=bt_starts=bt_stops=0;
}
static void counters(void) { wifi_starts=wifi_stops=survey_starts=survey_stops=reconnects=bt_starts=bt_stops=0; }
int main(void) {
    /* Shared helpers that the current production path only reaches via discarded logs. */
    (void)esp_err_to_name; (void)furi_delay_ms; (void)esp_bt_controller_get_status; (void)esp_bluedroid_get_status;
    /* Connected station: suspend, own the radio exclusively, then restore and reconnect once. */
    reset(); connected_station();
    CHECK(wlan_hal_survey_begin(true));
    CHECK(s_survey_active && wifi_stops==1 && survey_starts==1 && s_started && driver_up);
    CHECK(!s_wifi_connected && !s_wifi_auto_reconnect && !reconnects && inhibit_depth==1);
    CHECK(!strcmp(wlan_hal_survey_status(),"EXCLUSIVE RADIO READY"));
    CHECK(!wlan_hal_survey_begin(true)); /* A second owner must not overwrite saved state. */
    CHECK(s_survey_active && s_survey_sta_config_valid && survey_starts==1);
    /* Normal Wi-Fi API is locked out while the survey owns the radio. */
    CHECK(!wlan_hal_start()); wlan_hal_stop(); CHECK(driver_up && s_started);
    CHECK(!wlan_hal_run_in_worker(survey_save_station,NULL));
    wlan_hal_survey_end();
    CHECK(!s_survey_active && survey_stops==1 && wifi_starts==1 && s_started && driver_up);
    CHECK(s_wifi_auto_reconnect && s_wifi_connected && reconnects==1 && inhibit_depth==1);
    CHECK(!strcmp(driver_config.credentials,"test prior station configuration"));
    CHECK(!s_survey_sta_config.credentials[0] && !s_survey_sta_config_valid); /* cleared after restore */
    CHECK(!strcmp(wlan_hal_survey_status(),"RADIO RESTORED"));
    wlan_hal_survey_end(); CHECK(reconnects==1 && survey_stops==1); /* idempotent */

    /* Bluetooth on, Wi-Fi off: BT is stopped for the survey and restarted once. */
    reset(); controller=host=2; counters();
    CHECK(wlan_hal_survey_begin(true) && !controller && !host && bt_stops==1 && !wifi_starts);
    wlan_hal_survey_end();
    CHECK(!s_started && !driver_up && controller==2 && host==2 && bt_starts==1 && !wifi_starts);
    CHECK(!inhibit_depth && !s_survey_active);

    /* Wi-Fi was started while BT was on: end hands BT back to normal Wi-Fi ownership. */
    reset(); controller=host=2; connected_station();
    CHECK(!controller && s_bt_was_on);
    CHECK(wlan_hal_survey_begin(true) && !controller && s_survey_bt_restore);
    wlan_hal_survey_end();
    CHECK(s_started && reconnects==1 && !controller && s_bt_was_on && inhibit_depth==1);
    wlan_hal_stop(); CHECK(controller==2 && !inhibit_depth);

    /* Non-Wi-Fi survey still suspends station and reconnects afterwards. */
    reset(); connected_station();
    CHECK(wlan_hal_survey_begin(false) && !s_started && !driver_up && !survey_starts && !reconnects);
    wlan_hal_survey_end(); CHECK(s_started && reconnects==1 && inhibit_depth==1);

    /* Not a plain station (AP mode): refuse without touching radio or BT. */
    reset(); connected_station(); mode=WIFI_MODE_AP; controller=host=2;
    CHECK(!wlan_hal_survey_begin(true));
    CHECK(s_started && driver_up && !wifi_stops && !bt_stops && !s_survey_active && !s_survey_resume_wifi);
    CHECK(!strcmp(wlan_hal_survey_status(),"WI-FI STATE SAVE FAILED"));

    /* Survey radio fails to start: roll back BT and normal Wi-Fi, reconnect once. */
    reset(); connected_station(); controller=host=2; fail_start_number=0;
    CHECK(!wlan_hal_survey_begin(true));
    CHECK(!s_survey_active && survey_starts==1 && wifi_starts==1 && s_started && reconnects==1);
    CHECK(bt_starts==1 && bt_stops==2 && !controller && inhibit_depth==1); /* BT owned by Wi-Fi again */
    CHECK(!strcmp(wlan_hal_survey_status(),"WI-FI INIT FAILED"));

    /* Worker unavailable: no survey, BT restored, no light-sleep inhibit leaked. */
    reset(); controller=host=2; worker_ok=false; counters();
    CHECK(!wlan_hal_survey_begin(true) && controller==2 && host==2 && !s_survey_active && !inhibit_depth);

    /* Another radio app owns the radio: refuse without side effects. */
    reset(); other_app=true; controller=host=2; counters();
    CHECK(!wlan_hal_survey_begin(true) && !survey_starts && !bt_stops && !s_survey_active);
    CHECK(!strcmp(wlan_hal_survey_status(),"OTHER RADIO APP ACTIVE"));

    /* Station config cannot be restored: report it and never reconnect blindly. */
    reset(); connected_station();
    CHECK(wlan_hal_survey_begin(true)); set_config_error=ESP_FAIL;
    wlan_hal_survey_end();
    CHECK(!s_survey_active && s_started && !reconnects && !s_survey_sta_config_valid);
    CHECK(!strcmp(wlan_hal_survey_status(),"RADIO RESTORE FAILED"));
    CHECK(!rejected_cmds);
    puts("PASS: actual survey handoff suspends STA, restores state on stop/failure, preserves other owners, and never reconnects during survey");
    return 0;
}
''')


# Compile the entire real FAP radio backend, substituting only SDK/OS boundaries.
radio_source=(ROOT/'applications_user/wardriver/wardriver_wifi.c').read_text(encoding='utf-8')
radio_source='\n'.join(line for line in radio_source.splitlines() if not line.startswith('#include'))
run('combined_radio_test',
    (ROOT/'tests/host/wardriver_radio_backend.h').read_text()+radio_source+
    (ROOT/'tests/host/wardriver_radio_test.c').read_text(),
    [ROOT/'applications_user/wardriver/wardriver_detect.c', ROOT/'applications_user/wardriver/wardriver_monitor.c'])
