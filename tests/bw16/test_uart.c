#include "bw16_uart.h"
#include "wardriver_gps.h"
#include <furi_hal_uart_shared.h>
#include <furi_hal_shared_pins.h>
#include <driver/gpio.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static bool owned[3], awake, compatible=true;
bool furi_hal_external_uart_available(void) { return compatible; }
static int fail_stage, installs, deletes, routes, parks, flushes, sends, waits, gpio_calls;
static int tx, rx, last_baud;
/* Firmware arbiters: UART lease, GPIO43/44 lease and the pin snapshot. */
static bool uart_lease, uart_busy, uart_release_fail, pins_busy, save_fail, restore_fail, pins_saved;
static const void* pin_owner;
static int restores, delays, pins_busy_for;
void furi_delay_ms(uint32_t ms) { assert(ms==5);++delays; }
FuriHalUartSharedAcquireResult furi_hal_uart_shared_acquire(uint32_t timeout_ms) {
    /* Wardriver GPS uses the same 1000 ms lease timeout as BW16. */
    assert(timeout_ms==BW16_UART_LEASE_MS);
    /* The real lease is a binary semaphore: a second owner times out as Busy. */
    if(uart_busy || uart_lease) return FuriHalUartSharedAcquireBusy;
    uart_lease=true;return FuriHalUartSharedAcquireAcquired;
}
bool furi_hal_uart_shared_release(void) {
    assert(uart_lease && !pins_saved);if(uart_release_fail)return false;uart_lease=false;return true;
}
bool furi_hal_shared_pins_acquire(const void* owner) {
    assert(owner && uart_lease);if(pins_busy || pin_owner)return false;
    if(pins_busy_for>0) { --pins_busy_for;return false; } /* previous owner still releasing */
    pin_owner=owner;return true;
}
void furi_hal_shared_pins_release(const void* owner) { assert(owner==pin_owner && !pins_saved);pin_owner=NULL; }
bool furi_hal_shared_pins_save(const void* owner) {
    /* Snapshot only after the busy check and before BW16 touches the pins. */
    assert(owner==pin_owner && !owned[1] && !owned[2] && !pins_saved);
    if(save_fail) return false;
    pins_saved=true;
    return true;
}
void furi_hal_shared_pins_restore(const void* owner) {
    assert(owner==pin_owner && pins_saved && !owned[2]);++restores;if(!restore_fail)pins_saved=false;
}
bool furi_hal_shared_pins_is_saved(const void* owner) { return owner==pin_owner && pins_saved; }
void furi_hal_power_insomnia_enter(void) { assert(!awake); awake=true; }
void furi_hal_power_insomnia_exit(void) { assert(awake); awake=false; }
/* Counted light-sleep veto (UART RX is lost while the clock is gated). */
static int inhibit_depth;
void furi_hal_power_light_sleep_inhibit_enter(void) { ++inhibit_depth; }
void furi_hal_power_light_sleep_inhibit_exit(void) { assert(inhibit_depth>0); --inhibit_depth; }
bool uart_is_driver_installed(int n) { assert(n==1 || n==2); return owned[n]; }
esp_err_t uart_driver_install(int n,int rb,int tb,int count,QueueHandle_t* q,int flags) {
    assert(n==2 && !owned[n] && !tb && !flags);
    assert((rb==8192 && count==64 && q) || (rb==2048 && !count && !q));
    if(rb==8192) assert(uart_lease && pin_owner && pins_saved); /* BW16 holds every lease. */
    ++installs;if(fail_stage==1)return ESP_FAIL;owned[n]=true;if(q)*q=(void*)1;return ESP_OK;
}
esp_err_t uart_driver_delete(int n) {
    assert(n==2 && owned[n]);++deletes;if(fail_stage==5)return ESP_FAIL;owned[n]=false;return ESP_OK;
}
esp_err_t uart_param_config(int n,const uart_config_t* c) {
    assert(n==2 && owned[n] && c->data_bits==8 && c->stop_bits==1 && !c->flow_ctrl);
    last_baud=c->baud_rate;assert(last_baud==9600 || last_baud==115200);
    return fail_stage==2?ESP_FAIL:ESP_OK;
}
esp_err_t uart_set_pin(int n,int t,int r,int rts,int cts) {
    assert(n==2 && owned[n] && t==43 && r==44 && rts==-1 && cts==-1);
    ++routes;tx=t;rx=r;return fail_stage==3?ESP_FAIL:ESP_OK;
}
esp_err_t uart_flush_input(int n) { assert(n==2 && owned[n]);++flushes;return fail_stage==4?ESP_FAIL:ESP_OK; }
esp_err_t gpio_config(const gpio_config_t* c) {
    assert(c->pin_bit_mask==((1ULL<<43)|(1ULL<<44)) && c->mode==GPIO_MODE_INPUT);
    assert(c->pull_up_en==GPIO_PULLUP_ENABLE && c->pull_down_en==GPIO_PULLDOWN_DISABLE);
    ++parks;++gpio_calls;return fail_stage==6?ESP_FAIL:ESP_OK;
}
esp_err_t gpio_reset_pin(int n) { assert(n==43 || n==44);++gpio_calls;return ESP_OK; }
int uart_write_bytes(int n,const void* b,size_t size) {
    assert(n==2 && owned[n] && tx==43 && rx==44 && size<=48 && b);++sends;
    return fail_stage==7?(int)size-1:(int)size;
}
esp_err_t uart_wait_tx_done(int n,unsigned timeout) {
    assert(n==2 && timeout==250);++waits;return fail_stage==8?ESP_FAIL:ESP_OK;
}
int uart_read_bytes(int n,void* b,size_t size,unsigned timeout) {
    assert(n==2 && owned[n] && b && size && !timeout);return 0;
}
static void clean(Bw16Uart* io) {
    fail_stage=0;assert(bw16_uart_close(io)==ESP_OK);
    assert(!io->installed && !io->awake && !owned[2] && !awake);
    assert(!io->uart_leased && !io->pins_leased && !io->pins_saved);
    assert(!uart_lease && !pin_owner && !pins_saved);
}
static void released_untouched(Bw16Uart* io,int installs_before,int gpio_before) {
    assert(!io->installed && !io->awake && installs==installs_before && gpio_calls==gpio_before);
    assert(!uart_lease && !pin_owner && !pins_saved);
    assert(!io->uart_leased && !io->pins_leased && !io->pins_saved);
}
int main(void) {
    Bw16Uart io={0};
    compatible=false;assert(!bw16_uart_open(&io) && !installs && !gpio_calls && !uart_lease);compatible=true;
    /* Another UART owner (USB-UART), a pin owner (NRF/IR/RFID) or a
     * failed snapshot: refuse without installing or touching GPIO43/44. */
    uart_busy=true;assert(!bw16_uart_open(&io));released_untouched(&io,0,0);uart_busy=false;
    pins_busy=true;assert(!bw16_uart_open(&io));released_untouched(&io,0,0);pins_busy=false;
    save_fail=true;assert(!bw16_uart_open(&io));released_untouched(&io,0,0);save_fail=false;
    assert(!restores && bw16_uart_close(&io)==ESP_OK);
    for(int port=1;port<=2;++port) {
        owned[port]=true;int before=gpio_calls;
        assert(!bw16_uart_open(&io));assert(!io.installed && !io.awake && !installs);
        assert(!uart_lease && !pin_owner && !pins_saved && !restores);
        assert(bw16_uart_close(&io)==ESP_OK && gpio_calls==before && owned[port]);owned[port]=false;
    }
    for(int failure=1;failure<=6;++failure) {
        if(failure==5)continue;
        memset(&io,0,sizeof(io));fail_stage=failure;
        assert(!bw16_uart_open(&io));clean(&io);
    }
    for(int failure=5;failure<=6;++failure) {
        memset(&io,0,sizeof(io));assert(bw16_uart_open(&io));
        fail_stage=failure;assert(bw16_uart_close(&io)!=ESP_OK && io.awake);
        assert(io.installed==(failure==5));clean(&io);
    }
    /* Restore or UART release failure keeps the remaining leases for a retry,
     * and a new open is refused until cleanup has finished. */
    memset(&io,0,sizeof(io));assert(bw16_uart_open(&io) && uart_lease && pins_saved);
    restore_fail=true;assert(bw16_uart_close(&io)!=ESP_OK);
    assert(!io.installed && io.awake && io.pins_saved && io.uart_leased && io.pins_leased && awake);
    assert(uart_lease && pin_owner==&io && pins_saved);
    int installed_before=installs;assert(!bw16_uart_open(&io) && installs==installed_before);
    restore_fail=false;clean(&io);
    memset(&io,0,sizeof(io));assert(bw16_uart_open(&io));
    uart_release_fail=true;assert(bw16_uart_close(&io)!=ESP_OK);
    assert(!io.pins_saved && io.uart_leased && io.pins_leased && io.awake && pin_owner==&io);
    uart_release_fail=false;clean(&io);
    for(int failure=7;failure<=8;++failure) {
        memset(&io,0,sizeof(io));assert(bw16_uart_open(&io));fail_stage=failure;
        assert(bw16_uart_send(&io,"SCAN\n",5)!=ESP_OK && io.tx_failed);
        int before=sends;assert(bw16_uart_send(&io,"STOP\n",5)!=ESP_OK && sends==before);clean(&io);
    }
    WardriverConfig config={.gps_mode=1,.gps_uart=2,.gps_baud=9600,
        .gps_rx=44,.gps_tx=43,.isolated_uart_pins=true};
    /* GPS takes the same arbiters as every other UART user, in the same order, and
     * rolls all of them back. Nothing may be installed or touched when it is refused. */
    {
        WardriverGps gps;
        int installs_before,gpio_before,delays_before;
        #define GPS_IDLE() assert(!uart_lease && !pin_owner && !pins_saved && !owned[1] && !owned[2] && !inhibit_depth)
        /* Start while expecting a refusal that leaves no driver, GPIO call or lease behind. */
        #define GPS_REFUSED(text) do { \
            installs_before=installs;gpio_before=gpio_calls;delays_before=delays; \
            wardriver_gps_start(&gps,&config); \
            assert(!gps.installed && !strcmp(gps.status,text) && installs==installs_before); \
            assert(gpio_calls==gpio_before && !uart_lease && !pin_owner && !pins_saved && !inhibit_depth); \
        } while(0)
        /* Another lease holder (USB-UART, BW16). */
        uart_busy=true;GPS_REFUSED("UART BUSY");
        wardriver_gps_stop(&gps);uart_busy=false;GPS_IDLE();
        /* NRF/IR owner of the pin lease: bounded wait, UART lease handed back. */
        pins_busy=true;GPS_REFUSED("PIN IN USE");
        assert(delays-delays_before>0 && delays-delays_before<=40);
        wardriver_gps_stop(&gps);pins_busy=false;GPS_IDLE();
        /* The RFID reader releases the UART lease a moment before its pin lease. */
        pins_busy_for=3;delays_before=delays;wardriver_gps_start(&gps,&config);
        assert(gps.installed && delays-delays_before==3 && !pins_busy_for && inhibit_depth==1);
        wardriver_gps_stop(&gps);GPS_IDLE();
        /* Lease-less drivers on either port that routes to GPIO43/44. */
        for(int port=1;port<=2;++port) {
            owned[port]=true;GPS_REFUSED("UART BUSY");
            wardriver_gps_stop(&gps);assert(owned[port]);owned[port]=false;
        }
        GPS_IDLE();
        /* A board with no GPIO43/44 snapshot: GPS still runs, nothing is restored. */
        save_fail=true;
        {
            int restored=restores;
            wardriver_gps_start(&gps,&config);
            assert(gps.installed && owned[2] && uart_lease && pin_owner && !pins_saved && inhibit_depth==1);
            wardriver_gps_stop(&gps);assert(restores==restored);
        }
        save_fail=false;GPS_IDLE();
        /* Install, config and routing failures roll back every step. */
        for(int stage=1;stage<=3;++stage) {
            fail_stage=stage;wardriver_gps_start(&gps,&config);
            assert(!gps.installed && !strcmp(gps.status,"UART ERROR"));GPS_IDLE();
        }
        fail_stage=0;
        /* A release that fails keeps every lease whose state is still live, refuses a new
         * start with a distinct status, and is finished by the next stop or start. */
        for(int failure=0;failure<3;++failure) {
            wardriver_gps_start(&gps,&config);assert(gps.installed && owned[2] && pins_saved);
            if(failure==0) fail_stage=5;           /* driver delete fails */
            if(failure==1) restore_fail=true;      /* pin snapshot restore fails */
            if(failure==2) uart_release_fail=true; /* UART lease release fails */
            wardriver_gps_stop(&gps);
            /* The sleep veto is kept for as long as any lease or driver is. */
            assert(!gps.installed && uart_lease && pin_owner && inhibit_depth==1);
            assert(owned[2]==(failure==0) && pins_saved==(failure<2));
            installs_before=installs;wardriver_gps_start(&gps,&config);
            assert(!gps.installed && !strcmp(gps.status,"UART CLEANUP") && installs==installs_before);
            assert(uart_lease && pin_owner && inhibit_depth==1);
            fail_stage=0;restore_fail=false;uart_release_fail=false;
            if(failure==2) {
                /* Start finishes the pending release first, then installs normally. */
                wardriver_gps_start(&gps,&config);assert(gps.installed && owned[2]);
            }
            wardriver_gps_stop(&gps);
            GPS_IDLE();
        }
        #undef GPS_REFUSED
        #undef GPS_IDLE
    }
    for(unsigned i=0;i<100;++i) {
        memset(&io,0,sizeof(io));assert(bw16_uart_open(&io) && last_baud==115200);
        int routed=routes,flushed=flushes;
        assert(bw16_uart_send(&io,NULL,0)==ESP_ERR_INVALID_ARG);
        assert(bw16_uart_send(&io,"SCAN\nSTOP\n",10)==ESP_ERR_INVALID_ARG);
        assert(bw16_uart_send(&io,"SCAN\n",5)==ESP_OK);
        assert(bw16_uart_send(&io,"STOP\n",5)==ESP_OK);
        assert(routes==routed && flushes==flushed);
        assert(uart_lease && pin_owner==&io && pins_saved);
        WardriverGps gps;wardriver_gps_start(&gps,&config);
        assert(!gps.installed && !strcmp(gps.status,"UART BUSY"));
        clean(&io);
        wardriver_gps_start(&gps,&config);assert(gps.installed && last_baud==9600 && inhibit_depth==1);
        int before=gpio_calls;assert(!bw16_uart_open(&io));
        assert(bw16_uart_close(&io)==ESP_OK && gpio_calls==before && gps.installed);
        wardriver_gps_poll(&gps,0);wardriver_gps_stop(&gps);assert(!owned[2] && !inhibit_depth);
    }
    puts("UART: busy-owner isolation, fixed TX43/RX44, no per-command flush/reroute, failure rollback PASS");
    puts("UART: shared-UART + GPIO43/44 leases held while open, snapshot restored, retryable release PASS");
    puts("UART: 100 BW16/GPS swaps through both production drivers; no driver/power-lock leak PASS");
}
