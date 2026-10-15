/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TVID_HEADER_SIZE 32u
#define TVID_MAX_JPEG 65536u
#define TVID_WIDTH 320u
#define TVID_HEIGHT 170u
#define TVID_RATE 16000u

typedef struct {
    uint16_t fps;
    bool audio;
    uint32_t frames;
    uint32_t max_jpeg;
} TvidHeader;

bool tvid_parse_header(const uint8_t* bytes, size_t size, TvidHeader* out);
bool tvid_parse_frame(const uint8_t bytes[8], const TvidHeader* header,
                      uint64_t remaining, uint32_t* jpeg_size, uint32_t* pcm_size);
