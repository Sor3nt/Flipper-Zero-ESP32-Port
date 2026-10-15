#include "hal_stubs.h"
#define calloc test_calloc
#include "furi_hal_sd_under_test.inc"
#undef calloc
static void clear_test(void) {
    assert(!lock_depth);
    fail_detach=0;
    if(sd_initialized)assert(furi_hal_sd_unmount());
    assert(!device_count);
    lock_depth=peak_lock=device_count=peak_devices=device_adds=device_removes=global_deinits=0;
    card_inits=detaches=mounts=registrations=unregistrations=raw_reads=raw_writes=cs_writes=0;
    fail_host=fail_device=fail_card=fail_alloc=fail_detach=fail_mount=0;
    card_attached=cs_high=cs_output=false;
    memset(rates,0,sizeof(rates));
    memset(&injected_identity,0,sizeof(injected_identity));
    memset(&furi_hal_spi_bus_external,0,sizeof(furi_hal_spi_bus_external));
#if CONFIG_T_EMBED_PLUS_SD_RECOVERY
    sd_retry_frequency_step=0;
#endif
}
#if CONFIG_T_EMBED_PLUS_SD_RECOVERY
static void sandisk_fallback_recovery(void) {
    clear_test();
    injected_identity.csd.sector_size=512;
    injected_identity.csd.capacity=62688;
    injected_identity.cid.oem_id=0x5344;
    memcpy(injected_identity.cid.name,"SD032",6);
    for(unsigned attempt=0;attempt<3;++attempt) {
        assert(furi_hal_sd_init(attempt!=0)==FuriStatusError);
        assert(!sd_card&&!sd_initialized&&!sd_diskio_registered&&!device_count&&!lock_depth);
        assert(!registrations&&!raw_reads&&!raw_writes&&!mounts&&!global_deinits);
        assert(device_adds==device_removes&&cs_high&&cs_output);
        uint8_t sector[512];
        assert(!furi_hal_sd_read_sectors_raw(0,sector,1));
        assert(!furi_hal_sd_write_sectors_raw(0,sector,1));
    }
    assert(card_inits==3&&rates[0]==20000&&rates[1]==10000&&rates[2]==4000);
    memcpy(injected_identity.cid.name,"SD32G",6);
    injected_identity.csd.capacity=62333952;
    assert(furi_hal_sd_init(true)==FuriStatusOk&&card_inits==4);
    assert(registrations==1&&peak_devices==1&&rates[3]==4000);
    assert(furi_hal_sd_sector_count()==62333952&&furi_hal_sd_mount());
    // A similar name alone, or any different signature field, is not rejected.
    for(unsigned field=0;field<4;++field) {
        clear_test();
        injected_identity.csd.sector_size=512;injected_identity.csd.capacity=62688;
        injected_identity.cid.oem_id=0x5344;memcpy(injected_identity.cid.name,"SD032",6);
        if(field==0)injected_identity.cid.name[4]='3';
        if(field==1)injected_identity.cid.oem_id=0x1234;
        if(field==2)injected_identity.csd.capacity=8192;
        if(field==3)injected_identity.csd.sector_size=1024;
        assert(furi_hal_sd_init(false)==FuriStatusOk&&registrations==1);
    }
    puts("PASS HAL: SD032 false identity rejected before filesystem/raw access, owned device released, fresh SDHC identity accepted; near matches preserved");
}
static void one_attempt_and_clock_fallback(void) {
    clear_test();fail_card=2;
    assert(furi_hal_sd_init(false)==FuriStatusError);
    assert(card_inits==1&&!device_count&&cs_high&&cs_output&&!lock_depth&&!global_deinits);
    assert(furi_hal_sd_init(true)==FuriStatusError);
    assert(card_inits==2&&rates[0]==20000&&rates[1]==10000&&!device_count&&cs_output);
    assert(furi_hal_sd_init(true)==FuriStatusOk);
    assert(card_inits==3&&rates[2]==4000&&peak_devices==1&&device_count==1&&!global_deinits);
    assert(furi_hal_sd_init(false)==FuriStatusOk&&card_inits==3);
    assert(furi_hal_sd_init(true)==FuriStatusOk&&rates[3]==4000);
    assert(furi_hal_sd_unmount()&&cs_high&&cs_output&&!lock_depth);
    assert(furi_hal_sd_init(false)==FuriStatusOk&&rates[4]==20000);
    puts("PASS HAL: one card init per request, 20/10/4 MHz bounded fallback, no duplicate probe, new session restores20");
}
static void failures_clean_owned_handle(void) {
    for(unsigned mode=0;mode<3;++mode) {
        clear_test();if(mode==0)fail_host=1;else if(mode==1)fail_device=1;else fail_alloc=1;
        assert(furi_hal_sd_init(false)==FuriStatusError);
        assert(!sd_card&&!sd_initialized&&!device_count&&!lock_depth&&!global_deinits&&cs_output&&cs_high);
        assert(device_adds==device_removes);
        assert(furi_hal_sd_init(true)==FuriStatusOk&&rates[0]==10000&&peak_devices==1);
    }
    clear_test();furi_hal_spi_bus_external=(FuriHalSpiBus){.initialized=true,.host_id=-1,.mosi_pin=13};
    assert(furi_hal_sd_init(false)==FuriStatusError&&!card_inits&&!device_adds&&!cs_writes&&!lock_depth);
    assert(furi_hal_sd_init(true)==FuriStatusError&&!detaches&&!cs_writes&&!lock_depth);
    clear_test();fail_card=1;assert(furi_hal_sd_init(false)==FuriStatusError);
    assert(furi_hal_sd_unmount());assert(furi_hal_sd_init(false)==FuriStatusOk&&rates[1]==20000);
    puts("PASS HAL: host/device/allocation failure cleanup and conflicting SPI ownership are bounded");
}
static void failed_filesystem_retry_and_detach(void) {
    clear_test();fail_mount=1;
    assert(!furi_hal_sd_mount()&&sd_initialized&&card_attached);
    sdmmc_card_t* old=sd_card;unsigned before=device_removes;fail_detach=1;
    assert(furi_hal_sd_init(true)==FuriStatusError&&sd_card==old&&device_removes==before&&card_attached);
    assert(card_inits==1&&!lock_depth);
    assert(furi_hal_sd_init(true)==FuriStatusOk&&card_inits==2&&rates[1]==10000);
    assert(device_removes==before+1&&!card_attached&&peak_devices==1);
    assert(furi_hal_sd_mount()&&card_attached&&furi_hal_sd_is_mounted());
    fail_detach=1;assert(!furi_hal_sd_unmount()&&sd_initialized&&device_count==1);
    assert(!lock_depth&&!global_deinits);
    puts("PASS HAL: failed filesystem mount triggers requested real reinit; detach failure preserves current card");
}
#endif
static void soft_release_keeps_msc_card(void) {
    clear_test();assert(furi_hal_sd_mount());unsigned before=card_inits,removed=device_removes;
    assert(furi_hal_sd_release_fatfs()&&!furi_hal_sd_is_mounted());
    uint8_t sector[512];assert(furi_hal_sd_read_sectors_raw(0,sector,1)&&sector[0]==0x5a);
    assert(furi_hal_sd_write_sectors_raw(1,sector,1));
    assert(furi_hal_sd_sector_size()==512&&furi_hal_sd_sector_count()==8192);
    assert(furi_hal_sd_init(false)==FuriStatusOk&&card_inits==before&&device_removes==removed);
    assert(furi_hal_sd_mount()&&card_inits==before);
    assert(raw_reads==1&&raw_writes==1&&!lock_depth);
    assert(furi_hal_sd_unmount());
    assert(!furi_hal_sd_read_sectors_raw(0,sector,1));
    puts("PASS HAL: soft FatFs release and raw MSC access retain card/driver; normal remount does not reset");
}
int main(void) {
#if CONFIG_T_EMBED_PLUS_SD_RECOVERY
    sandisk_fallback_recovery();one_attempt_and_clock_fallback();failures_clean_owned_handle();failed_filesystem_retry_and_detach();
#else
    clear_test();assert(furi_hal_sd_init(false)==FuriStatusOk);
    assert(furi_hal_sd_init(true)==FuriStatusOk&&card_inits==1&&rates[0]==20000&&!detaches&&!cs_writes);
    assert(furi_hal_sd_unmount());
    puts("PASS HAL: default-off keeps legacy ignored reset, normal20 MHz and no new GPIO writes");
#endif
    soft_release_keeps_msc_card();clear_test();return 0;
}
