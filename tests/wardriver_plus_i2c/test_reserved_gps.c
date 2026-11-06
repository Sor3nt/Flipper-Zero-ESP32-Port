/* Production config loader, with storage reads that can fail without EOF. */
#include "wardriver_config.h"
#include <storage/storage.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct File { const char* data; };
static const char* primary;
static const char* backup;
static bool fail_read,fail_alloc;
static unsigned reads,allocs,frees;
#define CHECK(c) do { if(!(c)) { fprintf(stderr,"config FAIL line %d: %s\n",__LINE__,#c); exit(1); } } while(0)
void* furi_record_open(const char* name) { (void)name; return (void*)1; }
void furi_record_close(const char* name) { (void)name; }
File* storage_file_alloc(Storage* s) { (void)s; if(fail_alloc) return NULL; ++allocs; return calloc(1,sizeof(File)); }
bool storage_file_open(File* f,const char* path,int access,int mode) {
    CHECK(access==FSAM_READ && mode==FSOM_OPEN_EXISTING);
    f->data=strstr(path,".bak")?backup:primary; return f->data!=NULL;
}
size_t storage_file_read(File* f,void* buffer,size_t n) {
    ++reads; CHECK(reads<=2 && n<=512); if(fail_read) return 0;
    size_t length=strlen(f->data); if(length>n) length=n;
    memcpy(buffer,f->data,length); return length;
}
bool storage_file_close(File* f) { f->data=NULL; return true; }
void storage_file_free(File* f) { ++frees; free(f); }
/* Loading must never write. COFF linkers retain references from the uncalled
 * save function, so provide explicit fail-fast boundaries on Windows too. */
bool storage_simply_mkdir(Storage* s,const char* p) { (void)s; (void)p; abort(); }
size_t storage_file_write(File* f,const void* b,size_t n) { (void)f; (void)b; (void)n; abort(); }
bool storage_file_sync(File* f) { (void)f; abort(); }
int storage_common_stat(Storage* s,const char* p,FileInfo* i) { (void)s; (void)p; (void)i; abort(); }
int storage_common_rename(Storage* s,const char* a,const char* b) { (void)s; (void)a; (void)b; abort(); }
int storage_common_remove(Storage* s,const char* p) { (void)s; (void)p; abort(); }
static bool load(WardriverConfig* c) {
    reads=0; bool ok=wardriver_config_load(c); CHECK(allocs==frees); return ok;
}

#include "wardriver_gps.h"
#include <furi_hal_uart_shared.h>
#include <furi_hal_shared_pins.h>
#include <furi_hal_power.h>
#include <driver/uart.h>
#include <driver/gpio.h>
#include <assert.h>
static unsigned gps_calls,gps_installs,gps_deletes,gps_resets,gps_reads;
static bool gps_busy;
static int wanted_rx,wanted_tx;
bool uart_is_driver_installed(int uart) { assert(uart==1 || uart==2);++gps_calls;return gps_busy; }
esp_err_t uart_driver_install(int uart,int rx,int tx,int count,QueueHandle_t* queue,int flags) {
    assert((uart==1 || uart==2) && rx==2048 && !tx && !count && !queue && !flags);
    ++gps_calls;++gps_installs;return ESP_OK;
}
esp_err_t uart_driver_delete(int uart) { assert(uart==1 || uart==2);++gps_calls;++gps_deletes;return ESP_OK; }
esp_err_t uart_param_config(int uart,const uart_config_t* cfg) {
    assert((uart==1 || uart==2) && cfg->baud_rate==9600);++gps_calls;return ESP_OK;
}
esp_err_t uart_set_pin(int uart,int tx,int rx,int rts,int cts) {
    assert((uart==1 || uart==2) && tx==wanted_tx && rx==wanted_rx && rts==-1 && cts==-1);
    ++gps_calls;return ESP_OK;
}
int uart_read_bytes(int uart,void* data,size_t length,unsigned timeout) {
    assert((uart==1 || uart==2) && data && length==256 && !timeout);++gps_calls;++gps_reads;return 0;
}
esp_err_t gpio_reset_pin(int pin) {
    assert(pin==wanted_rx || pin==wanted_tx);++gps_calls;++gps_resets;return ESP_OK;
}
/* Firmware arbiters. GPS must hold the UART lease for the whole session, the GPIO43/44
 * lease and snapshot only for those pins, and give everything back in reverse order. */
static unsigned uart_leases,pin_leases,pin_saves,pin_restores;
static bool uart_held,pins_held,pins_saved,uart_lease_busy;
void furi_delay_ms(uint32_t ms) { (void)ms; }
FuriHalUartSharedAcquireResult furi_hal_uart_shared_acquire(uint32_t timeout_ms) {
    assert(timeout_ms==1000);
    if(uart_lease_busy) return FuriHalUartSharedAcquireBusy;
    assert(!uart_held);++uart_leases;uart_held=true;return FuriHalUartSharedAcquireAcquired;
}
bool furi_hal_uart_shared_release(void) { assert(uart_held && !pins_saved);uart_held=false;return true; }
bool furi_hal_shared_pins_acquire(const void* owner) { assert(owner && uart_held && !pins_held);++pin_leases;pins_held=true;return true; }
void furi_hal_shared_pins_release(const void* owner) { assert(owner && pins_held && !pins_saved);pins_held=false; }
bool furi_hal_shared_pins_save(const void* owner) { assert(owner && pins_held && !pins_saved);++pin_saves;pins_saved=true;return true; }
void furi_hal_shared_pins_restore(const void* owner) { assert(owner && pins_held && pins_saved);++pin_restores;pins_saved=false; }
bool furi_hal_shared_pins_is_saved(const void* owner) { (void)owner;return pins_saved; }
/* Light sleep gates the UART clock: GPS vetoes it for the whole session. */
static int sleep_depth,sleep_enters;
void furi_hal_power_light_sleep_inhibit_enter(void) { ++sleep_depth;++sleep_enters; }
void furi_hal_power_light_sleep_inhibit_exit(void) { assert(sleep_depth>0);--sleep_depth; }
#define NO_LEASES() assert(!uart_held && !pins_held && !pins_saved && !sleep_depth)
static void saved(WardriverConfig* c,unsigned mode,unsigned uart,int rx,int tx) {
    static char settings[300];
    snprintf(settings,sizeof(settings),"Filetype: Wardriver settings\nVersion: 1\nGPS Mode: %u\nGPS UART: %u\n"
             "GPS Baud: 9600\nGPS RX: %u\nGPS TX: %u\nIsolated UART Pins: true\n",mode,uart,(unsigned)rx,(unsigned)tx);
    primary=settings;CHECK(load(c));
    assert(c->gps_mode==mode && c->gps_uart==uart && c->gps_rx==rx && c->gps_tx==tx && c->isolated_uart_pins);
}
int main(void) {
    WardriverConfig config;WardriverGps gps;
#if CONFIG_T_EMBED_BW16_I2C
#if CONFIG_T_EMBED_EXTERNAL_UART
    const int reserved[]={8,18};
    const unsigned first_pair=1;
#else
    const int reserved[]={43,44,8,18};
    const unsigned first_pair=0;
#endif
    for(unsigned mode=1;mode<=2;++mode)for(unsigned uart=1;uart<=2;++uart)
        for(unsigned pin=0;pin<sizeof(reserved)/sizeof(reserved[0]);++pin)for(unsigned side=0;side<2;++side) {
            saved(&config,mode,uart,side?16:reserved[pin],side?reserved[pin]:17);
            WardriverConfig unchanged=config;unsigned before=gps_calls;
            wardriver_gps_start(&gps,&config);
            assert(!gps.installed && !gps.parser.data.enabled && !strcmp(gps.status,"PIN IN USE"));
            wardriver_gps_poll(&gps,100);wardriver_gps_stop(&gps);
            assert(gps_calls==before && !memcmp(&config,&unchanged,sizeof(config)));
        }
    for(unsigned pair=first_pair;pair<2;++pair) {
        saved(&config,2,2,pair?8:44,pair?18:43);unsigned before=gps_calls;
        wardriver_gps_start(&gps,&config);wardriver_gps_stop(&gps);
        assert(!gps.installed && !strcmp(gps.status,"PIN IN USE") && gps_calls==before);
    }
    assert(!gps_installs && !gps_deletes && !gps_resets && !gps_reads);
    assert(!uart_leases && !pin_leases && !pin_saves && !pin_restores && !sleep_enters);NO_LEASES();
    puts("PASS: saved AUTO/ON GPS settings, both UARTs, all profile-reserved RX/TX pins blocked before every UART/GPIO/lease call; settings unchanged");
#endif
#if !CONFIG_T_EMBED_BW16_I2C || CONFIG_T_EMBED_EXTERNAL_UART
    saved(&config,2,2,44,43);wanted_rx=44;wanted_tx=43;
    wardriver_gps_start(&gps,&config);assert(gps.installed && gps.parser.data.enabled);
    /* The shared header: UART lease + GPIO43/44 lease + snapshot, all held while listening. */
    assert(uart_held && pins_held && pins_saved && uart_leases==1 && pin_leases==1 && pin_saves==1);
    assert(sleep_depth==1 && sleep_enters==1);
    wardriver_gps_stop(&gps);assert(gps_resets==2);
    assert(pin_restores==1);NO_LEASES();
    /* A refused UART lease leaves the driver and GPIO43/44 untouched. */
    uart_lease_busy=true;unsigned refused=gps_calls;
    wardriver_gps_start(&gps,&config);wardriver_gps_stop(&gps);
    assert(!gps.installed && !strcmp(gps.status,"UART BUSY") && gps_calls==refused && pin_leases==1);
    assert(sleep_enters==1);
    uart_lease_busy=false;NO_LEASES();
    puts("PASS: non-Plus profile retains prior GPS 44/43 behavior, including UART+I2C; leases held and returned in order");
#endif
    saved(&config,0,2,44,43);unsigned before=gps_calls;
    wardriver_gps_start(&gps,&config);wardriver_gps_stop(&gps);
    assert(!gps.installed && !gps.parser.data.enabled && !strcmp(gps.status,"OFF") && gps_calls==before);
    saved(&config,1,1,16,17);wanted_rx=16;wanted_tx=17;
    unsigned leases_before=uart_leases,pins_before=pin_leases,saves_before=pin_saves;
    wardriver_gps_start(&gps,&config);assert(gps.installed && gps.parser.data.enabled);
    /* Other pins still take the UART lease (so the RFID reader yields UART1) but
     * not the GPIO43/44 lease or snapshot. */
    assert(uart_held && !pins_held && uart_leases==leases_before+1 && pin_leases==pins_before);
    assert(sleep_depth==1);
    wardriver_gps_poll(&gps,100);wardriver_gps_stop(&gps);
    assert(!gps.installed && gps_reads==1 && pin_saves==saves_before);NO_LEASES();
    saved(&config,2,2,16,-1);wanted_rx=16;wanted_tx=-1;
    wardriver_gps_start(&gps,&config);assert(gps.installed);
    wardriver_gps_stop(&gps);
    config.isolated_uart_pins=false;before=gps_calls;
    wardriver_gps_start(&gps,&config);assert(!gps.installed && !strcmp(gps.status,"CHECK PINS") && gps_calls==before);
    config.isolated_uart_pins=true;gps_busy=true;before=gps_installs;
    wardriver_gps_start(&gps,&config);wardriver_gps_stop(&gps);
    assert(!gps.installed && !strcmp(gps.status,"UART BUSY") && gps_installs==before);
    NO_LEASES();
    puts("PASS: GPS OFF and other explicitly isolated pins retain validation, listen-only mode and cleanup");
    return 0;
}
