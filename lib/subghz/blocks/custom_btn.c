#include "custom_btn_i.h"

static uint8_t custom_btn_id = SUBGHZ_CUSTOM_BTN_OK;
static uint8_t custom_btn_original = 0;
static uint8_t custom_btn_max_btns = 0;
static uint8_t controller_programming_mode = PROG_MODE_OFF;
static bool subghz_custom_btn_long = false;

static bool custom_btn_has_pages = false;
static uint8_t custom_btn_page = 0;
static uint8_t custom_btn_max_pages = 2;

bool subghz_custom_btn_set(uint8_t btn_id) {
    if(btn_id > custom_btn_max_btns) {
        custom_btn_id = SUBGHZ_CUSTOM_BTN_OK;
        return false;
    } else {
        custom_btn_id = btn_id;
        return true;
    }
}

uint8_t subghz_custom_btn_get(void) {
    return custom_btn_id;
}

void subghz_custom_btn_set_original(uint8_t btn_code) {
    custom_btn_original = btn_code;
}

uint8_t subghz_custom_btn_get_original(void) {
    return custom_btn_original;
}

void subghz_custom_btn_set_max(uint8_t b) {
    custom_btn_max_btns = b;
}

void subghz_custom_btns_reset(void) {
    custom_btn_original = 0;
    custom_btn_max_btns = 0;
    controller_programming_mode = PROG_MODE_OFF;
    custom_btn_id = SUBGHZ_CUSTOM_BTN_OK;
    subghz_custom_btn_long = false;
    custom_btn_has_pages = false;
    custom_btn_page = 0;
    custom_btn_max_pages = 2;
}

bool subghz_custom_btn_is_allowed(void) {
    return custom_btn_max_btns != 0;
}

void subghz_custom_btn_set_prog_mode(ProgMode prog_mode) {
    controller_programming_mode = prog_mode;
}

ProgMode subghz_custom_btn_get_prog_mode(void) {
    return controller_programming_mode;
}

void subghz_custom_btn_set_long(bool v) {
    subghz_custom_btn_long = v;
}

bool subghz_custom_btn_get_long(void) {
    return subghz_custom_btn_long;
}

void subghz_custom_btn_set_pages(bool enabled) {
    custom_btn_has_pages = enabled;
}

bool subghz_custom_btn_has_pages(void) {
    return custom_btn_has_pages;
}

void subghz_custom_btn_set_page(uint8_t page) {
    custom_btn_page = page;
}

uint8_t subghz_custom_btn_get_page(void) {
    return custom_btn_page;
}

void subghz_custom_btn_set_max_pages(uint8_t n) {
    custom_btn_max_pages = n;
}

uint8_t subghz_custom_btn_get_max_pages(void) {
    return custom_btn_max_pages;
}
