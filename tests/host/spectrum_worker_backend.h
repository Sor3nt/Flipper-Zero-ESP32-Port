#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include "subghz_spectrum_math.h"
#define COUNT_OF(a) (sizeof(a)/sizeof((a)[0]))
#define furi_assert assert
#define SUBGHZ_SPECTRUM_BINS 128
#define SUBGHZ_WATERFALL_BINS 64
#define FuriWaitForever 0xffffffffu
enum { MALLOC_CAP_INTERNAL=1, MALLOC_CAP_8BIT=2, FuriMutexTypeNormal=0 };
enum { FuriHalSubGhzPath315, FuriHalSubGhzPath433, FuriHalSubGhzPath868, FuriHalSubGhzPathIsolate };
enum { CC1101_IOCFG0, CC1101IocfgHW, CC1101_MDMCFG3, CC1101_AGCCTRL2, CC1101_AGCCTRL1, CC1101_AGCCTRL0, CC1101_MDMCFG4, CC1101StateIDLE };
typedef struct { int32_t (*callback)(void*); void* context; } FuriThread;
typedef struct { bool locked; } FuriMutex;
typedef struct SubGhzSpectrumWorker SubGhzSpectrumWorker;
typedef void (*SubGhzSpectrumWorkerSweepCallback)(void*, const float*, uint32_t, uint32_t, uint16_t);
static unsigned allocations, mutexes, threads, spi_depth, tuned, reads, slept, idled;
static bool calibration_fail;
static uint8_t bandwidth_reg;
static uint32_t frequencies[128], dwell[128];
static int furi_hal_spi_bus_handle_subghz;
static void* heap_caps_calloc(size_t count, size_t size, unsigned caps) {
    assert(caps == (MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)); ++allocations;
    return calloc(count,size);
}
static FuriMutex* furi_mutex_alloc(int type) { assert(type==0); ++mutexes; return calloc(1,sizeof(FuriMutex)); }
static void furi_mutex_acquire(FuriMutex* m, unsigned wait) { assert(wait==FuriWaitForever && !m->locked); m->locked=true; }
static void furi_mutex_release(FuriMutex* m) { assert(m->locked); m->locked=false; }
static void furi_mutex_free(FuriMutex* m) { assert(!m->locked); --mutexes; free(m); }
static FuriThread* furi_thread_alloc_ex(const char* name,unsigned stack,int32_t (*cb)(void*),void* ctx) {
    assert(name && stack==4096); ++threads; FuriThread* t=malloc(sizeof(*t)); *t=(FuriThread){cb,ctx}; return t;
}
static void furi_thread_start(FuriThread* t) { assert(t->callback(t->context)==0); }
static void furi_thread_join(FuriThread* t) { assert(t); }
static void furi_thread_free(FuriThread* t) { --threads; free(t); }
static void furi_hal_spi_acquire(void* bus) { assert(bus && !spi_depth); ++spi_depth; }
static void furi_hal_spi_release(void* bus) { assert(bus && spi_depth==1); --spi_depth; }
static void furi_hal_subghz_reset(void) { assert(!spi_depth); }
static void furi_hal_subghz_set_path(int path) { assert(path>=0 && path<=3); }
static void furi_hal_subghz_idle(void) { assert(!spi_depth); ++idled; }
static void furi_hal_subghz_sleep(void) { assert(!spi_depth); ++slept; }
static bool furi_hal_subghz_is_frequency_valid(uint32_t frequency) { return subghz_spectrum_frequency_supported(frequency); }
static void cc1101_flush_rx(void* b) { assert(b && spi_depth==1); }
static void cc1101_flush_tx(void* b) { assert(b && spi_depth==1); }
static void cc1101_write_reg(void* b,int reg,uint8_t value) { assert(b && spi_depth==1); if(reg==CC1101_MDMCFG4) bandwidth_reg=value; }
static void cc1101_switch_to_idle(void* b) { assert(b && spi_depth==1); }
static void cc1101_set_frequency(void* b,uint32_t f) { assert(b && spi_depth==1 && tuned<128); frequencies[tuned++]=f; }
static void cc1101_calibrate(void* b) { assert(b && spi_depth==1); }
static bool cc1101_wait_status_state(void* b,int state,unsigned timeout) { assert(b && spi_depth==1 && state==CC1101StateIDLE && timeout==10000); return !calibration_fail; }
static void cc1101_switch_to_rx(void* b) { assert(b && spi_depth==1); }
static float furi_hal_subghz_get_rssi(void) { assert(!spi_depth); ++reads; return -60.0f; }
static void furi_delay_ms(unsigned ms) { assert(!spi_depth); if(tuned && reads<tuned) dwell[tuned-1]=ms; }
