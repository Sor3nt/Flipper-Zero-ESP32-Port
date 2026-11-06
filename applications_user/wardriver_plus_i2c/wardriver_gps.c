#include "wardriver_gps.h"
#include "wardriver_gps_uart.h"
#include <sdkconfig.h>
#include <driver/uart.h>
#include <driver/gpio.h>
#include <string.h>

#if CONFIG_T_EMBED_BW16_I2C
static bool plus_reserved_pin(int32_t pin) {
    /* NRF owns CE43/CSN44; battery, NFC and BW16 share SDA8/SCL18. */
    return pin==43 || pin==44 || pin==8 || pin==18;
}
#endif

void wardriver_gps_start(WardriverGps* g,const WardriverConfig* c) {
    memset(g,0,sizeof(*g)); wardriver_nmea_init(&g->parser);
    g->parser.data.enabled=c->gps_mode!=0;
    if(!c->gps_mode) { strcpy(g->status,"OFF"); return; }
#if CONFIG_T_EMBED_BW16_I2C
    /* Saved GPS settings must never take over these live Plus peripherals,
     * even if an older configuration asserts that its UART pins are isolated. */
    if(plus_reserved_pin(c->gps_rx) || plus_reserved_pin(c->gps_tx)) {
        g->parser.data.enabled=false;
        strcpy(g->status,"PIN IN USE");
        return;
    }
#endif
    strcpy(g->status,"WAITING");
    /* No pin is assigned automatically. Custom wiring must first be isolated
     * from its board peripheral. Never repurpose flash/PSRAM or USB pins.
     * This check is deliberately before driver install or pin mutation. */
    bool rx_ok=GPIO_IS_VALID_GPIO(c->gps_rx) && !(c->gps_rx>=26 && c->gps_rx<=37) &&
        c->gps_rx!=19 && c->gps_rx!=20;
    bool tx_ok=c->gps_tx==-1 || (GPIO_IS_VALID_OUTPUT_GPIO(c->gps_tx) &&
        !(c->gps_tx>=26 && c->gps_tx<=37) && c->gps_tx!=19 && c->gps_tx!=20);
    if(!c->isolated_uart_pins || !rx_ok || !tx_ok || c->gps_tx==c->gps_rx ||
       c->gps_uart<1 || c->gps_uart>2) { strcpy(g->status,"CHECK PINS"); return; }
    g->uart=(int)c->gps_uart; g->rx=c->gps_rx; g->tx=c->gps_tx;
    uart_config_t cfg={.baud_rate=(int)c->gps_baud,.data_bits=UART_DATA_8_BITS,
        .parity=UART_PARITY_DISABLE,.stop_bits=UART_STOP_BITS_1,
        .flow_ctrl=UART_HW_FLOWCTRL_DISABLE,.source_clk=UART_SCLK_DEFAULT};
    /* Takes the shared UART/pin leases (making a background RFID reader yield)
     * before any driver check or install, and rolls everything back on failure. */
    switch(wardriver_gps_uart_open(g->uart,g->rx,g->tx,&cfg)) {
    case WardriverGpsUartOk: g->installed=true; break;
    case WardriverGpsUartBusy: strcpy(g->status,"UART BUSY"); break;
    case WardriverGpsUartPinsBusy: strcpy(g->status,"PIN IN USE"); break;
    case WardriverGpsUartCleanup: strcpy(g->status,"UART CLEANUP"); break;
    default: strcpy(g->status,"UART ERROR"); break;
    }
}
void wardriver_gps_poll(WardriverGps* g,uint32_t now) {
    if(!g->installed) return;
    uint8_t data[256];
    /* Bounded drain, nonblocking; GPS failure can never hold the scanner. */
    for(unsigned i=0;i<4;++i) {
        int n=uart_read_bytes(g->uart,data,sizeof(data),0);
        if(n<=0) break;
        wardriver_nmea_feed(&g->parser,data,(size_t)n,now);
    }
    WardriverGpsData d=wardriver_nmea_snapshot(&g->parser,now);
    strcpy(g->status,!d.connected?"WAITING":d.has_fix?"FIX":"NO FIX");
}
void wardriver_gps_stop(WardriverGps* g) {
    /* Polling stops now. Not gated on g->installed: a start or an earlier stop
     * that could not finish may still own leases, and this retries the release. */
    g->installed=false;
    wardriver_gps_uart_close();
}
