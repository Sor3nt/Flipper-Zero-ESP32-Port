#include "wardriver_gps.h"
#include <driver/uart.h>
#include <driver/gpio.h>
#include <string.h>

void wardriver_gps_start(WardriverGps* g,const WardriverConfig* c) {
    memset(g,0,sizeof(*g)); wardriver_nmea_init(&g->parser);
    g->parser.data.enabled=c->gps_mode!=0;
    if(!c->gps_mode) { strcpy(g->status,"OFF"); return; }
    /* The BW16 guard's swapped UART protocol does not authorize a GPS module
     * to take NRF CE/CS. GPS has no prepare/disconnect workflow. Reject shared
     * pins before touching either UART or GPIO; the survey still works without GPS. */
    if(c->gps_rx==43 || c->gps_rx==44 || c->gps_tx==43 || c->gps_tx==44) {
        strcpy(g->status,"NRF PINS RESERVED"); return;
    }
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
    if(uart_is_driver_installed(g->uart)) { strcpy(g->status,"UART BUSY"); return; }
    uart_config_t cfg={.baud_rate=(int)c->gps_baud,.data_bits=UART_DATA_8_BITS,
        .parity=UART_PARITY_DISABLE,.stop_bits=UART_STOP_BITS_1,
        .flow_ctrl=UART_HW_FLOWCTRL_DISABLE,.source_clk=UART_SCLK_DEFAULT};
    if(uart_driver_install(g->uart,2048,0,0,NULL,0)!=ESP_OK) { strcpy(g->status,"UART ERROR"); return; }
    g->installed=true;
    if(uart_param_config(g->uart,&cfg)!=ESP_OK || uart_set_pin(g->uart,g->tx,g->rx,-1,-1)!=ESP_OK) {
        wardriver_gps_stop(g); strcpy(g->status,"UART ERROR");
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
    if(!g->installed) return;
    uart_driver_delete(g->uart); g->installed=false;
    gpio_reset_pin(g->rx);
    if(g->tx>=0) gpio_reset_pin(g->tx);
}
