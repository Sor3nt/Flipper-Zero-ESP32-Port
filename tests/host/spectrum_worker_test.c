static void sweep(void* context,const float* rssi,uint32_t center,uint32_t span,uint16_t bins) {
    SubGhzSpectrumWorker* w=context;
    assert(center==w->center && span==w->span);
    assert(bins==(w->spectrogram?64:128));
    unsigned valid=0;
    for(unsigned i=0;i<bins;++i) {
        uint32_t frequency=subghz_spectrum_bin_frequency(center,span,i,bins,w->spectrogram);
        if(subghz_spectrum_frequency_supported(frequency)) {
            assert(frequencies[valid++]==frequency);
            assert(rssi[i]==(calibration_fail?-127.0f:-60.0f));
        } else assert(rssi[i]==-127.0f);
    }
    assert(tuned==valid && reads==(calibration_fail?0:valid));
    if(!calibration_fail) for(unsigned i=0;i<valid;++i) assert(dwell[i]==3);
    if(w->spectrogram) assert(bandwidth_reg==0x87);
    w->worker_running=false;
}
int main(void) {
    for(unsigned cycle=0;cycle<300;++cycle) {
        tuned=reads=slept=idled=0;
        SubGhzSpectrumWorker* w=subghz_spectrum_worker_alloc(); assert(w);
        subghz_spectrum_worker_set_spectrogram(w,cycle%2==0);
        subghz_spectrum_worker_set_window(w,cycle%3?434000000:370000000,cycle%3?2000000:80000000);
        subghz_spectrum_worker_set_controls(w,false,3);
        calibration_fail=cycle%5==0;
        subghz_spectrum_worker_set_callback(w,sweep,w);
        subghz_spectrum_worker_start(w);
        assert(!subghz_spectrum_worker_is_running(w) && slept==1 && idled==1 && !spi_depth);
        subghz_spectrum_worker_free(w); --allocations;
        assert(!threads && !mutexes && !allocations);
    }
    puts("PASS: 300 production RF sweeps; 64 endpoint / 128 legacy bins, bandwidth, settle, invalid bands, calibration timeouts, cleanup");
}
