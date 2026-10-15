/* SPDX-License-Identifier: MIT */
#include "tvid_format.h"
#include <string.h>

static uint16_t le16(const uint8_t* p) {
    return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}
static uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
bool tvid_parse_header(const uint8_t* p, size_t size, TvidHeader* out) {
    if(!p || !out || size != TVID_HEADER_SIZE || memcmp(p, "TVID0001", 8)) return false;
    uint16_t fps = le16(p + 12), flags = le16(p + 14);
    uint32_t frames = le32(p + 20), max_jpeg = le32(p + 24);
    if(le16(p + 8) != TVID_WIDTH || le16(p + 10) != TVID_HEIGHT ||
       (fps != 5 && fps != 10 && fps != 20) || flags > 1 ||
       le32(p + 16) != TVID_RATE || !frames || frames > 432000 ||
       max_jpeg < 4 || max_jpeg > TVID_MAX_JPEG || le32(p + 28)) return false;
    *out = (TvidHeader){.fps = fps, .audio = flags != 0,
                        .frames = frames, .max_jpeg = max_jpeg};
    return true;
}
bool tvid_parse_frame(const uint8_t p[8], const TvidHeader* h,
                      uint64_t remaining, uint32_t* jpeg_size, uint32_t* pcm_size) {
    if(!p || !h || !jpeg_size || !pcm_size || !h->fps) return false;
    uint32_t jpeg = le32(p), pcm = le32(p + 4);
    uint32_t expected_pcm = h->audio ? TVID_RATE / h->fps * 2 : 0;
    if(jpeg < 4 || jpeg > h->max_jpeg || jpeg > TVID_MAX_JPEG || pcm != expected_pcm ||
       (uint64_t)jpeg + pcm > remaining) return false;
    *jpeg_size = jpeg;
    *pcm_size = pcm;
    return true;
}
