#include "bw16_uart.h"
#include <driver/gpio.h>
#include <furi_hal_shared_pins.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static bool busy,owned,saved;
static bool awake;
void furi_hal_power_insomnia_enter(void) { assert(!awake); awake=true; }
void furi_hal_power_insomnia_exit(void) { assert(awake); awake=false; }
static int failure,installs,deletes,configs,pins,restores,releases,writes,waits;
static int op(int n) { return failure==n ? ESP_FAIL : ESP_OK; }
bool furi_hal_shared_pins_acquire(const void* p) { assert(p); return !busy; }
void furi_hal_shared_pins_release(const void* p) { assert(p && !saved); ++releases; }
bool furi_hal_shared_pins_save(const void* p) { assert(p); saved=failure!=1; return saved; }
void furi_hal_shared_pins_restore(const void* p) { assert(p && saved); saved=false; ++restores; }
bool uart_is_driver_installed(int n) { assert(n==1); return owned; }
esp_err_t uart_driver_install(int n,int rx,int tx,int count,QueueHandle_t* q,int flags) {
    assert(n==1 && rx==4096 && tx==0 && count==64 && flags==0 && !owned);
    ++installs; if(op(2)) return ESP_FAIL; owned=true; *q=(void*)1; return ESP_OK;
}
esp_err_t uart_driver_delete(int n) {
    assert(n==1 && owned); ++deletes; if(op(6)) return ESP_FAIL; owned=false; return ESP_OK;
}
esp_err_t uart_param_config(int n,const uart_config_t* cfg) {
    assert(n==1 && owned && cfg->baud_rate==115200 && cfg->data_bits==8 && cfg->stop_bits==1);
    ++configs; return op(3);
}
esp_err_t uart_set_pin(int n,int tx,int rx,int rts,int cts) {
    assert(owned && n==1 && tx==44 && rx==43 && rts==-1 && cts==-1); ++pins; return op(4);
}
esp_err_t uart_flush_input(int n) { assert(n==1 && owned); return op(5); }
esp_err_t gpio_config(const gpio_config_t* cfg) {
    assert(owned && cfg->pin_bit_mask==((1ULL<<43)|(1ULL<<44)) && cfg->mode==GPIO_MODE_INPUT);
    return op(9);
}
int uart_write_bytes(int n,const void* p,size_t len) {
    assert(n==1 && owned && len==5 && memcmp(p,"SCAN\n",5)==0); ++writes;
    return failure==7 ? 2 : 5;
}
esp_err_t uart_wait_tx_done(int n,unsigned timeout) { assert(n==1 && timeout==250); ++waits; return op(8); }
static void reset(void) {
    busy=owned=saved=awake=false;
    failure=installs=deletes=configs=pins=restores=releases=writes=waits=0;
}
esp_err_t furi_hal_bw16_guard_open(FuriHalBw16Guard* g,const void* p) {
    assert(p && saved); g->owner=p; return op(10);
}
esp_err_t furi_hal_bw16_guard_ready(FuriHalBw16Guard* g) {
    assert(g->owner && owned); g->ready=true; return op(11);
}
esp_err_t furi_hal_bw16_guard_scan(FuriHalBw16Guard* g) {
    assert(g->owner && g->ready); ++writes; ++waits;
    return failure==7 || failure==8 ? ESP_FAIL : ESP_OK;
}
esp_err_t furi_hal_bw16_guard_close(FuriHalBw16Guard* g) {
    assert(g->owner && saved); memset(g,0,sizeof(*g)); return ESP_OK;
}
int main(void) {
    Bw16Uart io={0}; reset(); busy=true;
    assert(!bw16_uart_open(&io) && !installs && !deletes && !releases);
    reset(); owned=true; memset(&io,0,sizeof(io));
    assert(!bw16_uart_open(&io) && !installs && !configs && !deletes && releases==1 && owned);
    for(int step=1;step<=5;++step) {
        reset(); failure=step; memset(&io,0,sizeof(io));
        assert(!bw16_uart_open(&io));
        assert(!io.leased && !io.installed && !io.saved && !saved && !owned && !awake);
        assert(releases==1 && deletes==(step>=3) && restores==(step>=2));
    }
    reset(); failure=9; memset(&io,0,sizeof(io));
    assert(!bw16_uart_open(&io) && deletes==1 && restores==1 && releases==1 && !pins);
    reset(); memset(&io,0,sizeof(io)); assert(bw16_uart_open(&io));
    assert(bw16_uart_scan(&io) && writes==1 && waits==1);
    failure=7; assert(!bw16_uart_scan(&io) && waits==2);
    failure=8; assert(!bw16_uart_scan(&io) && waits==3);
    failure=6; assert(bw16_uart_close(&io)==ESP_FAIL && io.leased && io.installed && io.saved);
    failure=0; assert(bw16_uart_close(&io)==ESP_OK && !owned && !saved);
    int n=deletes; assert(bw16_uart_close(&io)==ESP_OK && deletes==n && releases==1);
    for(int step=10;step<=11;++step) {
        reset(); failure=step; memset(&io,0,sizeof(io));
        assert(!bw16_uart_open(&io) && !io.guard.owner && !io.leased && !awake);
    }
    puts("UART: busy owner, every setup rollback, short TX, timeout, cleanup retry PASS");
}
