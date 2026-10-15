#pragma once
#include <stdint.h>
typedef int rtw_result_t;
#define RTW_SUCCESS 0
#define RTW_TRUE 1
#define RTW_MODE_STA 1
#define RTW_PROMISC_ENABLE_2 3
#define RTW_PROMISC_DISABLE 0
struct rtw_scan_result_t {
    struct { unsigned len; char val[33]; } SSID;
    struct { uint8_t octet[6]; } BSSID;
    uint8_t channel;
    int signal_strength;
};
struct rtw_scan_handler_result_t { int scan_complete; rtw_scan_result_t ap_details; };
int wifi_on(int mode);
int wifi_scan_networks(rtw_result_t (*callback)(rtw_scan_handler_result_t*),void*);
int wifi_set_channel(uint8_t channel);
int wifi_set_promisc(int mode,void (*callback)(unsigned char*,unsigned int,void*),int);
