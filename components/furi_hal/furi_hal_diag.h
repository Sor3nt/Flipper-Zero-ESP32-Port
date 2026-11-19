/**
 * @file furi_hal_diag.h
 * Uptime, reset reason and the last crash log (Settings -> About -> Diagnostics).
 *
 * Crash capture works in two steps, so nothing touches the flash while the system
 * is panicking:
 *  1. Panic: a wrapper around ESP-IDF's esp_panic_handler() copies the cause
 *     (exception, PC, fault address, registers, task, backtrace, abort message)
 *     into RTC memory (RTC_NOINIT_ATTR): plain RAM writes, no lock, no flash, no
 *     allocation. RTC memory survives the panic reset; ESP-IDF's own reset reason
 *     tests rely on that for panics, aborts, esp_restart() and watchdogs.
 *  2. Next boot: furi_hal_diag_init() (boot task, internal stack) moves the record
 *     to NVS so it also survives a power cut. The RTC copy is cleared only once
 *     NVS has it, so a failed save is retried on the following boot. Resets that
 *     skip the panic handler (brownout, hardware watchdog) are logged too, with
 *     the reset reason only, and kept apart from the panic details.
 * The record is read once at boot into static RAM: apps never touch NVS for it
 * (on the S3 boards app stacks live in PSRAM, which must not run flash code).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FURI_HAL_DIAG_BACKTRACE_MAX 16
#define FURI_HAL_DIAG_REGS_MAX      40

typedef enum {
    FuriHalDiagExceptionNone = 0,
    FuriHalDiagExceptionFault, /**< CPU exception: LoadProhibited, access fault, ... */
    FuriHalDiagExceptionAbort, /**< abort(), assert, furi_check/furi_crash, stack overflow */
    FuriHalDiagExceptionIntWdt, /**< interrupt watchdog */
    FuriHalDiagExceptionTaskWdt, /**< task watchdog (only when it is set to panic) */
    FuriHalDiagExceptionDebug, /**< debug exception: stack guard, watchpoint */
} FuriHalDiagException;

typedef enum {
    /** Cause, PC, uptime and messages only: the second step (task, registers,
     *  stack walk) faulted. Never seen unless the stack/TCB is badly corrupted. */
    FuriHalDiagStageBasic = 1,
    FuriHalDiagStageComplete = 2,
} FuriHalDiagStage;

#define FURI_HAL_DIAG_TRUNC_REASON      (1u << 0)
#define FURI_HAL_DIAG_TRUNC_DESCRIPTION (1u << 1)
#define FURI_HAL_DIAG_TRUNC_DETAILS     (1u << 2)

/** What the panic handler saw. Fixed layout, a multiple of 4 bytes: it is stored
 *  in RTC memory and NVS. Change it -> bump the magic/version in the .c file. */
typedef struct {
    uint8_t exception; /**< FuriHalDiagException */
    uint8_t core; /**< CPU core that panicked */
    uint8_t stage; /**< FuriHalDiagStage */
    uint8_t truncated; /**< FURI_HAL_DIAG_TRUNC_* : text that did not fit */
    uint8_t backtrace_depth; /**< valid entries in backtrace[] */
    uint8_t backtrace_end; /**< 0 = complete, 1 = more frames, 2 = corrupted stack */
    uint8_t regs_count; /**< valid entries in regs[] */
    uint8_t reserved;
    uint32_t uptime_s; /**< uptime when it crashed */
    uint32_t pc; /**< address of the faulting instruction */
    uint32_t fault_addr; /**< EXCVADDR (Xtensa) / MTVAL (RISC-V) */
    uint32_t cause; /**< EXCCAUSE (Xtensa) / MCAUSE (RISC-V) */
    uint32_t sp; /**< stack pointer at the fault */
    /** Xtensa: PC of each stack frame, faulting one first (the monitor's
     *  "Backtrace:"). RISC-V: [0] = PC, [1] = RA (return address). */
    uint32_t backtrace[FURI_HAL_DIAG_BACKTRACE_MAX];
    /** Register dump in the monitor's order. Xtensa: PC PS A0..A15 SAR EXCCAUSE
     *  EXCVADDR LBEG LEND LCOUNT. RISC-V: MEPC RA SP GP TP T0-T2 S0 S1 A0-A7
     *  S2-S11 T3-T6 MSTATUS MTVEC MCAUSE MTVAL MHARTID. */
    uint32_t regs[FURI_HAL_DIAG_REGS_MAX];
    char task[16]; /**< task running on that core */
    char fw_version[12]; /**< firmware that crashed */
    char elf_sha256[20]; /**< its ELF SHA256, first 16 hex digits (match the .elf to decode) */
    char reason[48]; /**< e.g. "LoadProhibited", "Interrupt wdt timeout on CPU0" */
    char description[64]; /**< e.g. "Exception was unhandled." */
    char details[192]; /**< abort message: "furi_check failed [GUI] at PC 0x...", assert text */
} FuriHalDiagPanic;

/** Saved crash history (NVS), as read at boot. */
typedef struct {
    uint32_t count; /**< abnormal resets recorded so far */

    /** Last panic, complete. Not overwritten by later resets without details. */
    uint8_t has_panic;
    uint8_t panic_reset_reason; /**< esp_reset_reason_t of the boot after it */
    uint8_t reserved0[2];
    uint32_t panic_number; /**< count when it was recorded (orders it vs "other") */
    uint32_t panic_timestamp; /**< RTC time it was saved, ~1 s after the crash; 0 = clock not set */
    uint32_t panic_id; /**< CRC of the record: the same crash is never saved twice */
    FuriHalDiagPanic panic;

    /** Last abnormal reset that skipped the panic handler (brownout, hardware
     *  watchdog, ...): only the reason is known. */
    uint8_t has_other;
    uint8_t other_reset_reason;
    uint8_t reserved1[2];
    uint32_t other_number;
    uint32_t other_timestamp;
} FuriHalDiagCrash;

/** Boot step: move a crash record from RTC memory to NVS and load the history.
 *  Call once from the boot task after nvs_flash_init() and furi_hal_rtc_init(). */
void furi_hal_diag_init(void);

/** Microseconds since boot (esp_timer_get_time: lock-free, cheap). */
uint64_t furi_hal_diag_get_uptime_us(void);

/** Reset reason of the current boot (esp_reset_reason_t). */
uint8_t furi_hal_diag_get_reset_reason(void);

/** Short readable name of an esp_reset_reason_t value, e.g. "Power-on" (never NULL). */
const char* furi_hal_diag_get_reset_reason_name(uint8_t reason);

/** Copy of the saved crash history. @return false when nothing was ever saved. */
bool furi_hal_diag_get_last_crash(FuriHalDiagCrash* crash);

typedef struct FuriString FuriString;

/** Short name of the newest history entry, e.g. "LoadProhibited", "furi_check
 *  failed", "Brownout". Always terminated. */
void furi_hal_diag_format_summary(const FuriHalDiagCrash* crash, char* buffer, size_t size);

/** The whole history, newest first, in the style of the serial monitor's panic
 *  report ('\n' line ends). Replaces the content of out. */
void furi_hal_diag_format_log(const FuriHalDiagCrash* crash, FuriString* out);

typedef enum {
    FuriHalDiagTestCrashFault, /**< store to address 0: CPU exception */
    FuriHalDiagTestCrashAbort, /**< abort() */
    FuriHalDiagTestCrashFuriCheck, /**< furi_check(false) */
} FuriHalDiagTestCrash;

/** Crash on purpose, to check the crash log end to end (CLI "crashlog test").
 *  Does not return. */
void furi_hal_diag_test_crash(FuriHalDiagTestCrash kind);

#ifdef __cplusplus
}
#endif
