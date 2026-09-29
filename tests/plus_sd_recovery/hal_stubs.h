#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>
#include "furi_hal_sd_under_test.h"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_RESPONSE 0x108
#define MALLOC_CAP_DMA 1
#define MALLOC_CAP_INTERNAL 2
#define SPI2_HOST 1
#define FF_USE_TRIM 0
#define SDMMC_DISCARD_ARG 1
#define SDMMC_ERASE_ARG 0
#define SDSPI_SLOT_NO_CD -1
#define SDSPI_SLOT_NO_WP -1
#define SDSPI_SLOT_NO_INT -1
#define ESP_LOGI(...) test_log(__VA_ARGS__)
#define ESP_LOGW(...) test_log(__VA_ARGS__)
#define ESP_LOGE(...) test_log(__VA_ARGS__)
void test_log(const char* tag,const char* format,...) {(void)tag;(void)format;}
const char* esp_err_to_name(esp_err_t e) {(void)e;return "injected";}
typedef unsigned char BYTE,DSTATUS;
typedef uint16_t WORD;
typedef uint32_t DWORD,UINT;
typedef int DRESULT,FRESULT;
enum {STA_NOINIT=1,RES_OK=0,RES_ERROR=1,RES_PARERR=2,FR_OK=0,FR_DISK_ERR=1,FR_NO_FILESYSTEM=13};
enum {CTRL_SYNC,GET_SECTOR_COUNT,GET_SECTOR_SIZE,GET_BLOCK_SIZE};
typedef struct {int unused;} FATFS;
FATFS fatfs_object;
typedef struct {DSTATUS(*init)(BYTE);DSTATUS(*status)(BYTE);DRESULT(*read)(BYTE,BYTE*,DWORD,UINT);DRESULT(*write)(BYTE,const BYTE*,DWORD,UINT);DRESULT(*ioctl)(BYTE,BYTE,void*);} ff_diskio_impl_t;
typedef struct {bool initialized;int host_id,mosi_pin,miso_pin,sck_pin;} FuriHalSpiBus;
FuriHalSpiBus furi_hal_spi_bus_external,furi_hal_spi_bus_subghz;
typedef struct {uint16_t pin;} GpioPin;
const GpioPin gpio_sdcard_cs={13},gpio_sdcard_miso={10};
enum {GpioModeOutputPushPull=1};
typedef int sdspi_dev_handle_t;
typedef struct {int host_id,gpio_cs,gpio_cd,gpio_wp,gpio_int;} sdspi_device_config_t;
typedef struct {int slot,max_freq_khz;esp_err_t(*init)(void);} sdmmc_host_t;
typedef struct {
    uint32_t is_mem,is_sdio,is_mmc,ocr,max_freq_khz;
    int real_freq_khz;
    struct {int sector_size,capacity;} csd;
    struct {int mfg_id,oem_id,revision,date;uint32_t serial;char name[6];} cid;
} sdmmc_card_t;
esp_err_t sdspi_host_init(void);
#define SDSPI_DEVICE_CONFIG_DEFAULT() ((sdspi_device_config_t){.host_id=SPI2_HOST,.gpio_cd=-1,.gpio_wp=-1,.gpio_int=-1})
#define SDSPI_HOST_DEFAULT() ((sdmmc_host_t){.max_freq_khz=20000,.init=sdspi_host_init})
static unsigned lock_depth,peak_lock,device_count,peak_devices,device_adds,device_removes,global_deinits;
static unsigned card_inits,detaches,mounts,registrations,unregistrations,raw_reads,raw_writes,cs_writes;
static int fail_host,fail_device,fail_card,fail_alloc,fail_detach,fail_mount;
static bool card_attached,cs_high,cs_output;
static sdmmc_card_t injected_identity;
static int rates[32];
void furi_hal_spi_bus_lock(void) {if(++lock_depth>peak_lock)peak_lock=lock_depth;}
void furi_hal_spi_bus_unlock(void) {assert(lock_depth);--lock_depth;}
void furi_hal_gpio_write(const GpioPin* p,bool value) {assert(lock_depth&&p->pin==13&&value);cs_high=value;++cs_writes;}
void furi_hal_gpio_init_simple(const GpioPin* p,int mode) {assert(lock_depth&&p->pin==13&&mode==GpioModeOutputPushPull&&cs_high);cs_output=true;}
void* heap_caps_aligned_alloc(size_t align,size_t n,unsigned caps) {(void)align;(void)caps;return malloc(n);}
void* heap_caps_calloc(size_t n,size_t s,unsigned caps) {(void)caps;return calloc(n,s);}
bool esp_ptr_external_ram(const void* p) {(void)p;return false;}
void* test_calloc(size_t n,size_t s) {if(fail_alloc){--fail_alloc;return NULL;}return calloc(n,s);}
esp_err_t sdspi_host_init(void) {assert(lock_depth);if(fail_host){--fail_host;return ESP_FAIL;}return ESP_OK;}
esp_err_t sdspi_host_init_device(const sdspi_device_config_t* c,sdspi_dev_handle_t* h) {
    assert(lock_depth&&c->host_id==SPI2_HOST&&c->gpio_cs==13&&c->gpio_cd==-1);
    if(fail_device){--fail_device;return ESP_FAIL;}
    ++device_adds;++device_count;if(device_count>peak_devices)peak_devices=device_count;
#if CONFIG_T_EMBED_PLUS_SD_RECOVERY
    assert(device_count==1);
#endif
    *h=1;cs_high=cs_output=true;return ESP_OK;
}
esp_err_t sdspi_host_remove_device(sdspi_dev_handle_t h) {
#if CONFIG_T_EMBED_PLUS_SD_RECOVERY
    assert(lock_depth&&!card_attached);
#endif
    assert(h==1&&device_count);--device_count;++device_removes;cs_output=false;return ESP_OK;
}
esp_err_t sdspi_host_deinit(void) {++global_deinits;device_count=0;return ESP_OK;}
esp_err_t sdmmc_card_init(const sdmmc_host_t* h,sdmmc_card_t* c) {
    assert(lock_depth&&device_count==1&&card_inits<32);rates[card_inits++]=h->max_freq_khz;
    if(fail_card){--fail_card;return ESP_FAIL;}
    if(injected_identity.csd.sector_size) {*c=injected_identity;return ESP_OK;}
    c->csd.sector_size=512;c->csd.capacity=8192;c->cid.date=202609;return ESP_OK;
}
void sdmmc_card_print_info(FILE* f,const sdmmc_card_t* c) {(void)f;(void)c;}
esp_err_t sdmmc_get_status(sdmmc_card_t* c) {assert(lock_depth&&c&&device_count==1);return ESP_OK;}
esp_err_t sdmmc_read_sectors(sdmmc_card_t* c,void* b,uint32_t s,uint32_t n) {assert(lock_depth&&c&&device_count==1&&n&&s+n<=8192);memset(b,0x5a,512*n);++raw_reads;return ESP_OK;}
esp_err_t sdmmc_write_sectors(sdmmc_card_t* c,const void* b,uint32_t s,uint32_t n) {assert(lock_depth&&c&&b&&device_count==1&&n&&s+n<=8192);++raw_writes;return ESP_OK;}
void ff_diskio_register(BYTE p,const ff_diskio_impl_t* f) {assert(p==0&&f&&device_count==1);++registrations;}
void ff_diskio_unregister(BYTE p) {
#if CONFIG_T_EMBED_PLUS_SD_RECOVERY
    assert(lock_depth&&!card_attached);
#endif
    assert(!p);++unregistrations;
}
void fatfs_init(void) {memset(&fatfs_object,0,sizeof(fatfs_object));}
FRESULT f_mount(FATFS* fs,const char* path,int immediate) {
    assert(!strcmp(path,"0:")&&immediate==(fs?1:0));
    if(!fs){++detaches;if(fail_detach){--fail_detach;return FR_DISK_ERR;}card_attached=false;return FR_OK;}
    ++mounts;card_attached=true;if(fail_mount){--fail_mount;return FR_DISK_ERR;}return FR_OK;
}
