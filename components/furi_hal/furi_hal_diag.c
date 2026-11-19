#include "furi_hal_diag.h"
#include "furi_hal_rtc.h"

#include <furi.h>
#include <toolbox/fw_version.h>

#include <sdkconfig.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <esp_app_desc.h>
#include <esp_attr.h>
#include <esp_cpu.h>
#include <esp_log.h>
#include <esp_memory_utils.h>
#include <esp_rom_crc.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <nvs.h>
#include <soc/soc_caps.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/* The ESP32-C6 BLE controller wraps esp_panic_handler() itself when its log dump
 * option is on, and the linker allows only one wrapper: ours stays out then (see
 * CMakeLists.txt). Abnormal resets are still logged, only without panic details. */
#if CONFIG_BT_LE_CONTROLLER_LOG_WRAP_PANIC_HANDLER_ENABLE
#define FURI_HAL_DIAG_WRAP_PANIC 0
#else
#define FURI_HAL_DIAG_WRAP_PANIC 1
#endif

#if FURI_HAL_DIAG_WRAP_PANIC
#include <esp_debug_helpers.h>
#include <esp_private/panic_internal.h>
#if __XTENSA__
#include <esp_cpu_utils.h>
#include <xtensa/corebits.h>
#include <xtensa_context.h>
#elif __riscv
#include <riscv/rvruntime-frames.h>
#endif
#endif

#define TAG "FuriHalDiag"

/* Bump both when FuriHalDiagPanic / FuriHalDiagCrash change: old records are
 * then ignored instead of misread. */
#define FURI_HAL_DIAG_RTC_MAGIC     0xD1A60002UL
#define FURI_HAL_DIAG_NVS_VERSION   2
#define FURI_HAL_DIAG_GUARD_MAGIC   0x6A4D1A60UL
#define FURI_HAL_DIAG_REASON_UNSET  0xFFFFFFFFUL
#define FURI_HAL_DIAG_NVS_NAMESPACE "furi_diag"
#define FURI_HAL_DIAG_NVS_KEY       "last_crash"
/* 2024-01-01: an RTC time before that means the clock was never set. */
#define FURI_HAL_DIAG_CLOCK_VALID 1704067200UL

_Static_assert(sizeof(FuriHalDiagPanic) % 4 == 0, "FuriHalDiagPanic must be whole words");
_Static_assert(sizeof(FuriHalDiagCrash) % 4 == 0, "FuriHalDiagCrash must be whole words");

/* One per CPU core: a core only ever writes its own slot, so two cores
 * panicking at once (or one stalled by the other half-way) cannot mix their
 * data. Any slot is valid only with the right magic and CRC. */
typedef struct {
    uint32_t magic; /* written last */
    uint32_t crc; /* esp_rom_crc32_le() of everything after this field */
    uint32_t seq; /* capture order: the lowest valid seq is the first crash */
    uint32_t reset_reason; /* FURI_HAL_DIAG_REASON_UNSET until the next boot sets it */
    FuriHalDiagPanic panic;
} FuriHalDiagRtcSlot;

typedef struct {
    uint16_t version;
    uint16_t size;
    FuriHalDiagCrash crash;
} FuriHalDiagNvsRecord;

/* RTC memory, not initialised at startup (garbage after a power-on reset, hence
 * magic + CRC). Survives panic, abort, esp_restart and watchdog resets. */
static RTC_NOINIT_ATTR FuriHalDiagRtcSlot furi_hal_diag_rtc_slot[SOC_CPU_CORES_NUM];
/* Where a crash record is assembled before it is committed to its slot: a
 * fault while gathering leaves the slot's last good copy untouched. Kept in RTC
 * memory rather than on the stack, which may be the very thing that overflowed. */
static RTC_NOINIT_ATTR FuriHalDiagPanic furi_hal_diag_rtc_scratch[SOC_CPU_CORES_NUM];
/* Set while the boot step reads/writes NVS. Still set at the next boot means
 * that step itself crashed: it is skipped once instead of boot-looping. */
static RTC_NOINIT_ATTR uint32_t furi_hal_diag_rtc_guard;

static uint8_t furi_hal_diag_reset_reason = ESP_RST_UNKNOWN;
/* History loaded at boot. Static (no allocation that could fail) and in PSRAM
 * where the board has it. Written only by furi_hal_diag_init() before any
 * service starts, read-only afterwards: no lock needed. */
static EXT_RAM_BSS_ATTR FuriHalDiagCrash furi_hal_diag_history;
static bool furi_hal_diag_have_history = false;

static uint32_t furi_hal_diag_slot_crc(const FuriHalDiagRtcSlot* slot) {
    return esp_rom_crc32_le(
        0,
        (const uint8_t*)&slot->seq,
        sizeof(FuriHalDiagRtcSlot) - offsetof(FuriHalDiagRtcSlot, seq));
}

/* ------------------------------------------------------------------------ */
/* Panic time: runs inside ESP-IDF's panic handler, before it prints anything.
 * Only RAM writes: no lock, no allocation, no flash write, no printf, nothing
 * large on the stack. ESP-IDF re-enables the flash cache before this point
 * (panic_enable_cache() in panic_handler.c), so reading strings from flash is
 * fine; it prints the same strings itself right after. */
/* ------------------------------------------------------------------------ */

#if FURI_HAL_DIAG_WRAP_PANIC

#if CONFIG_ESP_PANIC_HANDLER_IRAM
#define FURI_HAL_DIAG_PANIC_ATTR IRAM_ATTR
#else
#define FURI_HAL_DIAG_PANIC_ATTR
#endif

/* panic.c: set by esp_system_abort() (abort(), assert, furi_crash, stack
 * overflow) before it raises the exception; the message is in the details. */
extern bool g_panic_abort;
extern char* g_panic_abort_details;

void __real_esp_panic_handler(panic_info_t* info);

/* Per core, internal RAM, zeroed at boot: 1 once that core started a capture.
 * A second panic on the same core (a fault while capturing or while ESP-IDF
 * prints) then keeps the first crash and goes straight to ESP-IDF. */
static volatile uint8_t furi_hal_diag_core_busy[SOC_CPU_CORES_NUM];
static volatile uint32_t furi_hal_diag_seq;

static uint32_t FURI_HAL_DIAG_PANIC_ATTR furi_hal_diag_next_seq(void) {
    /* Atomic across cores; bounded so a broken bus can never hang the panic path. */
    for(uint8_t attempt = 0; attempt < 16; attempt++) {
        const uint32_t current = furi_hal_diag_seq;
        if(esp_cpu_compare_and_set(&furi_hal_diag_seq, current, current + 1)) {
            return current + 1;
        }
    }
    return ++furi_hal_diag_seq;
}

static inline bool furi_hal_diag_readable(const void* ptr) {
    return ptr && (esp_ptr_byte_accessible(ptr) || esp_ptr_in_drom(ptr));
}

/* Bounded copy that never trusts the source: printable ASCII only.
 * @return true when src did not fit. */
static bool FURI_HAL_DIAG_PANIC_ATTR
    furi_hal_diag_copy_str(char* dst, size_t size, const char* src) {
    size_t i = 0;
    bool truncated = false;
    if(furi_hal_diag_readable(src)) {
        for(; src[i] != '\0'; i++) {
            if(i + 1 >= size) {
                truncated = true;
                break;
            }
            const char c = src[i];
            if(c == '\r' || c == '\n' || c == '\t') {
                dst[i] = ' ';
            } else {
                dst[i] = (c >= 0x20 && c < 0x7f) ? c : '?';
            }
        }
    }
    dst[i] = '\0';
    return truncated;
}

/* Publish the scratch record in this core's slot. Cannot fault: RTC to RTC
 * copies only. The slot is invalid (magic 0) for the duration of the copy. */
static void FURI_HAL_DIAG_PANIC_ATTR
    furi_hal_diag_commit(FuriHalDiagRtcSlot* slot, const FuriHalDiagPanic* panic, uint32_t seq) {
    slot->magic = 0;
    __sync_synchronize();
    slot->seq = seq;
    slot->reset_reason = FURI_HAL_DIAG_REASON_UNSET;
    memcpy(&slot->panic, panic, sizeof(*panic));
    slot->crc = furi_hal_diag_slot_crc(slot);
    __sync_synchronize();
    slot->magic = FURI_HAL_DIAG_RTC_MAGIC;
    __sync_synchronize();
}

static void FURI_HAL_DIAG_PANIC_ATTR furi_hal_diag_capture_task(FuriHalDiagPanic* panic) {
    /* Lock-free: reads pxCurrentTCBs[core] and the TCB's name array. */
    TaskHandle_t task = xTaskGetCurrentTaskHandleForCore(panic->core);
    if(task && esp_ptr_byte_accessible(task)) {
        furi_hal_diag_copy_str(panic->task, sizeof(panic->task), pcTaskGetName(task));
    }
}

static void FURI_HAL_DIAG_PANIC_ATTR
    furi_hal_diag_capture_frame(FuriHalDiagPanic* panic, const void* frame_ptr) {
    if(!esp_ptr_byte_accessible(frame_ptr)) return;

#if __XTENSA__
    const XtExcFrame* frame = frame_ptr;
    panic->cause = frame->exccause;
    panic->fault_addr = frame->excvaddr;
    panic->sp = frame->a1;

    /* Register dump, in the order the monitor prints it. */
    const uint32_t regs[] = {
        frame->pc,       frame->ps,  frame->a0,  frame->a1,  frame->a2,     frame->a3,
        frame->a4,       frame->a5,  frame->a6,  frame->a7,  frame->a8,     frame->a9,
        frame->a10,      frame->a11, frame->a12, frame->a13, frame->a14,    frame->a15,
        frame->sar,      frame->exccause,        frame->excvaddr,
#if XCHAL_HAVE_LOOPS
        frame->lbeg,     frame->lend,            frame->lcount,
#endif
    };
    _Static_assert(COUNT_OF(regs) <= FURI_HAL_DIAG_REGS_MAX, "regs[] too small");
    memcpy(panic->regs, regs, sizeof(regs));
    panic->regs_count = COUNT_OF(regs);

    /* The same walk as esp_backtrace_print_from_frame(), which prints the
     * monitor's "Backtrace:" line: faulting PC first, then each caller. Every
     * stack read happens only after its stack pointer was checked as sane. */
    esp_backtrace_frame_t stack = {
        .pc = frame->pc,
        .sp = frame->a1,
        .next_pc = frame->a0,
        .exc_frame = frame,
    };
    bool corrupted = !(esp_stack_ptr_is_sane(stack.sp) &&
                       (esp_ptr_executable((void*)esp_cpu_process_stack_pc(stack.pc)) ||
                        frame->exccause == EXCCAUSE_INSTR_PROHIBITED));
    uint8_t depth = 0;
    panic->backtrace[depth++] = esp_cpu_process_stack_pc(stack.pc);
    while(depth < FURI_HAL_DIAG_BACKTRACE_MAX && stack.next_pc != 0 && !corrupted) {
        if(!esp_backtrace_get_next_frame(&stack)) corrupted = true;
        panic->backtrace[depth++] = esp_cpu_process_stack_pc(stack.pc);
    }
    panic->backtrace_depth = depth;
    panic->backtrace_end = corrupted ? 2 : (stack.next_pc != 0 ? 1 : 0);
#elif __riscv
    const RvExcFrame* frame = frame_ptr;
    panic->cause = frame->mcause;
    panic->fault_addr = frame->mtval;
    panic->sp = frame->sp;

    /* RvExcFrame is MEPC, RA, SP, ..., MHARTID: 32-bit words in the monitor's order. */
    const size_t words = sizeof(RvExcFrame) / sizeof(uint32_t);
    const size_t count = words < FURI_HAL_DIAG_REGS_MAX ? words : FURI_HAL_DIAG_REGS_MAX;
    memcpy(panic->regs, frame, count * sizeof(uint32_t));
    panic->regs_count = (uint8_t)count;

    /* No on-device unwinding on RISC-V (the monitor decodes the stack dump with
     * the ELF): keep the faulting PC and the return address. */
    panic->backtrace[0] = frame->mepc;
    panic->backtrace[1] = frame->ra;
    panic->backtrace_depth = 2;
    panic->backtrace_end = 1;
#endif
}

static void FURI_HAL_DIAG_PANIC_ATTR
    furi_hal_diag_capture(const panic_info_t* info, uint32_t core) {
    FuriHalDiagRtcSlot* slot = &furi_hal_diag_rtc_slot[core];
    FuriHalDiagPanic* panic = &furi_hal_diag_rtc_scratch[core];
    const uint32_t seq = furi_hal_diag_next_seq();

    /* Step 1: only panic_info_t, the abort message and constants. Committed at
     * once, so even if step 2 faults the crash cause is saved. */
    memset(panic, 0, sizeof(*panic));
    panic->core = (uint8_t)core;
    panic->pc = (uint32_t)info->addr;
    panic->uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    furi_hal_diag_copy_str(panic->fw_version, sizeof(panic->fw_version), FURI_ESP32_VERSION);

    if(g_panic_abort) {
        panic->exception = FuriHalDiagExceptionAbort;
        if(furi_hal_diag_copy_str(
               panic->details, sizeof(panic->details), g_panic_abort_details)) {
            panic->truncated |= FURI_HAL_DIAG_TRUNC_DETAILS;
        }
    } else {
        switch(info->exception) {
        case PANIC_EXCEPTION_DEBUG:
            panic->exception = FuriHalDiagExceptionDebug;
            break;
        case PANIC_EXCEPTION_IWDT:
            panic->exception = FuriHalDiagExceptionIntWdt;
            break;
        case PANIC_EXCEPTION_TWDT:
            panic->exception = FuriHalDiagExceptionTaskWdt;
            break;
        case PANIC_EXCEPTION_ABORT:
            panic->exception = FuriHalDiagExceptionAbort;
            break;
        default:
            panic->exception = FuriHalDiagExceptionFault;
            break;
        }
        if(furi_hal_diag_copy_str(panic->reason, sizeof(panic->reason), info->reason)) {
            panic->truncated |= FURI_HAL_DIAG_TRUNC_REASON;
        }
        if(furi_hal_diag_copy_str(
               panic->description, sizeof(panic->description), info->description)) {
            panic->truncated |= FURI_HAL_DIAG_TRUNC_DESCRIPTION;
        }
    }
    panic->stage = FuriHalDiagStageBasic;
    furi_hal_diag_commit(slot, panic, seq);

    /* Step 2: reads through pointers (ELF hash, TCB, exception frame, stack).
     * These are the same reads ESP-IDF does to print its report. */
    esp_app_get_elf_sha256(panic->elf_sha256, 17);
    furi_hal_diag_capture_task(panic);
    if(info->frame) furi_hal_diag_capture_frame(panic, info->frame);
    panic->stage = FuriHalDiagStageComplete;
    furi_hal_diag_commit(slot, panic, seq);
}

/* Linked in place of esp_panic_handler() (-Wl,--wrap, see CMakeLists.txt):
 * record the crash, then hand over to ESP-IDF's handler, which prints the
 * usual Guru Meditation report and reboots exactly as before. */
void FURI_HAL_DIAG_PANIC_ATTR __wrap_esp_panic_handler(panic_info_t* info) {
    const uint32_t core = (uint32_t)esp_cpu_get_core_id();
    if(info && core < SOC_CPU_CORES_NUM && !furi_hal_diag_core_busy[core]) {
        furi_hal_diag_core_busy[core] = 1;
        furi_hal_diag_capture(info, core);
    }
    __real_esp_panic_handler(info);
}

#endif /* FURI_HAL_DIAG_WRAP_PANIC */

/* ------------------------------------------------------------------------ */
/* Boot time                                                                */
/* ------------------------------------------------------------------------ */

static bool furi_hal_diag_slot_valid(const FuriHalDiagRtcSlot* slot) {
    return slot->magic == FURI_HAL_DIAG_RTC_MAGIC && slot->crc == furi_hal_diag_slot_crc(slot);
}

static void furi_hal_diag_clear_slots(void) {
    for(size_t core = 0; core < SOC_CPU_CORES_NUM; core++) {
        furi_hal_diag_rtc_slot[core].magic = 0;
    }
}

/* Resets worth a log entry. Power-on, software restart, deep sleep, USB/JTAG
 * (flashing) and the external reset pin are normal. */
static bool furi_hal_diag_is_abnormal(esp_reset_reason_t reason) {
    switch(reason) {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_BROWNOUT:
    case ESP_RST_CPU_LOCKUP:
    case ESP_RST_PWR_GLITCH:
    case ESP_RST_EFUSE:
        return true;
    default:
        return false;
    }
}

static bool furi_hal_diag_nvs_load(FuriHalDiagNvsRecord* record) {
    nvs_handle_t nvs;
    if(nvs_open(FURI_HAL_DIAG_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return false;

    size_t size = sizeof(*record);
    const esp_err_t err = nvs_get_blob(nvs, FURI_HAL_DIAG_NVS_KEY, record, &size);
    nvs_close(nvs);

    return err == ESP_OK && size == sizeof(*record) &&
           record->version == FURI_HAL_DIAG_NVS_VERSION && record->size == sizeof(*record);
}

/* Boot task only: its stack is internal RAM, which the flash write needs (the
 * cache, and with it PSRAM, is off while NVS writes). An NVS entry is replaced
 * atomically: a power cut mid-write leaves the old or the new record. */
static bool furi_hal_diag_nvs_save(const FuriHalDiagNvsRecord* record) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(FURI_HAL_DIAG_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if(err == ESP_OK) {
        err = nvs_set_blob(nvs, FURI_HAL_DIAG_NVS_KEY, record, sizeof(*record));
        if(err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "Unable to save the crash log: %s", esp_err_to_name(err));
    }
    return err == ESP_OK;
}

static uint32_t furi_hal_diag_now(void) {
    const uint32_t now = furi_hal_rtc_get_timestamp();
    return now >= FURI_HAL_DIAG_CLOCK_VALID ? now : 0;
}

static void furi_hal_diag_log_panic(const FuriHalDiagCrash* crash) {
    const FuriHalDiagPanic* panic = &crash->panic;
    const char* cause = panic->reason[0] ? panic->reason : panic->details;
    ESP_LOGW(
        TAG,
        "Crash #%lu saved (%s): %s | task %s, core %u, PC 0x%08lx, addr 0x%08lx, uptime %lus",
        (unsigned long)crash->panic_number,
        furi_hal_diag_get_reset_reason_name(crash->panic_reset_reason),
        cause[0] ? cause : "?",
        panic->task[0] ? panic->task : "?",
        (unsigned)panic->core,
        (unsigned long)panic->pc,
        (unsigned long)panic->fault_addr,
        (unsigned long)panic->uptime_s);
}

void furi_hal_diag_init(void) {
    const esp_reset_reason_t reason = esp_reset_reason();
    furi_hal_diag_reset_reason = (uint8_t)reason;

    /* RTC memory is not retained over a power-on reset: whatever is there is noise. */
    const bool rtc_kept = reason != ESP_RST_POWERON;
    const bool guard_tripped = rtc_kept && furi_hal_diag_rtc_guard == FURI_HAL_DIAG_GUARD_MAGIC;
    furi_hal_diag_rtc_guard = 0;

    if(guard_tripped) {
        /* The last boot crashed inside this function (corrupted NVS?). Skip it
         * once rather than loop; the next boot tries again. */
        ESP_LOGE(TAG, "The crash log step crashed the last boot: skipped this boot");
        furi_hal_diag_clear_slots();
        return;
    }

    /* The crash waiting in RTC memory, if any: the valid slot captured first. */
    FuriHalDiagRtcSlot pending;
    bool have_pending = false;
    size_t pending_core = 0;
    if(rtc_kept) {
        for(size_t core = 0; core < SOC_CPU_CORES_NUM; core++) {
            const FuriHalDiagRtcSlot* slot = &furi_hal_diag_rtc_slot[core];
            if(furi_hal_diag_slot_valid(slot) && (!have_pending || slot->seq < pending.seq)) {
                pending = *slot;
                pending_core = core;
                have_pending = true;
            }
        }
    }

    if(have_pending && pending.reset_reason == FURI_HAL_DIAG_REASON_UNSET) {
        /* First boot after the crash: remember how it reset, so a save retried
         * on a later boot still reports the right reason. */
        FuriHalDiagRtcSlot* slot = &furi_hal_diag_rtc_slot[pending_core];
        slot->magic = 0;
        slot->reset_reason = reason;
        slot->crc = furi_hal_diag_slot_crc(slot);
        slot->magic = FURI_HAL_DIAG_RTC_MAGIC;
        pending.reset_reason = reason;
    }

    furi_hal_diag_rtc_guard = FURI_HAL_DIAG_GUARD_MAGIC;

    /* ~1.2 KB on the boot task's (internal) stack: nvs_set_blob() reads the
     * record while the cache is off. */
    FuriHalDiagNvsRecord record;
    const bool have_saved = furi_hal_diag_nvs_load(&record);
    if(!have_saved) memset(&record, 0, sizeof(record));
    FuriHalDiagCrash* crash = &record.crash;
    bool dirty = false;

    if(have_pending) {
        const uint32_t id =
            esp_rom_crc32_le(0, (const uint8_t*)&pending.panic, sizeof(pending.panic));
        if(!(crash->has_panic && crash->panic_id == id)) {
            crash->count++;
            crash->has_panic = 1;
            crash->panic_reset_reason = (uint8_t)pending.reset_reason;
            crash->panic_number = crash->count;
            crash->panic_timestamp = furi_hal_diag_now();
            crash->panic_id = id;
            crash->panic = pending.panic;
            dirty = true;
        }
    } else if(furi_hal_diag_is_abnormal(reason)) {
        /* No panic record: brownout, hardware watchdog, ... Logged apart so the
         * last panic's details stay. */
        crash->count++;
        crash->has_other = 1;
        crash->other_reset_reason = (uint8_t)reason;
        crash->other_number = crash->count;
        crash->other_timestamp = furi_hal_diag_now();
        dirty = true;
    }

    bool saved = true;
    if(dirty) {
        record.version = FURI_HAL_DIAG_NVS_VERSION;
        record.size = sizeof(record);
        saved = furi_hal_diag_nvs_save(&record);
    }

    furi_hal_diag_rtc_guard = 0;

    if(saved) {
        /* NVS has it (or already had it): never read it again. */
        furi_hal_diag_clear_slots();
    } else {
        ESP_LOGE(TAG, "Crash log kept in RTC memory, saved at the next boot");
    }

    if(have_saved || dirty) {
        /* Shown in About even when the save failed this boot. */
        furi_hal_diag_history = *crash;
        furi_hal_diag_have_history = true;
    }

    if(dirty && crash->has_panic && crash->panic_number == crash->count) {
        furi_hal_diag_log_panic(crash);
    } else if(dirty) {
        ESP_LOGW(
            TAG,
            "Reset #%lu saved: %s (no panic details)",
            (unsigned long)crash->other_number,
            furi_hal_diag_get_reset_reason_name(crash->other_reset_reason));
    }
}

uint64_t furi_hal_diag_get_uptime_us(void) {
    return (uint64_t)esp_timer_get_time();
}

uint8_t furi_hal_diag_get_reset_reason(void) {
    return furi_hal_diag_reset_reason;
}

const char* furi_hal_diag_get_reset_reason_name(uint8_t reason) {
    switch((esp_reset_reason_t)reason) {
    case ESP_RST_POWERON:
        return "Power-on";
    case ESP_RST_EXT:
        return "External pin";
    case ESP_RST_SW:
        return "Software restart";
    case ESP_RST_PANIC:
        return "Panic (crash)";
    case ESP_RST_INT_WDT:
        return "Interrupt WDT";
    case ESP_RST_TASK_WDT:
        return "Task WDT";
    case ESP_RST_WDT:
        return "Watchdog";
    case ESP_RST_DEEPSLEEP:
        return "Deep sleep wake";
    case ESP_RST_BROWNOUT:
        return "Brownout";
    case ESP_RST_SDIO:
        return "SDIO";
    case ESP_RST_USB:
        return "USB";
    case ESP_RST_JTAG:
        return "JTAG";
    case ESP_RST_EFUSE:
        return "eFuse error";
    case ESP_RST_PWR_GLITCH:
        return "Power glitch";
    case ESP_RST_CPU_LOCKUP:
        return "CPU lockup";
    default:
        return "Unknown";
    }
}

bool furi_hal_diag_get_last_crash(FuriHalDiagCrash* crash) {
    furi_check(crash);
    if(!furi_hal_diag_have_history) return false;
    *crash = furi_hal_diag_history;
    return true;
}

/* ------------------------------------------------------------------------ */
/* Text (About > Diagnostics and the "crashlog" CLI command)                */
/* ------------------------------------------------------------------------ */

#if __XTENSA__
#define FURI_HAL_DIAG_ADDR2LINE "xtensa-" CONFIG_IDF_TARGET "-elf-addr2line"
#else
#define FURI_HAL_DIAG_ADDR2LINE "riscv32-esp-elf-addr2line"
#endif

/* Register names in the order FuriHalDiagPanic.regs[] holds them (= the monitor's). */
static const char* const furi_hal_diag_reg_names[] = {
#if __XTENSA__
    "PC",  "PS",  "A0",  "A1",  "A2",  "A3",  "A4",       "A5",       "A6",
    "A7",  "A8",  "A9",  "A10", "A11", "A12", "A13",      "A14",      "A15",
    "SAR", "EXCCAUSE", "EXCVADDR", "LBEG", "LEND", "LCOUNT",
#else
    "MEPC", "RA",  "SP",  "GP",  "TP",  "T0",  "T1",      "T2",     "S0/FP",
    "S1",   "A0",  "A1",  "A2",  "A3",  "A4",  "A5",      "A6",     "A7",
    "S2",   "S3",  "S4",  "S5",  "S6",  "S7",  "S8",      "S9",     "S10",
    "S11",  "T3",  "T4",  "T5",  "T6",  "MSTATUS", "MTVEC", "MCAUSE", "MTVAL",
    "MHARTID",
#endif
};

static void furi_hal_diag_cat_duration(FuriString* out, uint32_t seconds) {
    furi_string_cat_printf(
        out,
        "%lud %02luh %02lum %02lus",
        (unsigned long)(seconds / 86400),
        (unsigned long)((seconds / 3600) % 24),
        (unsigned long)((seconds / 60) % 60),
        (unsigned long)(seconds % 60));
}

static void furi_hal_diag_cat_saved_time(FuriString* out, uint32_t timestamp) {
    if(!timestamp) {
        furi_string_cat(out, "Saved: (clock not set)\n");
        return;
    }
    DateTime dt;
    datetime_timestamp_to_datetime(timestamp, &dt);
    furi_string_cat_printf(
        out,
        "Saved: %04u-%02u-%02u %02u:%02u:%02u\n",
        (unsigned)dt.year,
        (unsigned)dt.month,
        (unsigned)dt.day,
        (unsigned)dt.hour,
        (unsigned)dt.minute,
        (unsigned)dt.second);
}

/* The newest entry of the history comes first. */
static bool furi_hal_diag_panic_is_latest(const FuriHalDiagCrash* crash) {
    return crash->has_panic && (!crash->has_other || crash->panic_number > crash->other_number);
}

static void furi_hal_diag_panic_summary(const FuriHalDiagPanic* panic, char* buffer, size_t size) {
    switch(panic->exception) {
    case FuriHalDiagExceptionAbort: {
        if(strstr(panic->details, "stack overflow")) {
            strlcpy(buffer, "Stack overflow", size);
            return;
        }
        /* "furi_check failed [GUI] at PC ..." -> "furi_check failed",
         * "assert failed: ..." -> "assert failed",
         * "abort() was called at PC ..." -> "abort() was called". */
        size_t len = strlen(panic->details);
        const char* stops[] = {" [", ":", " at PC"};
        for(size_t i = 0; i < COUNT_OF(stops); i++) {
            const char* stop = strstr(panic->details, stops[i]);
            if(stop && (size_t)(stop - panic->details) < len) len = stop - panic->details;
        }
        if(len == 0) {
            strlcpy(buffer, "Abort", size);
        } else {
            if(len > size - 1) len = size - 1;
            memcpy(buffer, panic->details, len);
            buffer[len] = '\0';
        }
        return;
    }
    case FuriHalDiagExceptionIntWdt:
        strlcpy(buffer, "Interrupt WDT", size);
        return;
    case FuriHalDiagExceptionTaskWdt:
        strlcpy(buffer, "Task WDT", size);
        return;
    default:
        strlcpy(buffer, panic->reason[0] ? panic->reason : "Exception", size);
        return;
    }
}

void furi_hal_diag_format_summary(const FuriHalDiagCrash* crash, char* buffer, size_t size) {
    furi_check(crash && buffer && size);
    if(furi_hal_diag_panic_is_latest(crash)) {
        furi_hal_diag_panic_summary(&crash->panic, buffer, size);
    } else if(crash->has_other) {
        strlcpy(buffer, furi_hal_diag_get_reset_reason_name(crash->other_reset_reason), size);
    } else {
        strlcpy(buffer, "None", size);
    }
}

static void furi_hal_diag_cat_panic(FuriString* out, const FuriHalDiagCrash* crash) {
    const FuriHalDiagPanic* panic = &crash->panic;

    furi_string_cat_printf(out, "== Crash #%lu ==\n", (unsigned long)crash->panic_number);
    furi_string_cat_printf(
        out, "Reset: %s\n", furi_hal_diag_get_reset_reason_name(crash->panic_reset_reason));
    furi_hal_diag_cat_saved_time(out, crash->panic_timestamp);
    furi_string_cat_printf(
        out,
        "FW: %s\nELF: %s\nUptime: ",
        panic->fw_version[0] ? panic->fw_version : "?",
        panic->elf_sha256[0] ? panic->elf_sha256 : "?");
    furi_hal_diag_cat_duration(out, panic->uptime_s);
    furi_string_cat(out, "\n");

    if(panic->stage != FuriHalDiagStageComplete) {
        furi_string_cat(
            out,
            "(Partial record: reading the task/stack faulted during the crash. "
            "The cause below is complete.)\n");
    }
    furi_string_cat(out, "\n");

    if(panic->exception == FuriHalDiagExceptionAbort) {
        furi_string_cat_printf(
            out,
            "Abort on core %u:\n%s%s\n",
            (unsigned)panic->core,
            panic->details[0] ? panic->details : "abort() was called",
            (panic->truncated & FURI_HAL_DIAG_TRUNC_DETAILS) ? "..." : "");
    } else {
        furi_string_cat_printf(
            out,
            "Guru Meditation Error: Core %u panic'ed (%s%s). %s%s\n",
            (unsigned)panic->core,
            panic->reason[0] ? panic->reason : "Unknown",
            (panic->truncated & FURI_HAL_DIAG_TRUNC_REASON) ? "..." : "",
            panic->description,
            (panic->truncated & FURI_HAL_DIAG_TRUNC_DESCRIPTION) ? "..." : "");
    }
    furi_string_cat_printf(out, "Task: %s\n", panic->task[0] ? panic->task : "?");

    if(panic->regs_count) {
        furi_string_cat_printf(out, "\nCore %u register dump:\n", (unsigned)panic->core);
        for(size_t i = 0; i < panic->regs_count && i < FURI_HAL_DIAG_REGS_MAX &&
                          i < COUNT_OF(furi_hal_diag_reg_names);
            i++) {
            furi_string_cat_printf(
                out,
                "%s: 0x%08lx\n",
                furi_hal_diag_reg_names[i],
                (unsigned long)panic->regs[i]);
        }
    } else {
        furi_string_cat_printf(out, "PC: 0x%08lx\n", (unsigned long)panic->pc);
    }

#if __XTENSA__
    furi_string_cat(out, "\nBacktrace:\n");
#else
    furi_string_cat(out, "\nPC, RA:\n");
#endif
    for(size_t i = 0; i < panic->backtrace_depth && i < FURI_HAL_DIAG_BACKTRACE_MAX; i++) {
        furi_string_cat_printf(out, "0x%08lx\n", (unsigned long)panic->backtrace[i]);
    }
    if(panic->backtrace_end == 2) {
        furi_string_cat(out, "|<-CORRUPTED\n");
    } else if(panic->backtrace_end == 1) {
        furi_string_cat(out, "|<-CONTINUES\n");
    }

    furi_string_cat_printf(
        out,
        "\nDecode with the .elf of this build (ELF %s):\n" FURI_HAL_DIAG_ADDR2LINE
        " -pfiaC -e furi_esp32.elf ADDRESS\n",
        panic->elf_sha256[0] ? panic->elf_sha256 : "?");
}

static void furi_hal_diag_cat_other(FuriString* out, const FuriHalDiagCrash* crash) {
    furi_string_cat_printf(out, "== Reset #%lu ==\n", (unsigned long)crash->other_number);
    furi_string_cat_printf(
        out, "Reset: %s\n", furi_hal_diag_get_reset_reason_name(crash->other_reset_reason));
    furi_hal_diag_cat_saved_time(out, crash->other_timestamp);
    furi_string_cat(
        out,
        "No panic details: this reset does not go through the panic handler "
        "(supply dip, hardware watchdog, ...).\n");
}

void furi_hal_diag_format_log(const FuriHalDiagCrash* crash, FuriString* out) {
    furi_check(crash && out);
    furi_string_printf(out, "Abnormal resets: %lu\n\n", (unsigned long)crash->count);
    if(furi_hal_diag_panic_is_latest(crash)) {
        furi_hal_diag_cat_panic(out, crash);
        if(crash->has_other) {
            furi_string_cat(out, "\n");
            furi_hal_diag_cat_other(out, crash);
        }
    } else {
        if(crash->has_other) furi_hal_diag_cat_other(out, crash);
        if(crash->has_panic) {
            furi_string_cat(out, "\n");
            furi_hal_diag_cat_panic(out, crash);
        }
    }
}

void furi_hal_diag_test_crash(FuriHalDiagTestCrash kind) {
    switch(kind) {
    case FuriHalDiagTestCrashAbort:
        abort();
    case FuriHalDiagTestCrashFuriCheck:
        furi_check(false, "crashlog test");
        break;
    default:
        /* Store to address 0: StoreProhibited (Xtensa) / store access fault (RISC-V). */
        *(volatile uint32_t*)0 = 0xDEAD;
        break;
    }
    abort();
}
