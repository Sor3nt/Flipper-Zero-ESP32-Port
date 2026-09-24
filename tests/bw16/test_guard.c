#include <furi_hal_bw16_guard.h>
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include <hal/gpio_ll.h>
#include <soc/gpio_sig_map.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

TestGpio GPIO;
uintptr_t GPIO_PIN_MUX_REG[49];
static int token;
static bool leased=true,global_lock,bus_lock,device,uart_route;
static unsigned level[49],reg_config,width,transfers,fail_transfer;
static int mode,writes,waits;
bool furi_hal_shared_pins_is_saved(const void* owner) { return owner==&token && leased; }
void furi_hal_spi_bus_lock(void) { assert(!global_lock); global_lock=true; }
void furi_hal_spi_bus_unlock(void) { assert(global_lock && !bus_lock); global_lock=false; }
esp_err_t spi_bus_add_device(int host,const spi_device_interface_config_t* c,void** p) {
    assert(global_lock && !device && host==SPI2_HOST && c->spics_io_num==-1);
    if(mode==1) return ESP_FAIL;
    device=true; *p=&token; return ESP_OK;
}
esp_err_t spi_bus_remove_device(void* d) {
    assert(global_lock && !bus_lock && device && d==&token);
    if(mode==8) return ESP_FAIL;
    device=false; return ESP_OK;
}
esp_err_t spi_device_acquire_bus(void* d,unsigned wait) {
    assert(global_lock && device && !bus_lock && d==&token && wait==portMAX_DELAY);
    if(mode==2) return ESP_FAIL;
    bus_lock=true; return ESP_OK;
}
void spi_device_release_bus(void* d) {
    assert(d==&token && global_lock && bus_lock && level[44]==1);
    bus_lock=false;
}
void gpio_ll_set_level(TestGpio* p,unsigned pin,unsigned value) {
    assert(p==&GPIO && bus_lock); level[pin]=value;
}
void gpio_ll_iomux_func_sel(uintptr_t pin,unsigned function) { (void)pin;(void)function;assert(bus_lock); }
void gpio_ll_output_enable(TestGpio* p,unsigned pin) { (void)p;(void)pin;assert(bus_lock); }
void esp_rom_gpio_connect_out_signal(unsigned pin,unsigned signal,bool inv,bool eninv) {
    assert(bus_lock && signal==SIG_GPIO_OUT_IDX && !inv && !eninv);
    if(pin==44) uart_route=false;
}
esp_err_t gpio_set_level(unsigned pin,unsigned value) { assert(bus_lock);level[pin]=value;return ESP_OK; }
int gpio_get_level(unsigned pin) { return (int)level[pin]; }
esp_err_t gpio_config(const gpio_config_t* c) {
    assert(bus_lock && c->mode==GPIO_MODE_INPUT_OUTPUT);
    return mode==3?ESP_FAIL:ESP_OK;
}
void esp_rom_delay_us(unsigned us) { (void)us;assert(bus_lock); }
esp_err_t spi_device_polling_transmit(void* d,spi_transaction_t* t) {
    assert(d==&token && bus_lock && !level[44] && !level[43] && t->length==16);
    if(++transfers==fail_transfer) return ESP_FAIL;
    if(mode==4) { t->rx_data[0]=t->rx_data[1]=0xff;return ESP_OK; }
    t->rx_data[0]=0x0e;
    unsigned address=t->tx_data[0]&31;
    if(t->tx_data[0]&0x20) {
        assert(address==0 && !(t->tx_data[1]&2)); /* Never power up NRF. */
        if(mode!=5) reg_config=t->tx_data[1];
    } else t->rx_data[1]=(uint8_t)(address==3?width:reg_config);
    return ESP_OK;
}
int uart_write_bytes(int n,const void* bytes,size_t length) {
    assert(n==1 && length==5 && !memcmp(bytes,"SCAN\n",5));
    assert(global_lock && bus_lock && uart_route && !(reg_config&2));
    ++writes;level[44]=0;return mode==6?2:5;
}
esp_err_t uart_wait_tx_done(int n,unsigned timeout) {
    assert(n==1 && timeout==250 && global_lock && bus_lock);++waits;
    if(mode==7) return ESP_FAIL; /* UART still driving LOW. */
    level[44]=1;return ESP_OK;
}
static void reset(void) {
    assert(!global_lock && !bus_lock && !device);
    reg_config=0x0e;width=3;transfers=fail_transfer=0;mode=writes=waits=0;
    leased=true;uart_route=false;level[43]=0;level[44]=1;
}
static FuriHalBw16Guard open_ready(void) {
    FuriHalBw16Guard g={0};
    assert(furi_hal_bw16_guard_open(&g,&token)==ESP_OK && bus_lock);
    assert(reg_config==0x0c);uart_route=true;
    assert(furi_hal_bw16_guard_ready(&g)==ESP_OK && !bus_lock);
    return g;
}
int main(void) {
    reset();FuriHalBw16Guard g={0};leased=false;
    assert(furi_hal_bw16_guard_open(&g,&token)!=ESP_OK && !device);
    for(int f=1;f<=5;++f) {
        reset();mode=f;memset(&g,0,sizeof(g));
        assert(furi_hal_bw16_guard_open(&g,&token)!=ESP_OK);
        mode=0;assert(furi_hal_bw16_guard_close(&g)==ESP_OK);
    }
    for(unsigned f=1;f<=6;++f) {
        reset();fail_transfer=f;memset(&g,0,sizeof(g));
        assert(furi_hal_bw16_guard_open(&g,&token)!=ESP_OK);
        fail_transfer=0;assert(furi_hal_bw16_guard_close(&g)==ESP_OK);
    }
    reset();g=open_ready();
    /* Unsolicited replies toggle CE; power-down and CSN HIGH keep NRF inactive. */
    level[43]=1;assert(!(reg_config&2) && level[44]);
    assert(furi_hal_bw16_guard_scan(&g)==ESP_OK && writes==1 && waits==1 && !bus_lock);
    level[43]=0;assert(furi_hal_bw16_guard_close(&g)==ESP_OK && reg_config==0x0c);
    for(int f=6;f<=7;++f) {
        reset();g=open_ready();mode=f;
        assert(furi_hal_bw16_guard_scan(&g)!=ESP_OK && !bus_lock && !uart_route && level[44]);
        assert(writes==1 && waits==1 && g.failed);
        assert(furi_hal_bw16_guard_scan(&g)!=ESP_OK && writes==1);
        mode=0;assert(furi_hal_bw16_guard_close(&g)==ESP_OK);
    }
    reset();g=open_ready();mode=8;
    assert(furi_hal_bw16_guard_close(&g)!=ESP_OK && !bus_lock && device && !g.ready);
    mode=0;assert(furi_hal_bw16_guard_close(&g)==ESP_OK && !device);
    reset();puts("NRF guard: probe faults, verified power-down, SPI exclusion, short/TX timeout parking, cleanup retry PASS");
}
