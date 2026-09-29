/* Included after the actual production compositor, not a parallel renderer. */
int main(int argc, char** argv) {
    assert(argc == 2);
    assert(COUNT_OF(subghz_spectrum_spans) == 6);
    const size_t pixels = 320 * 170;
    uint16_t* guarded = calloc(pixels + 2, sizeof(uint16_t));
    assert(guarded);
    guarded[0] = 0x1234; guarded[pixels + 1] = 0xabcd;
    SubGhzSpectrum view = {
        .w = 320, .h = 170, .fb = guarded + 1,
        .wf = calloc(WF_ROWS_MAX, BINS), .spectrogram = true,
        .center = 434000000, .span = 2000000, .dwell_ms = 3,
        .threshold = -70, .cursor = 64, .peak_decay = true,
        .has_data = true,
        .bins = 64,
    };
    assert(view.wf);
    view.has_data=false;
    subghz_spectrum_compose(&view);
    assert(view.fb[85*320+20]==COL_TRACE);
    assert(view.fb[86*320+20]==rgb565(0,35,90));
    view.has_data=true;
    view.stripe=calloc(320*STRIPE_H,sizeof(uint16_t)); assert(view.stripe);
    /* Synthetic two-carrier fixture. Never represented as hardware evidence. */
    for(int i = 0; i < BINS; ++i) {
        int d1 = abs(i - 21), d2 = abs(i - 47);
        float level = -94 + (i % 4);
        if(d1 < 9) level += (9 - d1) * 5;
        if(d2 < 5) level += (5 - d2) * 7;
        view.rssi[i] = level;
        view.displayed[i] = level - 2;
        view.peak[i] = level + 4;
        for(size_t row = 0; row < WF_ROWS_MAX; ++row)
            view.wf[row * BINS + i] = (uint8_t)(subghz_waterfall_norm(level - row % 9) * 255);
    }
    for(size_t frame = 0; frame < 200; ++frame) {
        view.span = subghz_spectrum_spans[frame % COUNT_OF(subghz_spectrum_spans)];
        view.wf_write = frame % WF_ROWS_MAX;
        view.spectrogram = frame % 2 == 1;
        view.adjust = frame % SubGhzSpectrumAdjustMAX;
        subghz_spectrum_compose(&view);
        blit_rows=0; subghz_spectrum_blit(&view); assert(blit_rows==170);
        assert(guarded[0] == 0x1234 && guarded[pixels + 1] == 0xabcd);
    }
    view.spectrogram = true;
    view.span = 2000000;
    view.adjust = SubGhzSpectrumAdjustCenter;
    subghz_spectrum_compose(&view);
    FILE* preview = fopen(argv[1], "wb");
    assert(preview);
    for(size_t i = 0; i < pixels; ++i) {
        uint16_t c = view.fb[i];
        c = (uint16_t)((c << 8) | (c >> 8));
        uint8_t rgb[] = {(uint8_t)(((c >> 11) & 31) * 255 / 31),
                         (uint8_t)(((c >> 5) & 63) * 255 / 63),
                         (uint8_t)((c & 31) * 255 / 31)};
        assert(fwrite(rgb, sizeof(rgb), 1, preview) == 1);
    }
    assert(fclose(preview) == 0);
    free(view.stripe); free(view.wf); free(guarded);
    puts("PASS: 200 production compositor frames / history wraps / framebuffer guards");
}
