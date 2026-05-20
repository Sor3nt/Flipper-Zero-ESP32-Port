#include "furi_hal_version.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include <esp_mac.h>

/* -------------------------------------------------------------------------
 * Deterministic device name from MAC address
 *
 * Generates a unique, pronounceable 5-7 character name using a Linear
 * Congruential Generator (LCG) seeded from the last 4 bytes of the
 * factory MAC (esp_read_mac with ESP_MAC_WIFI_STA).  Every device gets
 * a stable name at first boot without any user interaction, matching
 * the original Flipper Zero behaviour.
 *
 * The user can still override it through Settings → Desktop → Name
 * (stored in /dolphin/name.settings).
 * -------------------------------------------------------------------------*/

static const char s_consonants[] = "bcdfghjklmnpqrstvwxz";
static const char s_vowels[]     = "aeiou";
#define N_CONS  (sizeof(s_consonants) - 1)
#define N_VOW   (sizeof(s_vowels) - 1)

/* standard glibc LCG constants */
#define LCG_MUL  1103515245u
#define LCG_INC  12345u
#define LCG_MASK 0x7fffffffu
#define LCG_NEXT(s)  ((s) = ((s) * LCG_MUL + LCG_INC) & LCG_MASK)

/* Generate a pronounceable name by alternating consonant/vowel using an
 * LCG seeded from the MAC.  Written once – subsequent calls return the
 * cached result. */
static const char* generate_deterministic_name(void) {
    static char cached[FURI_HAL_VERSION_ARRAY_NAME_LENGTH];
    static bool done = false;
    if(done) return cached;

    uint8_t mac[6] = {0};
    if(esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        /* If MAC read fails, fall back to eFuse default */
        esp_efuse_mac_get_default(mac);
    }

    /* Seed: last 4 bytes of the MAC => 32-bit */
    uint32_t seed = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) |
                    ((uint32_t)mac[4] <<  8) |  (uint32_t)mac[5];
    if(seed == 0) seed = 0xDEADBEEF;  /* guard against all-zero MAC */

    uint32_t lcg = seed;
    int len = 5 + (int)(seed % 3);            /* 5, 6, or 7 */
    if(len >= (int)sizeof(cached)) len = (int)sizeof(cached) - 1;

    int pos = 0;
    for(int i = 0; i < len; i++) {
        LCG_NEXT(lcg);
        if(i % 2 == 0) {
            cached[pos++] = s_consonants[lcg % N_CONS];
        } else {
            cached[pos++] = s_vowels[lcg % N_VOW];
        }
    }
    cached[0] = (char)(cached[0] - 0x20);     /* capitalise */
    cached[pos] = '\0';
    done = true;
    return cached;
}

#undef LCG_NEXT
#undef LCG_MASK
#undef LCG_INC
#undef LCG_MUL

typedef struct {
    char name[FURI_HAL_VERSION_ARRAY_NAME_LENGTH];
    char device_name[FURI_HAL_VERSION_DEVICE_NAME_LENGTH];
    char ble_local_name[FURI_HAL_BT_ADV_NAME_LENGTH + 1];
    uint8_t ble_mac[6];
    uint8_t uid[6];
} FuriHalVersionState;

static FuriHalVersionState furi_hal_version = {
    .name = "ESP32",
    .device_name = "Furi ESP32",
    .ble_local_name = "ESP32",
    .ble_mac = {0},
    .uid = {0},
};

static void furi_hal_version_refresh_names(const char* name) {
    snprintf(furi_hal_version.name, sizeof(furi_hal_version.name), "%s", name);
    snprintf(
        furi_hal_version.device_name,
        sizeof(furi_hal_version.device_name),
        "Furi %s",
        furi_hal_version.name);
    snprintf(
        furi_hal_version.ble_local_name,
        sizeof(furi_hal_version.ble_local_name),
        "%s",
        furi_hal_version.name);

    version_set_custom_name((Version*)version_get(), furi_hal_version.name);
}

void furi_hal_version_init(void) {
    /* Read factory MAC from eFuse — used as hardware UID and BLE address. */
    esp_efuse_mac_get_default(furi_hal_version.uid);
    memcpy(furi_hal_version.ble_mac, furi_hal_version.uid, sizeof(furi_hal_version.uid));

    /* Determine effective name:
     *   1. If a custom name was already loaded (e.g., from namechanger), use it.
     *   2. Otherwise generate a deterministic name from the MAC address. */
    const char* custom = version_get_custom_name(NULL);
    const char* effective;
    if(custom && custom[0]) {
        effective = custom;
    } else {
        effective = generate_deterministic_name();
    }
    furi_hal_version_refresh_names(effective);
}

bool furi_hal_version_do_i_belong_here(void) {
    return true;
}

const char* furi_hal_version_get_model_name(void) {
    return "Flipper Zero";
}

const char* furi_hal_version_get_model_code(void) {
    /* Report the actual silicon so host tools (qFlipper, esptool) see the
     * real chip rather than a hardcoded "ESP32-C6". */
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    return "ESP32-S3";
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
    return "ESP32-C6";
#else
    return "ESP32";
#endif
}

const char* furi_hal_version_get_fcc_id(void) {
    return "N/A";
}

const char* furi_hal_version_get_ic_id(void) {
    return "N/A";
}

const char* furi_hal_version_get_mic_id(void) {
    return "N/A";
}

const char* furi_hal_version_get_srrc_id(void) {
    return "N/A";
}

const char* furi_hal_version_get_ncc_id(void) {
    return "N/A";
}

FuriHalVersionOtpVersion furi_hal_version_get_otp_version(void) {
    return FuriHalVersionOtpVersionEmpty;
}

uint8_t furi_hal_version_get_hw_version(void) {
    return 1;
}

uint8_t furi_hal_version_get_hw_target(void) {
    return version_get_target(NULL);
}

uint8_t furi_hal_version_get_hw_body(void) {
    return 0;
}

FuriHalVersionColor furi_hal_version_get_hw_color(void) {
    return FuriHalVersionColorUnknown;
}

uint8_t furi_hal_version_get_hw_connect(void) {
    return 0;
}

FuriHalVersionRegion furi_hal_version_get_hw_region(void) {
    return FuriHalVersionRegionWorld;
}

const char* furi_hal_version_get_hw_region_name(void) {
    return "WW";
}

FuriHalVersionRegion furi_hal_version_get_hw_region_otp(void) {
    return FuriHalVersionRegionWorld;
}

const char* furi_hal_version_get_hw_region_name_otp(void) {
    return "WW";
}

FuriHalVersionDisplay furi_hal_version_get_hw_display(void) {
    return FuriHalVersionDisplayUnknown;
}

uint32_t furi_hal_version_get_hw_timestamp(void) {
    return 0;
}

const char* furi_hal_version_get_name_ptr(void) {
    return furi_hal_version.name;
}

const char* furi_hal_version_get_device_name_ptr(void) {
    return furi_hal_version.device_name;
}

const char* furi_hal_version_get_ble_local_device_name_ptr(void) {
    return furi_hal_version.ble_local_name;
}

void furi_hal_version_set_name(const char* name) {
    /* Accept NULL or empty string — fall back to the MAC-derived name. */
    if(name && name[0]) {
        furi_hal_version_refresh_names(name);
    } else {
        furi_hal_version_refresh_names(generate_deterministic_name());
    }
}

const uint8_t* furi_hal_version_get_ble_mac(void) {
    return furi_hal_version.ble_mac;
}

const struct Version* furi_hal_version_get_firmware_version(void) {
    return version_get();
}

size_t furi_hal_version_uid_size(void) {
    return sizeof(furi_hal_version.uid);
}

const uint8_t* furi_hal_version_uid(void) {
    return furi_hal_version.uid;
}

const uint8_t* furi_hal_version_uid_default(void) {
    return furi_hal_version.uid;
}
