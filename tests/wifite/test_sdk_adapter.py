#!/usr/bin/env python3
"""Exercise generated SDK scan/association functions with a mock driver; no RF."""
from pathlib import Path
import importlib.util, os, re, subprocess, tempfile
root=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('adapter',root/'tools/wifite_sdk_adapter.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
src=Path(os.environ['IDF_PATH'])/'components/wpa_supplicant/esp_supplicant/src/esp_wps.c'
s=m.adapt(src.read_bytes())
try:
 m.adapt(src.read_bytes()+b'\n')
 raise AssertionError('Modified SDK was accepted')
except ValueError:pass

def function(name):
 # These functions have no braces in string literals.
 match=re.search(r'\n(?:static )?(?:void|bool|int)\s*\n?'+name+r'\([^;]*?\)\n\{',s)
 assert match,name
 a=match.start();begin=s.index('{',match.start());depth=1;i=begin+1
 while depth:
  depth+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[a:i]

stub=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "sdk_adapter.h"
#define os_memcpy memcpy
#define os_memcmp memcmp
#define os_memset memset
#define ESP_OK 0
#define ESP_ERR_NO_MEM 2
#define ESP_ERR_INVALID_STATE 1
#define WPS_TYPE_DISABLE 0
#define WPS_TYPE_PIN 2
#define WPS_STATUS_DISABLE 0
#define WPS_STATUS_SCANNING 1
#define WPS_STATUS_PENDING 2
#define WIFI_IF_STA 0
#define WIFI_SCAN_TYPE_ACTIVE 0
#define WIFI_APPIE_WPS_PR 0
#define WIFI_APPIE_WPS_AR 1
#define WLAN_CAPABILITY_PRIVACY 16
#define WPS_REQ_REGISTRAR 2
#define DEV_PW_DEFAULT 0
#define WAIT_START 0
#define WPS_IGNORE_SEL_REG_MAX_CNT 10
#define ETH_ALEN 6
#define OS_BLOCK 0
#define WIFI_EVENT 0
#define WIFI_EVENT_STA_WPS_ER_PBC_OVERLAP 0
#define wpa_printf(...) ((void)0)
typedef int ETS_STATUS;
typedef struct {struct {uint8_t bssid[6],ssid[32],channel;int bssid_set,failure_retry_cnt;} sta;} wifi_config_t;
typedef struct {uint8_t*bssid;uint8_t channel;bool show_hidden;int scan_type;struct{struct{int min,max;}active;}scan_time;} wifi_scan_config_t;
struct wps_scan_ie {uint8_t*bssid;uint8_t chan;uint16_t capinfo;uint8_t*ssid,*wpa,*rsn,*wps;};
struct wps_device_data {int unused;};
struct wps_sm {bool is_wps_scan,wps_pbc_overlap,intermediate_disconnect,ignore_sel_reg;uint8_t bssid[6],channel,uuid[16];int discover_ssid_cnt,scan_cnt,state;struct{uint8_t ssid[32];size_t ssid_len;}creds[1];struct wps_device_data*dev;};
static struct wps_sm sm,*gWpsSm=&sm;
static bool s_wps_enabled=true,wf_sdk_enabled;
static uint8_t wf_sdk_bssid[6],wf_sdk_channel;
static uint32_t wf_sdk_scans,wf_sdk_seen,wf_sdk_connecting,wf_sdk_ready;
static int wf_sdk_error;
static void(*wf_sdk_on_ready)(void);
static void(*wf_sdk_on_receive)(const uint8_t*,const uint8_t*,size_t);
static bool wf_sdk_active(void){return wf_sdk_enabled;}
static int status,type=2,config_result,connect_result,scan_result,configs,connects,scans,timers,privacy;
static wifi_config_t applied;
static wifi_scan_config_t scanned;
static int wps_get_status(void){return status;}
static void wps_set_status(int x){status=x;}
static int wps_get_type(void){return type;}
static bool esp_wifi_enable_sta_privacy_internal(void){privacy++;return true;}
static int esp_wifi_disconnect(void){return 0;}
static int esp_wifi_set_config(int i,const wifi_config_t*c){assert(i==0);applied=*c;configs++;return config_result;}
static int esp_wifi_connect(void){connects++;return connect_result;}
static void esp_event_post(int a,int b,int c,int d,int e){(void)a;(void)b;(void)c;(void)d;(void)e;}
static void wifi_wps_scan(void*a,void*b){(void)a;(void)b;}
static void wifi_station_wps_msg_timeout(void*a,void*b){(void)a;(void)b;}
static void eloop_cancel_timeout(void(*fn)(void*,void*),void*a,void*b){(void)fn;(void)a;(void)b;}
static void eloop_register_timeout(int a,int b,void(*fn)(void*,void*),void*c,void*d){(void)a;(void)b;(void)fn;(void)c;(void)d;timers++;}
int esp_wifi_promiscuous_scan_start(wifi_scan_config_t*c,void(*fn)(void*,ETS_STATUS)){(void)fn;scans++;if(c)scanned=*c;return scan_result;}
struct wpabuf{int unused;};
static struct wpabuf* wps_build_probe_req_ie(int a,struct wps_device_data*b,const uint8_t*c,int d,int e,const uint8_t*f){(void)a;(void)b;(void)c;(void)e;(void)f;assert(d==2);return calloc(1,sizeof(struct wpabuf));}
static struct wpabuf* wps_build_assoc_req_ie(int a){assert(a==2);return calloc(1,sizeof(struct wpabuf));}
static void* wpabuf_mhead(struct wpabuf*p){return p;}
static size_t wpabuf_len(struct wpabuf*p){(void)p;return 1;}
static void wpabuf_free(struct wpabuf*p){free(p);}
static int esp_wifi_set_appie_internal(int a,void*b,size_t n,int d){(void)a;(void)b;(void)n;(void)d;return 0;}
'''
test=r'''
static void ready(void){}
static void rx(const uint8_t*a,const uint8_t*b,size_t n){(void)a;(void)b;(void)n;}
int main(void){
 uint8_t target[]={2,1,2,3,4,5},other[]={2,9,8,7,6,5},ssid[]={0,3,'L','a','b'},ie=1;
 assert(wf_sdk_prepare(target,7,ready,rx)==0);
 struct wps_scan_ie ap={target,7,16,ssid,NULL,&ie,&ie};
 sm.is_wps_scan=true;status=WPS_STATUS_SCANNING;
 ap.bssid=other;assert(!wf_sdk_scan(&ap)&&!wf_sdk_seen);
 ap.bssid=target;ap.wps=NULL;assert(!wf_sdk_scan(&ap));ap.wps=&ie;
 ap.rsn=NULL;assert(!wf_sdk_scan(&ap));ap.rsn=&ie;
 assert(wf_sdk_scan(&ap)&&privacy==1&&sm.discover_ssid_cnt==1);
 assert(wf_sdk_scan(&ap)&&sm.discover_ssid_cnt==1);
 wifi_wps_scan_internal();
 assert(scans==1&&wf_sdk_scans==1&&scanned.channel==7&&scanned.bssid==wf_sdk_bssid);
 assert(scanned.scan_time.active.max==300);
 wifi_wps_scan_done(NULL,0);
 assert(status==WPS_STATUS_PENDING&&configs==1&&connects==1&&wf_sdk_connecting==1);
 assert(applied.sta.failure_retry_cnt==2&&applied.sta.channel==7&&applied.sta.bssid_set);
 assert(!memcmp(applied.sta.bssid,target,6)&&!memcmp(applied.sta.ssid,"Lab",3));
 assert(timers==0); /* No enrollee two-second timeout while external worker owns EAP. */
 assert(!wf_sdk_scan(&ap)); /* A connection scan cannot become a new WPS discovery. */
 sm.discover_ssid_cnt=1;config_result=-99;wifi_wps_scan_done(NULL,0);
 assert(wf_sdk_error==-99&&connects==1);
 scan_result=-88;wifi_wps_scan_internal();assert(wf_sdk_error==-88);
 WfSdkStatus got;wf_sdk_status(&got);assert(got.scans==2&&got.seen==2&&got.error==-88);
 wf_sdk_clear();assert(!wf_sdk_active());
 config_result=0;sm.discover_ssid_cnt=1;wifi_wps_scan_done(NULL,0);
 assert(connects==2&&timers==1); /* Ordinary SDK enrollee path retains its timer. */
 puts("PASS: pinned SDK adapter, selected-router filtering, native scan/association, error propagation and enrollee isolation");
}
'''
with tempfile.TemporaryDirectory() as td:
 td=Path(td);(td/'esp_err.h').write_text('typedef int esp_err_t;\n')
 code=stub+function('wifi_wps_scan_done')+function('wifi_wps_scan_internal')+'\n#include "sdk_adapter.inc"\n'+test
 (td/'test.c').write_text(code)
 exe=td/'test'
 subprocess.run(['gcc','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(td),'-I'+str(root/'components/wifi/wifite'),str(td/'test.c'),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
