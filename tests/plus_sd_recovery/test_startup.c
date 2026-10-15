#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef struct { void* port; unsigned pin; } GpioPin;
typedef int esp_err_t;
enum { GpioModeInput, GpioModeOutputPushPull, GpioPullUp, GpioSpeedLow, FuriStatusOk=0 };
#define CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG 1
#define BOARD_EXTERNAL_UART_TX 43
#define BOARD_EXTERNAL_UART_RX 44
#define BOARD_PIN_NRF24_CSN 44
#define BOARD_PIN_NRF24_CE 43
#define BOARD_PIN_SD_CS 13
#define BOARD_PIN_PWR_EN 15
#define BOARD_CC1101_SPI_SHARED 1
#define ESP_OK 0
#define ESP_ERR_NVS_NO_FREE_PAGES 1
#define ESP_ERR_NVS_NEW_VERSION_FOUND 2
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
static const GpioPin gpio_sdcard_cs={0,13}, gpio_lcd_cs={0,41}, gpio_ext_pa4={0,12}, gpio_nrf24_cs={0,44};
static const int furi_hal_spi_bus_handle_subghz=1;
static bool output[49], written[49], level[49];
static unsigned sd_attempts, spi_traffic, delays;
static bool bus_ready, card_ok;
static void furi_hal_cortex_init_early(void) {}
static void furi_hal_gpio_write(const GpioPin* p,bool high) {written[p->pin]=true;level[p->pin]=high;}
static void furi_hal_gpio_init_simple(const GpioPin* p,int mode) {
    assert(mode==GpioModeOutputPushPull);
#if CONFIG_T_EMBED_PLUS_SD_RECOVERY
    if(p->pin==13||p->pin==41||p->pin==12||p->pin==44||p->pin==43) {
        assert(written[p->pin]);
        assert(level[p->pin]==(p->pin!=43));
    }
    if(p->pin==15) {
        assert(output[13]&&level[13]);assert(output[41]&&level[41]);
        assert(output[12]&&level[12]);assert(output[44]&&level[44]);
        assert(output[43]&&!level[43]);
    }
#endif
    output[p->pin]=true;
}
static void furi_hal_gpio_init(const GpioPin* p,int mode,int pull,int speed) {
    assert(mode==GpioModeInput&&pull==GpioPullUp&&speed==GpioSpeedLow);
    assert(p->pin==43||p->pin==44);output[p->pin]=false;
}
static int nvs_flash_init(void) {return ESP_OK;}
static void nvs_flash_erase(void) {assert(false);}
#define INIT_STUB(name) static void name(void) {}
INIT_STUB(furi_hal_rtc_init)
INIT_STUB(furi_hal_version_init)
INIT_STUB(furi_hal_info_init)
INIT_STUB(furi_hal_power_init)
INIT_STUB(furi_hal_crypto_init)
INIT_STUB(furi_hal_usb_init)
INIT_STUB(furi_hal_light_init)
INIT_STUB(furi_hal_speaker_init)
INIT_STUB(furi_hal_nfc_init)
static void furi_hal_spi_bus_handle_init(const int* h) {assert(*h==1);assert(!spi_traffic);bus_ready=true;}
static void furi_delay_ms(unsigned ms) {delays+=ms;}
static int furi_hal_sd_init(bool reset) {
    assert(!reset&&bus_ready&&!spi_traffic);
    assert(output[13]&&level[13]&&output[41]&&level[41]);
    assert(output[12]&&level[12]&&output[44]&&level[44]);
    ++sd_attempts;
    return card_ok?FuriStatusOk:1;
}
static void furi_hal_subghz_init(void) {
#if CONFIG_T_EMBED_PLUS_SD_RECOVERY
    assert(sd_attempts==1);
#endif
    ++spi_traffic;
}
static void furi_hal_display_init(void) {
#if CONFIG_T_EMBED_PLUS_SD_RECOVERY
    assert(sd_attempts==1);
#endif
    ++spi_traffic;
}
/* PRODUCTION */
int main(void) {
    for(unsigned success=0;success<2;++success) {
        memset(output,0,sizeof(output));memset(written,0,sizeof(written));memset(level,0,sizeof(level));
        sd_attempts=spi_traffic=delays=0;bus_ready=false;card_ok=success;
        furi_hal_init_early();furi_hal_init();
        assert(spi_traffic==2);
#if CONFIG_T_EMBED_PLUS_SD_RECOVERY
        assert(sd_attempts==1&&delays==100);
        assert(output[13]&&level[13]&&output[41]&&level[41]);
        assert(output[12]&&level[12]&&output[44]&&level[44]);
        assert(output[43]&&!level[43]);
#else
        assert(!sd_attempts&&!delays&&!bus_ready);
        assert(!output[13]&&!output[41]&&!output[12]);
#if CONFIG_T_EMBED_EXTERNAL_UART
        assert(!output[43]&&!output[44]);
#else
        assert(output[44]&&level[44]&&output[43]&&!level[43]);
#endif
#endif
        assert(furi_hal_external_uart_available()==(bool)CONFIG_T_EMBED_EXTERNAL_UART);
    }
    puts("PASS production startup ordering and failed-card boot");
}
