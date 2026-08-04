#include "../drivers/engine/driver.h"

#include <stdio.h>
#include <string.h>

extern const WmbusDriver wmbus_drv_apator_hca;

const WmbusDriver* const wmbus_engine_registry[] = { &wmbus_drv_apator_hca };
const size_t wmbus_engine_registry_len = 1;

#define MANUF(a, b, c) \
    ((uint16_t)((((uint16_t)((a) - '@')) << 10) | \
                (((uint16_t)((b) - '@')) << 5) | \
                ((uint16_t)((c) - '@'))))

static int expect_contains(const char* name, const char* out, const char* expected) {
    if(strstr(out, expected)) return 0;
    fprintf(stderr, "%s: missing '%s' in:\n%s\n", name, expected, out);
    return 1;
}

static void decode(const uint8_t* apdu, size_t len, char* out, size_t cap) {
    wmbus_engine_decode(MANUF('A', 'P', 'A'), 0x04, 0x08, apdu, len, out, cap);
}

/* wmbusmeters apatoreitn.xmq test HCA1: bare 15-byte frame after
 * CI=0xA0. Expected: current 1, previous 89, date 2022-09-18,
 * season start 2016-05-01, seal broken 2019-08-28,
 * temp prev 19.890625, temp avg 21.703125. */
static int run_upstream_direct(void) {
    const uint8_t apdu[] = {
        0xA1, 0x00, 0x00, 0x59, 0x00, 0x1C, 0x27, 0x01,
        0x00, 0x32, 0x2D, 0xE4, 0x13, 0xB4, 0x15,
    };
    char out[256];
    int fails = 0;

    decode(apdu, sizeof(apdu), out, sizeof(out));

    fails += expect_contains("direct", out, "Apator HCA\n");
    fails += expect_contains("direct", out, "Current 1\n");
    fails += expect_contains("direct", out, "Previous 89\n");
    fails += expect_contains("direct", out, "Date 2022-09-18\n");
    fails += expect_contains("direct", out, "Season 2016-05-01\n");
    fails += expect_contains("direct", out, "Seal BROKEN\n");
    fails += expect_contains("direct", out, "ESBDate 2019-08-28\n");
    fails += expect_contains("direct", out, "TempPrev 19.891 C\n");
    fails += expect_contains("direct", out, "TempAvg 21.703 C\n");

    return fails;
}

/* Same frame wrapped in a leading 0xA0 marker (upstream
 * frame_direct_with_a0 variant). */
static int run_upstream_a0_prefixed(void) {
    const uint8_t apdu[] = {
        0xA0,
        0xA1, 0x00, 0x00, 0x59, 0x00, 0x1C, 0x27, 0x01,
        0x00, 0x32, 0x2D, 0xE4, 0x13, 0xB4, 0x15,
    };
    char out[256];
    int fails = 0;

    decode(apdu, sizeof(apdu), out, sizeof(out));

    fails += expect_contains("a0", out, "Current 1\n");
    fails += expect_contains("a0", out, "Previous 89\n");
    fails += expect_contains("a0", out, "Date 2022-09-18\n");

    return fails;
}

/* wmbusmeters apatoreitn.xmq test HCA2: CI=0xB6 wrapper — skip
 * count 0x0A, ten bytes, 0xA0 marker, then the direct frame.
 * Expected: current 0, previous 2424, date 2022-08-31, season
 * start 2016-05-01, seal OK (esb null), temp prev 22.390625,
 * temp avg 25.78125. */
static int run_upstream_b6(void) {
    const uint8_t apdu[] = {
        0x0A, 0xFF, 0xFF, 0xF5, 0x45, 0x01, 0x86, 0xF4,
        0x1B, 0x9D, 0x58, 0xA0,
        0xA1, 0x00, 0x00, 0x78, 0x09, 0x00, 0x00, 0x00,
        0x00, 0x1F, 0x2D, 0x64, 0x16, 0xC8, 0x19,
    };
    char out[256];
    int fails = 0;

    decode(apdu, sizeof(apdu), out, sizeof(out));

    fails += expect_contains("b6", out, "Current 0\n");
    fails += expect_contains("b6", out, "Previous 2424\n");
    fails += expect_contains("b6", out, "Date 2022-08-31\n");
    fails += expect_contains("b6", out, "Season 2016-05-01\n");
    fails += expect_contains("b6", out, "Seal OK\n");
    fails += expect_contains("b6", out, "TempPrev 22.391 C\n");
    fails += expect_contains("b6", out, "TempAvg 25.781 C\n");

    return fails;
}

/* Field-verified live capture (APDU only, no meter ID): verified
 * against wmbusmeters 3.0.0 analyze and the on-meter display
 * (display shows TempPrev). Season lo 0x21 -> 2016-01-01. */
static int run_live_sample(void) {
    const uint8_t apdu[] = {
        0x21, 0x01, 0x00, 0xD5, 0x19, 0x00, 0x00, 0xD8,
        0x28, 0xE8, 0x34, 0x94, 0x15, 0x98, 0x16,
    };
    char out[256];
    int fails = 0;

    decode(apdu, sizeof(apdu), out, sizeof(out));

    fails += expect_contains("live", out, "Current 10456\n");
    fails += expect_contains("live", out, "Previous 6613\n");
    fails += expect_contains("live", out, "Date 2026-07-08\n");
    fails += expect_contains("live", out, "Season 2016-01-01\n");
    fails += expect_contains("live", out, "Seal OK\n");
    fails += expect_contains("live", out, "TempPrev 21.578 C\n");
    fails += expect_contains("live", out, "TempAvg 22.594 C\n");

    return fails;
}

static int run_bad_length(void) {
    const uint8_t apdu[] = {0x21, 0x01, 0x00};
    char out[256];
    int fails = 0;

    decode(apdu, sizeof(apdu), out, sizeof(out));

    fails += expect_contains("bad_length", out, "Apator HCA\n");
    fails += expect_contains("bad_length", out, "Bytes 3\n");
    fails += expect_contains("bad_length", out, "Status bad length\n");
    fails += expect_contains("bad_length", out, "Hex00 210100\n");

    return fails;
}

int main(void) {
    int fails = 0;

    fails += run_upstream_direct();
    fails += run_upstream_a0_prefixed();
    fails += run_upstream_b6();
    fails += run_live_sample();
    fails += run_bad_length();

    if(fails) return 1;
    printf("All Apator HCA tests passed.\n");
    return 0;
}
