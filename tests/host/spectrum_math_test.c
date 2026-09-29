#include "subghz_spectrum_math.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    assert(subghz_spectrum_bin_frequency(434000000,2000000,0,64,true)==433000000);
    assert(subghz_spectrum_bin_frequency(434000000,2000000,63,64,true)==435000000);
    assert(subghz_spectrum_bin_frequency(434000000,2000000,64,64,true)==0);
    assert(subghz_spectrum_bin_frequency(0,2000000,0,64,true)==0);
    assert(subghz_spectrum_bin_frequency(UINT32_MAX,2000000,63,64,true)==0);
    assert(subghz_spectrum_bin_frequency(434000000,2000000,0,128,false)==433007812);
    assert(subghz_spectrum_pan_step(2000000)==100000);
    assert(subghz_spectrum_pan_step(100000)==1000);
    assert(subghz_spectrum_pan_step(1000000)==10000);
    assert(subghz_spectrum_pan_step(10000000)==100000);
    assert(subghz_spectrum_pan_step(100000000)==1000000);
    assert(subghz_spectrum_pan_step(100000001)==10000000);
    const uint32_t valid[] = {300000000, 348000000, 387000000, 433920000, 464000000, 779000000, 928000000};
    const uint32_t invalid[] = {0, 299999999, 348000001, 386999999, 464000001, 778999999, 928000001, UINT32_MAX};
    for(size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) assert(subghz_spectrum_frequency_supported(valid[i]));
    for(size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) assert(!subghz_spectrum_frequency_supported(invalid[i]));
    unsigned history[96] = {0}; size_t write_row = 0;
    for(unsigned sweep = 1; sweep <= 10000; ++sweep) {
        history[write_row] = sweep; write_row = (write_row + 1) % 96;
        for(unsigned age = 0; age < 96 && age < sweep; ++age)
            assert(history[subghz_spectrum_history_row(write_row, age, 96)] == sweep - age);
    }
    assert(subghz_spectrum_history_row(0, 0, 0) == SIZE_MAX);
    assert(subghz_spectrum_history_row(96, 0, 96) == SIZE_MAX);
    assert(subghz_spectrum_history_row(0, 96, 96) == SIZE_MAX);
    const float trace[] = {-100, -30, -80};
    assert(subghz_spectrum_interpolate(trace, 3, 0, 5) == -100);
    assert(subghz_spectrum_interpolate(trace, 3, 1, 5) == -65);
    assert(subghz_spectrum_interpolate(trace, 3, 2, 5) == -30);
    assert(subghz_spectrum_interpolate(trace, 3, 4, 5) == -80);
    assert(subghz_spectrum_interpolate(trace, 3, 5, 5) == -127);
    assert(subghz_spectrum_interpolate(NULL, 3, 0, 5) == -127);
    assert(subghz_spectrum_interpolate(trace, 1, 3, 5) == -100);
    assert(subghz_spectrum_interpolate(trace, 3, 0, 1) == -100);
    assert(subghz_spectrum_sdr_color(0) == 0);
    assert(subghz_spectrum_sdr_color(255) == 0xffff);
    /* Lowest intensities round down in RGB565; this is quantization, not an error. */
    assert(subghz_spectrum_sdr_color(1) == 0);
    for(unsigned i = 3; i < 255; ++i) assert(subghz_spectrum_sdr_color(i) != 0);
    puts("PASS: specified CC1101 bands/gaps and 10000 circular waterfall wraps");
}
