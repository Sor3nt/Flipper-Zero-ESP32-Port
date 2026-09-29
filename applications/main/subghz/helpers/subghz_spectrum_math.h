#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* CC1101 specified bands, not the wider experimental tuning range of the
 * general-purpose HAL. Keep the measurement app within documented hardware. */
static inline bool subghz_spectrum_frequency_supported(uint32_t hz) {
    return (hz >= 300000000u && hz <= 348000000u) ||
           (hz >= 387000000u && hz <= 464000000u) ||
           (hz >= 779000000u && hz <= 928000000u);
}

static inline size_t subghz_spectrum_history_row(size_t write_row, size_t age, size_t rows) {
    if(!rows || write_row >= rows || age >= rows) return SIZE_MAX;
    return (write_row + rows - 1 - age) % rows;
}

static inline uint32_t subghz_spectrum_bin_frequency(
    uint32_t center, uint32_t span, size_t bin, size_t bins, bool endpoints) {
    if(bins < 2 || bin >= bins || !span || span / 2 > center) return 0;
    uint64_t start = center - span / 2;
    uint64_t offset = endpoints ? (uint64_t)span * bin / (bins - 1) :
        (uint64_t)(span / bins) * bin + (span / bins) / 2;
    return start + offset <= UINT32_MAX ? (uint32_t)(start + offset) : 0;
}

static inline uint32_t subghz_spectrum_pan_step(uint32_t span) {
    return span > 100000000 ? 10000000 : span > 10000000 ? 1000000 :
        span > 1000000 ? 100000 : span > 100000 ? 10000 : 1000;
}

/* Display interpolation only: it never creates additional RF measurements. */
static inline float subghz_spectrum_interpolate(
    const float* samples, size_t bins, size_t column, size_t width) {
    if(!samples || !bins || !width || column >= width) return -127.0f;
    if(bins == 1 || width == 1) return samples[0];
    const float position = (float)column * (bins - 1) / (width - 1);
    const size_t left = (size_t)position;
    if(left >= bins - 1) return samples[bins - 1];
    return samples[left] + (samples[left + 1] - samples[left]) * (position - left);
}

/* Classic SDR-style intensity ramp, not an FFT or protocol classification.
 * Independently implemented; RGB565 returned in host byte order. */
static inline uint16_t subghz_spectrum_sdr_color(uint8_t level) {
    const uint8_t colors[][3] = {
        {0, 0, 0}, {0, 0, 128}, {0, 128, 255}, {0, 220, 220},
        {0, 220, 0}, {255, 255, 0}, {255, 0, 0}, {255, 255, 255},
    };
    unsigned position = (unsigned)level * 7;
    unsigned segment = position / 255;
    if(segment == 7) return UINT16_MAX;
    unsigned fraction = position % 255;
    unsigned rgb[3];
    for(size_t i = 0; i < 3; ++i)
        rgb[i] = (colors[segment][i] * (255 - fraction) +
                  colors[segment + 1][i] * fraction) / 255;
    return (uint16_t)(((rgb[0] & 0xf8) << 8) | ((rgb[1] & 0xfc) << 3) | (rgb[2] >> 3));
}
