/**
 * @file check.h
 *
 * Furi crash and assert functions.
 *
 * ESP32 port: ARM-specific register tricks replaced with simple parameter passing.
 */
#pragma once

#include <m-core.h>
#include "common_defines.h"

#ifdef __cplusplus
extern "C" {
#endif

// Flags instead of pointers will save ~4 bytes on furi_assert and furi_check calls.
#define __FURI_ASSERT_MESSAGE_FLAG (0x01)
#define __FURI_CHECK_MESSAGE_FLAG  (0x02)

/** Crash system */
FURI_NORETURN void __furi_crash_implementation(const char* message);

/** Halt system */
FURI_NORETURN void __furi_halt_implementation(const char* message);

/** Crash system with message. */
#define __furi_crash(message)                        \
    do {                                             \
        __furi_crash_implementation((const char*)(message)); \
    } while(0)

/** Crash system
 *
 * @param      ... optional  message (const char*)
 */
#define furi_crash(...) M_APPLY(__furi_crash, M_IF_EMPTY(__VA_ARGS__)((NULL), (__VA_ARGS__)))

/** Halt system with message. */
#define __furi_halt(message)                        \
    do {                                            \
        __furi_halt_implementation((const char*)(message)); \
    } while(0)

/** Halt system
 *
 * @param      ... optional  message (const char*)
 */
#define furi_halt(...) M_APPLY(__furi_halt, M_IF_EMPTY(__VA_ARGS__)((NULL), (__VA_ARGS__)))

/** Check condition and crash if check failed */
#define __furi_check(__e, __m) \
    do {                       \
        if(!(__e)) {           \
            __furi_crash(__m); \
        }                      \
    } while(0)

/** Check condition and crash if failed
 *
 * @param      ... condition to check and optional  message (const char*)
 */
#define furi_check(...) \
    M_APPLY(__furi_check, M_DEFAULT_ARGS(2, (__FURI_CHECK_MESSAGE_FLAG), __VA_ARGS__))

/** Only in debug build: Assert condition and crash if assert failed  */
#ifdef FURI_DEBUG
#define __furi_assert(__e, __m) \
    do {                        \
        if(!(__e)) {            \
            __furi_crash(__m);  \
        }                       \
    } while(0)
#else
#define __furi_assert(__e, __m) \
    do {                        \
        ((void)(__e));          \
        ((void)(__m));          \
    } while(0)
#endif

/** Assert condition and crash if failed
 *
 * @warning    only will do check if firmware compiled in debug mode
 *
 * @param      ... condition to check and optional  message (const char*)
 */
#define furi_assert(...) \
    M_APPLY(__furi_assert, M_DEFAULT_ARGS(2, (__FURI_ASSERT_MESSAGE_FLAG), __VA_ARGS__))

#define furi_break(__e)               \
    do {                              \
        if(!(__e)) {                  \
            __builtin_trap();         \
        }                             \
    } while(0)

#ifdef __cplusplus
}
#endif
