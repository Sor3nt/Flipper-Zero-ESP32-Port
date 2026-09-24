"""GPS must reject NRF/USB/flash pins before touching a driver or GPIO."""
from pathlib import Path
from host_build import run
root=Path(__file__).resolve().parents[2]
app=root/'applications_user/wardriver'
out=root/'build_t_embed/host_tests'; out.mkdir(parents=True,exist_ok=True)
source=(app/'wardriver_gps.c').read_text(encoding='utf-8')
source='\n'.join(s for s in source.splitlines() if not s.startswith('#include'))
prefix=r'''
#include "wardriver_gps.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#define ESP_OK 0
#define UART_DATA_8_BITS 8
#define UART_PARITY_DISABLE 0
#define UART_STOP_BITS_1 1
#define UART_HW_FLOWCTRL_DISABLE 0
#define UART_SCLK_DEFAULT 0
#define GPIO_IS_VALID_GPIO(n) ((n)>=0 && (n)<=48)
#define GPIO_IS_VALID_OUTPUT_GPIO(n) GPIO_IS_VALID_GPIO(n)
typedef struct { int baud_rate,data_bits,parity,stop_bits,flow_ctrl,source_clk; } uart_config_t;
static bool uart_is_driver_installed(int n) { (void)n; abort(); }
static int uart_driver_install(int n,int a,int b,int c,void* d,int e) { (void)n;(void)a;(void)b;(void)c;(void)d;(void)e;abort(); }
static int uart_driver_delete(int n) { (void)n;abort(); }
static int uart_param_config(int n,const uart_config_t* c) { (void)n;(void)c;abort(); }
static int uart_set_pin(int n,int a,int b,int c,int d) { (void)n;(void)a;(void)b;(void)c;(void)d;abort(); }
static int uart_read_bytes(int n,void* b,size_t s,int t) { (void)n;(void)b;(void)s;(void)t;abort(); }
static void gpio_reset_pin(int n) { (void)n;abort(); }
'''
suffix=r'''
int main(void) {
    WardriverGps gps;
    WardriverConfig c={.gps_uart=1,.gps_baud=9600,.gps_rx=43,.gps_tx=-1,.isolated_uart_pins=true};
    wardriver_gps_start(&gps,&c); assert(!strcmp(gps.status,"OFF"));
    c.gps_mode=2;
    for(int pin=43;pin<=44;++pin) {
        c.gps_rx=pin;c.gps_tx=-1;wardriver_gps_start(&gps,&c);
        assert(!gps.installed && !strcmp(gps.status,"NRF PINS RESERVED"));
        c.gps_rx=4;c.gps_tx=pin;wardriver_gps_start(&gps,&c);
        assert(!gps.installed && !strcmp(gps.status,"NRF PINS RESERVED"));
        wardriver_gps_stop(&gps);
    }
    int blocked[]={-1,19,20,26,32,37,49};
    for(unsigned i=0;i<sizeof(blocked)/sizeof(blocked[0]);++i) {
        c.gps_rx=blocked[i];c.gps_tx=-1;wardriver_gps_start(&gps,&c);
        assert(!gps.installed && !strcmp(gps.status,"CHECK PINS"));
    }
    puts("PASS: GPS OFF and reserved NRF/USB/flash pins never call UART or mutate GPIO");
}
'''
c=out/'gps_pins_test.c';c.write_text(prefix+source+suffix,encoding='utf-8')
binary=out/'gps_pins_test'
run(['gcc','-std=c17','-Wall','-Wextra','-Werror','-O1','-I'+str(app),str(c),str(app/'wardriver_nmea.c'),'-lm','-o',str(binary)],check=True)
run([str(binary)],check=True)
