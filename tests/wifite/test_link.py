#!/usr/bin/env python3
"""Compile the actual link.c against fault-injected public SDK mocks; no RF."""
from pathlib import Path
import os, subprocess, tempfile
root = Path(__file__).resolve().parents[2]
headers = r'''
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 1
#define ESP_ERR_NO_MEM 2
#define ESP_ERR_INVALID_ARG 3
#define ESP_ERR_WIFI_NOT_CONNECT 4
#define ESP_IDF_VERSION_VAL(a,b,c) ((a)*10000+(b)*100+(c))
#define ESP_IDF_VERSION ESP_IDF_VERSION_VAL(5,4,1)
#define WIFI_IF_STA 0
#define WPS_TYPE_PIN 2
typedef struct {int wps_type;} esp_wps_config_t;
#define WPS_CONFIG_INIT_DEFAULT(t) {.wps_type=(t)}
int esp_wifi_set_promiscuous(bool);
int esp_wifi_get_promiscuous(bool*);
int esp_wifi_wps_enable(const esp_wps_config_t*);
int esp_wifi_wps_disable(void);
int esp_wifi_wps_start(int);
int esp_wifi_disconnect(void);
int esp_wifi_scan_stop(void);
int esp_wifi_internal_tx(int,void*,size_t);
'''
harness = r'''
#include "mock.h"
#include "link.h"
#include "sdk_adapter.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
static bool sniff=true, enabled, prepared;
static int enables, disables, starts, clears, fail_sniff, fail_enable, fail_prepare, fail_start;
static const uint8_t bssid[6]={2,1,2,3,4,5};
int esp_wifi_set_promiscuous(bool on){if(fail_sniff)return ESP_FAIL;sniff=on;return 0;}
int esp_wifi_get_promiscuous(bool *on){*on=sniff;return 0;}
int esp_wifi_disconnect(void){return 0;}
int esp_wifi_scan_stop(void){assert(enabled);return 0;}
int esp_wifi_wps_enable(const esp_wps_config_t*c){
 assert(!sniff&&!enabled&&c->wps_type==2);enables++;
 if(fail_enable)return ESP_FAIL;
 enabled=true;return 0;
}
int esp_wifi_wps_disable(void){assert(enabled);enabled=false;disables++;return 0;}
int wf_sdk_prepare(const uint8_t b[6],uint8_t ch,void(*r)(void),void(*rx)(const uint8_t*,const uint8_t*,size_t)){
 assert(enabled&&!prepared&&!memcmp(b,bssid,6)&&ch==7&&r&&rx);
 if(fail_prepare)return ESP_FAIL;
 prepared=true;return 0;
}
void wf_sdk_clear(void){assert(!enabled);prepared=false;clears++;}
int esp_wifi_wps_start(int n){assert(enabled&&prepared&&n==0);starts++;return fail_start?ESP_FAIL:0;}
int esp_wifi_internal_tx(int ifx,void*p,size_t n){assert(enabled&&prepared&&ifx==0&&p&&n>=18);return 0;}
static bool accept(const uint8_t*b){return !memcmp(b,bssid,6);}
static void ready(void){}
static void receive(const uint8_t*s,const uint8_t*p,size_t n){(void)s;(void)p;(void)n;}
int main(void){
 WfTarget t={.ssid="Lab",.ssid_len=3,.channel=7};memcpy(t.bssid,bssid,6);
 WfLinkCallbacks cb={accept,ready,receive};uint8_t p[18]={0};
 assert(wf_link_connect(&t)==ESP_ERR_INVALID_STATE);
 assert(wf_link_install(&cb)==0);
 assert(wf_link_install(&cb)==ESP_ERR_INVALID_STATE);
 for(int i=0;i<3;i++){
  sniff=true;assert(wf_link_connect(&t)==0);
  assert(wf_link_connect(&t)==ESP_ERR_INVALID_STATE);
  assert(wf_link_send(p,sizeof(p))==0);wf_link_disconnect();
  assert(!enabled&&!prepared);wf_link_disconnect();
 }
 assert(enables==3&&disables==3&&starts==3&&clears==3);
 fail_sniff=1;assert(wf_link_connect(&t)==ESP_FAIL&&enables==3);fail_sniff=0;
 fail_enable=1;assert(wf_link_connect(&t)==ESP_FAIL&&!enabled);fail_enable=0;
 fail_prepare=1;assert(wf_link_connect(&t)==ESP_FAIL&&!enabled&&!prepared);fail_prepare=0;
 fail_start=1;assert(wf_link_connect(&t)==ESP_FAIL&&!enabled&&!prepared);fail_start=0;
 assert(disables==5&&clears==5);
 assert(wf_link_send(p,sizeof(p))==ESP_ERR_INVALID_ARG);
 assert(wf_link_connect(&t)==0);wf_link_uninstall();assert(!enabled&&!prepared);
 assert(wf_link_install(&cb)==0);assert(wf_link_connect(&t)==0);wf_link_uninstall();
 puts("PASS: SDK init/start/disable lifecycle, repeat sessions, fault cleanup and TX guards");
}
'''
with tempfile.TemporaryDirectory() as td:
 td=Path(td)
 (td/'mock.h').write_text(headers)
 for n in ('esp_err.h','esp_idf_version.h','esp_private/wifi.h','esp_wifi.h','esp_wps.h'):
  p=td/n;p.parent.mkdir(parents=True,exist_ok=True);p.write_text('#include "mock.h"\n')
 (td/'test.c').write_text(harness)
 exe=td/'test'
 subprocess.run(['gcc','-Wall','-Wextra','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(td),'-I'+str(root/'components/wifi/wifite'),str(td/'test.c'),str(root/'components/wifi/wifite/link.c'),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
