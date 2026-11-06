"""Protect configured UART GPS pins, validation, ownership and error cleanup."""
from pathlib import Path
from host_build import run
root=Path(__file__).resolve().parents[2]
app=root/'applications_user/wardriver'
out=root/'build_t_embed/host_tests'; out.mkdir(parents=True,exist_ok=True)
# The lease/rollback logic lives in a header that the GPS unit includes.
source=(app/'wardriver_gps_uart.h').read_text(encoding='utf-8')+'\n'+(app/'wardriver_gps.c').read_text(encoding='utf-8')
source='\n'.join(s for s in source.splitlines() if not s.startswith('#include') and s!='#pragma once')
prefix=r'''
#include "wardriver_gps.h"
#include <furi_hal_uart_shared.h>
#include <furi_hal_shared_pins.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#define ESP_OK 0
#define UART_NUM_1 1
#define UART_NUM_2 2
typedef int esp_err_t;
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
/* Firmware arbiters, ordered like the other UART users (USB-UART, BW16). */
static bool uart_held, pins_held, pins_saved, lease_busy;
static int leases, pin_leases, saves, restores;
static void furi_delay_ms(uint32_t ms) { (void)ms; }
FuriHalUartSharedAcquireResult furi_hal_uart_shared_acquire(uint32_t t) { assert(t==1000 && !uart_held); if(lease_busy) return FuriHalUartSharedAcquireBusy; ++leases; uart_held=true; return FuriHalUartSharedAcquireAcquired; }
bool furi_hal_uart_shared_release(void) { assert(uart_held && !pins_saved); uart_held=false; return true; }
bool furi_hal_shared_pins_acquire(const void* o) { assert(o && uart_held && !pins_held); ++pin_leases; pins_held=true; return true; }
void furi_hal_shared_pins_release(const void* o) { assert(o && pins_held && !pins_saved); pins_held=false; }
bool furi_hal_shared_pins_save(const void* o) { assert(o && pins_held && !pins_saved); ++saves; pins_saved=true; return true; }
void furi_hal_shared_pins_restore(const void* o) { assert(o && pins_held && pins_saved); ++restores; pins_saved=false; }
bool furi_hal_shared_pins_is_saved(const void* o) { (void)o; return pins_saved; }
/* Counted light-sleep veto: UART RX is lost while the clock is gated. */
static int sleep_depth, sleep_enters;
void furi_hal_power_light_sleep_inhibit_enter(void) { ++sleep_depth; ++sleep_enters; }
void furi_hal_power_light_sleep_inhibit_exit(void) { assert(sleep_depth>0); --sleep_depth; }
'''
suffix=r'''
int main(void) {
    WardriverGps gps;
    WardriverConfig c={.gps_uart=2,.gps_baud=9600,.gps_rx=44,.gps_tx=43,.isolated_uart_pins=true};
    wardriver_gps_start(&gps,&c); assert(!strcmp(gps.status,"OFF") && calls==0);
    for(unsigned mode=1;mode<=2;++mode) {
        c.gps_mode=mode; wardriver_gps_start(&gps,&c);
        assert(gps.installed && !strcmp(gps.status,"WAITING"));
        assert(sleep_depth==1); /* No light sleep while GPS is listening. */
        wardriver_gps_stop(&gps);
        assert(sleep_depth==0);
        int stopped=calls; wardriver_gps_stop(&gps); assert(calls==stopped && sleep_depth==0);
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
        assert(sleep_depth==0); /* A failed bring-up must not leave the veto behind. */
    }
    fail_stage=0; busy=true; int installed=installs, deleted=deletes, vetoes=sleep_enters;
    wardriver_gps_start(&gps,&c); wardriver_gps_stop(&gps);
    assert(!gps.installed && !strcmp(gps.status,"UART BUSY"));
    assert(installs==installed && deletes==deleted);
    assert(sleep_enters==vetoes && sleep_depth==0); /* A refusal never touches the sleep gate. */
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
    /* Every lease taken was returned: held=false, snapshot restored, none leaked. */
    assert(!uart_held && !pins_held && !pins_saved && saves==restores && leases==pin_leases && saves<=pin_leases);
    /* A refused UART lease must not touch the driver, the pins or the pin lease. */
    lease_busy=true; int refused=calls, taken=pin_leases; vetoes=sleep_enters;
    wardriver_gps_start(&gps,&c); wardriver_gps_stop(&gps);
    assert(!gps.installed && !strcmp(gps.status,"UART BUSY") && calls==refused && pin_leases==taken);
    assert(sleep_enters==vetoes && sleep_depth==0);
    lease_busy=false; assert(!uart_held && !pins_held && sleep_depth==0);
    puts("PASS: configured GPS 44/43, AUTO/UART, unsafe pins, busy driver, lease order and failure cleanup");
}
'''
c=out/'gps_pins_test.c';c.write_text(prefix+source+suffix,encoding='utf-8')
binary=out/'gps_pins_test'
run(['gcc','-std=c17','-Wall','-Wextra','-Werror','-O1','-I'+str(app),'-I'+str(root/'components/furi_hal'),str(c),str(app/'wardriver_nmea.c'),'-lm','-o',str(binary)],check=True)
run([str(binary)],check=True)
