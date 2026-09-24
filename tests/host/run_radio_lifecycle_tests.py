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
typedef int wifi_mode_t;
enum { WIFI_MODE_NULL, WIFI_MODE_STA, WIFI_MODE_AP, WIFI_IF_STA };
typedef struct { char credentials[64]; } wifi_config_t;
typedef int Bt;
#define RECORD_BT "bt"
typedef void (*WlanHalWorkerFn)(void*);
static bool s_started,s_survey_active,s_survey_bt_restore,s_bt_was_on;
static bool s_wifi_connected,s_wifi_auto_reconnect,s_auth_fail_latched;
static const char* s_survey_status="READY";
static struct {
    bool started,suspended,reconnect,bt_restore,auth_failed,valid;
    wifi_config_t config; esp_err_t error;
} s_survey_saved;
static bool locked,worker_ok=true,other_app;
static wifi_mode_t mode=WIFI_MODE_STA;
static wifi_config_t driver_config;
static unsigned wifi_starts,wifi_stops,reconnects,bt_starts,bt_stops,fail_start_number;
static bool radio_lock(void) { if(locked) return false; locked=true; return true; }
static void radio_unlock(void) { CHECK(locked); locked=false; }
static bool wlan_hal_beacon_spam_is_running(void) { return other_app; }
static bool wlan_hal_evil_portal_is_running(void) { return false; }
static bool wlan_ensure_worker(void) { return worker_ok; }
static bool wlan_start_owned(bool survey) {
    CHECK(survey); ++wifi_starts; if(wifi_starts==fail_start_number) return false;
    s_started=true; s_wifi_auto_reconnect=false; return true;
}
static void wlan_stop_owned(bool survey) {
    CHECK(survey); if(s_started) ++wifi_stops;
    s_started=false; s_wifi_connected=false; s_wifi_auto_reconnect=false;
}
static bool wlan_hal_survey_run(WlanHalWorkerFn fn,void* arg) { CHECK(s_survey_active); fn(arg); return true; }
static Bt* furi_record_open(const char* record) { CHECK(!strcmp(record,RECORD_BT)); static Bt b; return &b; }
static void furi_record_close(const char* record) { CHECK(!strcmp(record,RECORD_BT)); }
static void bt_stop_stack(Bt* b) { (void)b; ++bt_stops; controller=0; host=0; }
static void bt_start_stack(Bt* b) { (void)b; ++bt_starts; controller=2; host=2; }
static esp_err_t esp_wifi_get_mode(wifi_mode_t* p) { *p=mode; return ESP_OK; }
static esp_err_t esp_wifi_get_config(int i,wifi_config_t* p) { (void)i; *p=driver_config; return ESP_OK; }
static esp_err_t esp_wifi_set_config(int i,const wifi_config_t* p) { (void)i; driver_config=*p; return ESP_OK; }
static esp_err_t esp_wifi_connect(void) { ++reconnects; s_wifi_connected=true; return ESP_OK; }
'''
wifi_source=(ROOT/'components/wifi/wlan_hal.c').read_text(encoding='utf-8')
functions=['wlan_hal_survey_status','survey_status','survey_active','survey_save_station',
    'survey_restore_station','survey_restore_owned','wlan_hal_survey_begin','wlan_hal_survey_end']
run('wifi_handoff_test',common+wifi+'\n'.join(function(wifi_source,n) for n in functions)+r'''
static void reset(void) {
    s_started=s_survey_active=s_survey_bt_restore=s_bt_was_on=false;
    s_wifi_connected=s_wifi_auto_reconnect=s_auth_fail_latched=false;
    controller=host=0; locked=false; worker_ok=true; other_app=false; mode=WIFI_MODE_STA;
    wifi_starts=wifi_stops=reconnects=bt_starts=bt_stops=fail_start_number=0;
    memset(&s_survey_saved,0,sizeof(s_survey_saved));
    strcpy(driver_config.credentials,"test prior station configuration");
}
int main(void) {
    reset(); s_started=s_wifi_connected=s_wifi_auto_reconnect=true;
    CHECK(wlan_hal_survey_begin(true));
    CHECK(s_started && !s_wifi_connected && !s_wifi_auto_reconnect && wifi_stops==1 && !reconnects);
    CHECK(!wlan_hal_survey_begin(true)); /* A second owner must not overwrite saved state. */
    wlan_hal_survey_end();
    CHECK(s_started && s_wifi_auto_reconnect && reconnects==1 && !s_survey_active && !locked);
    CHECK(!strcmp(driver_config.credentials,"test prior station configuration"));
    CHECK(!s_survey_saved.config.credentials[0]); /* Credentials are cleared after restoration. */
    wlan_hal_survey_end(); CHECK(reconnects==1);
    reset(); controller=host=2;
    CHECK(wlan_hal_survey_begin(true) && !controller && !host);
    wlan_hal_survey_end(); CHECK(!s_started && controller==2 && host==2 && bt_starts==1);
    reset(); s_started=s_wifi_connected=true;
    CHECK(wlan_hal_survey_begin(false) && !s_started && !reconnects);
    wlan_hal_survey_end(); CHECK(s_started && reconnects==1);
    reset(); s_started=true; mode=WIFI_MODE_AP;
    CHECK(!wlan_hal_survey_begin(true) && s_started && !wifi_stops && !bt_stops && !s_survey_active);
    reset(); s_started=s_wifi_connected=true; fail_start_number=1;
    CHECK(!wlan_hal_survey_begin(true));
    CHECK(s_started && reconnects==1 && !s_survey_active && !locked);
    reset(); controller=host=2; worker_ok=false;
    CHECK(!wlan_hal_survey_begin(true) && controller==2 && host==2 && !s_survey_active);
    reset(); other_app=true;
    CHECK(!wlan_hal_survey_begin(true) && !wifi_starts && !bt_stops);
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
    [ROOT/'applications_user/wardriver/wardriver_detect.c'])
