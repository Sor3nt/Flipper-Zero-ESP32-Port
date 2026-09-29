/* Reuse the matching hardware profile without changing either Wardriver. */
#include "wdgwars_map_profile.h"
#if WDG_MAP_PLUS_PROFILE
#include "../wardriver_plus_i2c/wardriver_gps.c"
#elif WDG_MAP_UART_I2C_PROFILE
#include "../wardriver/wardriver_gps.h"
static void wdg_map_uart_gps_start(WardriverGps* gps,const WardriverConfig* config);
#define wardriver_gps_start wdg_map_uart_gps_start
#include "../wardriver/wardriver_gps.c"
#undef wardriver_gps_start

void wardriver_gps_start(WardriverGps* gps,const WardriverConfig* config) {
    /* UART43/44 stays available. Saved custom settings cannot take over the
     * live BW16/battery/NFC I2C pins, even when marked isolated on the SD card. */
    if(config->gps_mode && (config->gps_rx==8 || config->gps_rx==18 ||
        config->gps_tx==8 || config->gps_tx==18)) {
        memset(gps,0,sizeof(*gps));
        wardriver_nmea_init(&gps->parser);
        gps->parser.data.enabled=false;
        strcpy(gps->status,"PIN IN USE");
        return;
    }
    wdg_map_uart_gps_start(gps,config);
}
#else
#include "../wardriver/wardriver_gps.c"
#endif
