[← Back to index](README.md)

# Internal RAM (DRAM) Pressure

This is the single area that has caused the most actual (if intermittent)
crashes and silent failures on paradoxt: `StoreProhibited` panics,
`ENOMEM`/`pthread create failed`, "Spectrum unavailable / Not enough memory,"
WiFi scans that silently return nothing, SPI reads that silently come back as
`0xFF`. Read this before adding any buffer, task stack, or large allocation.

## Why it's tight

From the actual linker map (`build/furi_esp32.map`): the internal DRAM
segment is ~334 KB, but **`.dram0.dummy` (the D/IRAM overlap region that code
placed in IRAM steals from DRAM) is ~132 KB by itself** — bigger than static
`.data` (~31.6 KB) + `.bss` (~79.2 KB) combined. **The single biggest lever on
internal RAM headroom is how much code runs from IRAM
(`CONFIG_ESP_WIFI_IRAM_OPT` / `..._RX_IRAM_OPT`, flash-XIP tradeoffs), not
static buffers.** Moving `.bss` to PSRAM only reclaims a modest amount (~15 KB
from doing all five safe candidate buffers) — do it, but don't expect it to be
a silver bullet.

`CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` is currently **81920 (80 KB)**, set in
**both** `sdkconfig` and `sdkconfig.defaults.esp32s3` — editing only
`sdkconfig` gets silently reverted the next time CMake reconfigures. If you
ever retune this value, change both files, and don't push it up near the full
internal budget — 80 KB was already a deliberate step down from an earlier,
too-aggressive 96 KB candidate.

## Rules

- **Never allocate a large static/CPU-only buffer with default (internal)
  RAM.** Tag it `EXT_RAM_BSS_ATTR` (`#include <esp_attr.h>`) so it lands in
  PSRAM. This pattern is already used in ~20 files (`ble_walk_hal.c`,
  `chameleon.c`, `cast_client.c`, `airplay_raop.c`, the `wlan_app` scanners,
  etc.) — follow it, don't reinvent it.
- **Never move a DMA buffer or an RMT/GPTimer ISR structure to PSRAM.** IDF's
  `RMT_ISR_IRAM_SAFE` / `GPTIMER_ISR_IRAM_SAFE` reject PSRAM pointers
  outright, and WiFi's static RX buffers must stay internal DMA memory.
  Internal-only, by hardware/IDF constraint: the Sub-GHz TX buffer, WiFi
  `STATIC_RX_BUFFER_NUM` buffers, any IR/RMT symbol buffer.
- **Any `heap_caps_calloc(..., MALLOC_CAP_INTERNAL)` (or `furi_thread_alloc` /
  `furi_thread_alloc_ex`) can return `NULL` under real RAM pressure — always
  check it.** `components/furi/core/thread.c` (`furi_thread_alloc` /
  `furi_thread_alloc_ex`) already does this correctly (returns `NULL` + logs,
  no `furi_check`, callers that already handle `NULL` — e.g. the Sub-GHz
  Spectrum worker — degrade gracefully). If you write new code that calls
  these, or any other internal allocator, **check the return value before
  touching it.** The crash this protects against is a `StoreProhibited` panic
  that only fires when internal RAM happens to be exhausted — i.e. it will
  pass every quick test and then crash in the field.
- **Spawning a worker task must not need internal RAM that is itself being
  freed by that same worker.** A real bug: WiFi scan spawned a `pthread`
  (internal-RAM TCB+stack) whose *job* was to call `bt_stop_stack()` and free
  up internal RAM — chicken-and-egg `ENOMEM` whenever BT was still resident.
  Fix pattern: use `xTaskCreateWithCaps(..., MALLOC_CAP_SPIRAM)` (stack in
  PSRAM, TCB is tiny and stays internal) so task creation doesn't need the RAM
  it's trying to release. `esp_wifi_*` itself still runs fine from a
  PSRAM-backed caller stack — proven by the Smart Deauth code path.
- **`vTaskDeleteWithCaps(NULL)` (self-delete of a `WithCaps` task) spawns its
  own tiny *internal-RAM* helper task and `abort()`s if that allocation
  fails.** Only self-delete *after* whatever freed internal RAM (e.g. BT
  teardown) has already happened on that task — not before.
- **The SPI driver auto-bounces non-DMA-capable (PSRAM) buffers through an
  internal `MALLOC_CAP_DMA` allocation per transaction.** This is IDF
  behavior, not a bug by itself — but under internal-RAM pressure that
  per-transaction alloc fails, and the transfer silently degrades (stub
  `0xFF` reads on CC1101/NRF24 register reads). `furi_hal_spi_bus.c` now keeps
  small fixed internal `.bss` bounce buffers for exactly this reason — if you
  touch SPI bus internals, keep that pattern, don't let it regress to an
  implicit per-transaction malloc.
- **DMA-capable render buffers (e.g. the Spectrum waterfall "stripe") can
  only live in internal DMA RAM** (`esp_lcd` DMA can't source from PSRAM) —
  when that allocation can't get its ideal size, shrink the working size (row
  count, etc.) instead of failing outright, and always have a graceful "not
  enough memory" path rather than letting a bare allocation failure fall
  through into a null dereference.
- **Diagnostic habit, before guessing:** log
  `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` and
  `heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)` at the point of
  failure. If *free* is stable but *largest contiguous block* is shrinking
  over time, that's fragmentation (classic cause here: repeated BT
  park/resume-on-idle cycles tearing down and reinitializing Bluedroid), not
  a leak — don't "fix" it as a leak.
- A background "park idle BT to save power" feature is exactly what starves
  internal RAM for everything else that still needs it (WiFi scan workers,
  Spectrum, Smart Deauth). Any new power feature that keeps a radio stack
  resident needs to be checked against this before it ships.
