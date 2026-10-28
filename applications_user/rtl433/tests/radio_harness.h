#pragma once
/* Executes the actual radio worker with the deployed ESP32 Furi stack limit.
 * CC1101 access is simulated; this does not claim physical RF validation. */
#include "rtl_radio.h"
#include "rtl_file.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct FuriThread {
  pthread_t thread;
  int32_t (*fn)(void *);
  void *ctx;
  bool started, joined;
  bool capture;
};
struct FuriMessageQueue {
  pthread_mutex_t lock;
  pthread_cond_t changed;
  size_t cap, size, count, head;
  unsigned char data[];
};
struct FuriMutex { pthread_mutex_t lock; };
static atomic_uint thread_count, rx_starts, rx_stops, decodes;
static atomic_uint live_decodes;
static atomic_uint fsk_decodes;
static atomic_int test_rssi = -60;
static void (*rx_callback)(bool, uint32_t, void *);
static void *rx_context;
static atomic_bool rx_active;
static atomic_uint_fast64_t clock_offset;
static unsigned registry_count, largest_stack;
static bool tx_active, tx_blocked, tx_complete;
static LevelDuration (*tx_callback)(void *);
static void *tx_context;
static FuriHalSubGhzPreset loaded_preset;
static uint32_t loaded_hz;
static unsigned tx_count, tx_stops;
/* Controlled scheduling pause. GPIO callbacks still enqueue received edges. */
static atomic_bool pause_capture, capture_paused;
static _Thread_local bool is_capture_worker;

static int64_t monotonic_us(void) {
  struct timespec t;
  assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
  return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
int64_t esp_timer_get_time(void) { return monotonic_us() + atomic_load(&clock_offset); }
uint32_t furi_get_tick(void) { return monotonic_us() / 1000; }
uint32_t furi_kernel_get_tick_frequency(void) { return 1000; }
void furi_delay_tick(uint32_t n) {
  if (is_capture_worker && atomic_load(&pause_capture)) {
    atomic_store(&capture_paused, true);
    while (atomic_load(&pause_capture)) usleep(100);
    atomic_store(&capture_paused, false);
  }
  usleep(n * 1000);
}
static void *thread_entry(void *p) {
  FuriThread *t = p;
  is_capture_worker = t->capture;
  t->fn(t->ctx);
  return NULL;
}
FuriThread *furi_thread_alloc_ex(const char *name, uint32_t stack,
                               int32_t (*fn)(void *), void *ctx) {
  /* components/furi/core/thread.c: UINT16_MAX * sizeof(StackType_t).
   * ESP-IDF 5.4.1 Xtensa portmacro.h: StackType_t == uint8_t. */
  if (!stack || stack > UINT16_MAX) {
    fprintf(stderr, "FURI STACK LIMIT: %s requested %u bytes; maximum 65535\n", name, stack);
    abort();
  }
  if (stack > largest_stack) largest_stack = stack;
  FuriThread *t = calloc(1, sizeof(*t));
  assert(t);
  t->fn = fn;
  t->ctx = ctx;
  t->capture = !strcmp(name, "RTLCapture");
  atomic_fetch_add(&thread_count, 1);
  return t;
}
void furi_thread_set_priority(FuriThread *t, FuriThreadPriority p) { (void)t; (void)p; }
void furi_thread_start(FuriThread *t) {
  assert(t && !t->started);
  t->started = true;
  assert(pthread_create(&t->thread, NULL, thread_entry, t) == 0);
}
void furi_thread_join(FuriThread *t) {
  assert(t && t->started && !t->joined);
  assert(pthread_join(t->thread, NULL) == 0);
  t->joined = true;
}
void furi_thread_free(FuriThread *t) {
  assert(t && (!t->started || t->joined));
  atomic_fetch_sub(&thread_count, 1);
  free(t);
}
FuriMessageQueue *furi_message_queue_alloc(uint32_t n, uint32_t size) {
  FuriMessageQueue *q = calloc(1, sizeof(*q) + n * size);
  assert(q);
  q->cap = n;
  q->size = size;
  pthread_mutex_init(&q->lock, NULL);
  pthread_cond_init(&q->changed, NULL);
  return q;
}
FuriStatus furi_message_queue_put(FuriMessageQueue *q, const void *p, uint32_t wait) {
  pthread_mutex_lock(&q->lock);
  while (q->count == q->cap && wait == FuriWaitForever)
    pthread_cond_wait(&q->changed, &q->lock);
  bool ok = q->count < q->cap;
  if (ok) {
    memcpy(q->data + ((q->head + q->count) % q->cap) * q->size, p, q->size);
    q->count++;
    pthread_cond_broadcast(&q->changed);
  }
  pthread_mutex_unlock(&q->lock);
  return ok ? FuriStatusOk : FuriStatusError;
}
FuriStatus furi_message_queue_get(FuriMessageQueue *q, void *p, uint32_t wait) {
  pthread_mutex_lock(&q->lock);
  while (!q->count && wait == FuriWaitForever)
    pthread_cond_wait(&q->changed, &q->lock);
  bool ok = q->count != 0;
  if (ok) {
    memcpy(p, q->data + q->head * q->size, q->size);
    q->head = (q->head + 1) % q->cap;
    q->count--;
    pthread_cond_broadcast(&q->changed);
  }
  pthread_mutex_unlock(&q->lock);
  return ok ? FuriStatusOk : FuriStatusError;
}
void furi_message_queue_free(FuriMessageQueue *q) {
  assert(!q->count);
  pthread_mutex_destroy(&q->lock);
  pthread_cond_destroy(&q->changed);
  free(q);
}
FuriMutex *furi_mutex_alloc(FuriMutexType type) {
  (void)type;
  FuriMutex *m = malloc(sizeof(*m));
  assert(m);
  pthread_mutex_init(&m->lock, NULL);
  return m;
}
void furi_mutex_acquire(FuriMutex *m, uint32_t wait) { (void)wait; pthread_mutex_lock(&m->lock); }
void furi_mutex_release(FuriMutex *m) { pthread_mutex_unlock(&m->lock); }
void furi_mutex_free(FuriMutex *m) { pthread_mutex_destroy(&m->lock); free(m); }

void subghz_devices_init(void) { assert(registry_count++ == 0); }
void subghz_devices_deinit(void) { assert(registry_count-- == 1); }
const SubGhzDevice *subghz_devices_get_by_name(const char *name) {
  (void)name;
  static const SubGhzDevice device = {1};
  return &device;
}
bool subghz_devices_is_frequency_valid(const SubGhzDevice *d, uint32_t hz) {
  assert(d);
  return (hz >= 281000000 && hz <= 361000000) ||
         (hz >= 378000000 && hz <= 481000000) ||
         (hz >= 749000000 && hz <= 962000000);
}
void subghz_devices_idle(const SubGhzDevice *d) { assert(d); }
void subghz_devices_reset(const SubGhzDevice *d) { assert(d && !atomic_load(&rx_active)); }
void subghz_devices_load_preset(const SubGhzDevice *d, FuriHalSubGhzPreset p, uint8_t *v) {
  assert(d && !atomic_load(&rx_active) && !tx_active);
  loaded_preset = p;
  if (p == FuriHalSubGhzPresetCustom) assert(v);
}
uint32_t subghz_devices_set_frequency(const SubGhzDevice *d, uint32_t hz) {
  assert(subghz_devices_is_frequency_valid(d, hz));
  loaded_hz = hz;
  return hz;
}
void subghz_devices_flush_rx(const SubGhzDevice *d) { assert(d); }
void subghz_devices_start_async_rx(const SubGhzDevice *d,
                                 void (*cb)(bool, uint32_t, void *), void *ctx) {
  assert(d && cb && ctx && !atomic_exchange(&rx_active, true));
  rx_callback = cb; rx_context = ctx;
  atomic_fetch_add(&rx_starts, 1);
}
void subghz_devices_stop_async_rx(const SubGhzDevice *d) {
  assert(d && atomic_exchange(&rx_active, false));
  atomic_fetch_add(&rx_stops, 1);
}
void subghz_devices_sleep(const SubGhzDevice *d) { assert(d && !atomic_load(&rx_active) && !tx_active); }
float subghz_devices_get_rssi(const SubGhzDevice *d) { assert(d); return atomic_load(&test_rssi); }
bool subghz_devices_start_async_tx(const SubGhzDevice *d, void *cb, void *ctx) {
  assert(d && cb && ctx && !atomic_load(&rx_active) && !tx_active);
  if (tx_blocked) return false;
  tx_active = true; tx_complete = false;
  tx_callback = cb; tx_context = ctx; tx_count++;
  return true;
}
bool subghz_devices_is_async_complete_tx(const SubGhzDevice *d) {
  assert(d && tx_active); return tx_complete;
}
void subghz_devices_stop_async_tx(const SubGhzDevice *d) {
  assert(d && tx_active); tx_active = false; tx_callback = NULL; tx_context = NULL; tx_stops++;
}
