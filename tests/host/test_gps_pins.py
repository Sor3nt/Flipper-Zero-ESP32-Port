"""Protect configured UART GPS pins, validation, ownership and error cleanup."""
from pathlib import Path
from host_build import run
root=Path(__file__).resolve().parents[2]
app=root/'applications_user/wardriver'
out=root/'build_t_embed/host_tests'; out.mkdir(parents=True,exist_ok=True)
source=(app/'wardriver_gps.c').read_text(encoding='utf-8')
source='\n'.join(s for s in source.splitlines() if not s.startswith('#include'))
prefix=r'''
#include "wardriver_gps.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#define ESP_OK 0
#define UART_DATA_8_BITS 8
#define UART_PARITY_DISABLE 0
#define UART_STOP_BITS_1 1
#define UART_HW_FLOWCTRL_DISABLE 0
#define UART_SCLK_DEFAULT 0
#define GPIO_IS_VALID_GPIO(n) ((n)>=0 && (n)<=48)
#define GPIO_IS_VALID_OUTPUT_GPIO(n) GPIO_IS_VALID_GPIO(n)
typedef struct { int baud_rate,data_bits,parity,stop_bits,flow_ctrl,source_clk; } uart_config_t;
static int calls, installs, deletes, resets, fail_stage;
static bool busy;
static bool uart_is_driver_installed(int n) { assert(n==1 || n==2); ++calls; return busy; }
static int uart_driver_install(int n,int a,int b,int c,void* d,int e) { (void)n; assert(a==2048 && !b && !c && !d && !e); ++calls; ++installs; return fail_stage==1?-1:ESP_OK; }
static int uart_driver_delete(int n) { (void)n; ++calls; ++deletes; return ESP_OK; }
static int uart_param_config(int n,const uart_config_t* c) { (void)n; assert(c->baud_rate==9600); ++calls; return fail_stage==2?-1:ESP_OK; }
static int uart_set_pin(int n,int a,int b,int c,int d) { (void)n; assert(a==43 && b==44 && c==-1 && d==-1); ++calls; return fail_stage==3?-1:ESP_OK; }
static int uart_read_bytes(int n,void* b,size_t s,int t) { (void)n;(void)b;(void)s;(void)t;abort(); }
static void gpio_reset_pin(int n) { assert(n==43 || n==44); ++calls; ++resets; }
'''
suffix=r'''
int main(void) {
    WardriverGps gps;
    WardriverConfig c={.gps_uart=2,.gps_baud=9600,.gps_rx=44,.gps_tx=43,.isolated_uart_pins=true};
    wardriver_gps_start(&gps,&c); assert(!strcmp(gps.status,"OFF") && calls==0);
    for(unsigned mode=1;mode<=2;++mode) {
        c.gps_mode=mode; wardriver_gps_start(&gps,&c);
        assert(gps.installed && !strcmp(gps.status,"WAITING"));
        wardriver_gps_stop(&gps);
        int stopped=calls; wardriver_gps_stop(&gps); assert(calls==stopped);
    }
    assert(installs==2 && deletes==2 && resets==4);
    int blocked[]={-1,19,20,26,32,37,49};
    for(unsigned i=0;i<sizeof(blocked)/sizeof(blocked[0]);++i) {
        int before=calls;
        c.gps_rx=blocked[i];c.gps_tx=-1;wardriver_gps_start(&gps,&c);
        assert(!gps.installed && !strcmp(gps.status,"CHECK PINS") && calls==before);
    }
    c.gps_rx=44;c.gps_tx=43;
    for(int stage=1;stage<=3;++stage) {
        fail_stage=stage; int deleted=deletes, reset=resets;
        wardriver_gps_start(&gps,&c);
        assert(!gps.installed && !strcmp(gps.status,"UART ERROR"));
        assert(deletes==deleted+(stage>1) && resets==reset+2*(stage>1));
    }
    fail_stage=0; busy=true; int installed=installs, deleted=deletes;
    wardriver_gps_start(&gps,&c); wardriver_gps_stop(&gps);
    assert(!gps.installed && !strcmp(gps.status,"UART BUSY"));
    assert(installs==installed && deletes==deleted);
    busy=false;
    for(int invalid=0;invalid<4;++invalid) {
        WardriverConfig bad=c;
        if(invalid==0) bad.isolated_uart_pins=false;
        if(invalid==1) bad.gps_uart=0;
        if(invalid==2) bad.gps_uart=3;
        if(invalid==3) bad.gps_tx=bad.gps_rx;
        int before=calls; wardriver_gps_start(&gps,&bad);
        assert(!gps.installed && !strcmp(gps.status,"CHECK PINS") && calls==before);
    }
    puts("PASS: configured GPS 44/43, AUTO/UART, unsafe pins, busy driver and failure cleanup");
}
'''
c=out/'gps_pins_test.c';c.write_text(prefix+source+suffix,encoding='utf-8')
binary=out/'gps_pins_test'
run(['gcc','-std=c17','-Wall','-Wextra','-Werror','-O1','-I'+str(app),str(c),str(app/'wardriver_nmea.c'),'-lm','-o',str(binary)],check=True)
run([str(binary)],check=True)
