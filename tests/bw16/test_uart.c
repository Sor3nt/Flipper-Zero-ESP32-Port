#include "bw16_uart.h"
#include "wardriver_gps.h"
#include <driver/gpio.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static bool owned[3], awake, compatible=true;
bool furi_hal_external_uart_available(void) { return compatible; }
static int fail_stage, installs, deletes, routes, parks, flushes, sends, waits, gpio_calls;
static int tx, rx, last_baud;
void furi_hal_power_insomnia_enter(void) { assert(!awake); awake=true; }
void furi_hal_power_insomnia_exit(void) { assert(awake); awake=false; }
bool uart_is_driver_installed(int n) { assert(n==1 || n==2); return owned[n]; }
esp_err_t uart_driver_install(int n,int rb,int tb,int count,QueueHandle_t* q,int flags) {
    assert(n==2 && !owned[n] && !tb && !flags);
    assert((rb==8192 && count==64 && q) || (rb==2048 && !count && !q));
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
}
int main(void) {
    Bw16Uart io={0};
    compatible=false;assert(!bw16_uart_open(&io) && !installs && !gpio_calls);compatible=true;
    for(int port=1;port<=2;++port) {
        owned[port]=true;int before=gpio_calls;
        assert(!bw16_uart_open(&io));assert(!io.installed && !io.awake && !installs);
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
    for(int failure=7;failure<=8;++failure) {
        memset(&io,0,sizeof(io));assert(bw16_uart_open(&io));fail_stage=failure;
        assert(bw16_uart_send(&io,"SCAN\n",5)!=ESP_OK && io.tx_failed);
        int before=sends;assert(bw16_uart_send(&io,"STOP\n",5)!=ESP_OK && sends==before);clean(&io);
    }
    WardriverConfig config={.gps_mode=1,.gps_uart=2,.gps_baud=9600,
        .gps_rx=44,.gps_tx=43,.isolated_uart_pins=true};
    for(unsigned i=0;i<100;++i) {
        memset(&io,0,sizeof(io));assert(bw16_uart_open(&io) && last_baud==115200);
        int routed=routes,flushed=flushes;
        assert(bw16_uart_send(&io,NULL,0)==ESP_ERR_INVALID_ARG);
        assert(bw16_uart_send(&io,"SCAN\nSTOP\n",10)==ESP_ERR_INVALID_ARG);
        assert(bw16_uart_send(&io,"SCAN\n",5)==ESP_OK);
        assert(bw16_uart_send(&io,"STOP\n",5)==ESP_OK);
        assert(routes==routed && flushes==flushed);
        WardriverGps gps;wardriver_gps_start(&gps,&config);
        assert(!gps.installed && !strcmp(gps.status,"UART BUSY"));
        clean(&io);
        wardriver_gps_start(&gps,&config);assert(gps.installed && last_baud==9600);
        int before=gpio_calls;assert(!bw16_uart_open(&io));
        assert(bw16_uart_close(&io)==ESP_OK && gpio_calls==before && gps.installed);
        wardriver_gps_poll(&gps,0);wardriver_gps_stop(&gps);assert(!owned[2]);
    }
    puts("UART: busy-owner isolation, fixed TX43/RX44, no per-command flush/reroute, failure rollback PASS");
    puts("UART: 100 BW16/GPS swaps through both production drivers; no driver/power-lock leak PASS");
}
