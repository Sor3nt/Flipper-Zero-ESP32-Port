"""Exercise production early GPIO initialization and firmware capability query."""
from pathlib import Path
import argparse,os,sys,tempfile
root=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(root/'tests/host'))
from host_build import run
if os.name=='nt' and not os.environ.get('ZIG_CC'):
    os.environ['ZIG_CC']=str(root/'.backups/bw16-evidence/native/ziglang/zig.exe')
source=(root/'components/furi_hal/furi_hal.c').read_text()
source=source[source.index('bool furi_hal_external_uart_available'):source.index('void furi_hal_deinit_early')]
prefix=r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
typedef struct { void* port; unsigned pin; } GpioPin;
enum { GpioModeInput, GpioModeOutputPushPull, GpioPullUp, GpioSpeedLow };
#define BOARD_PIN_NRF24_CSN 44
#define BOARD_PIN_NRF24_CE 43
#ifdef T_EMBED
#define BOARD_EXTERNAL_UART_TX 43
#define BOARD_EXTERNAL_UART_RX 44
#endif
#define ESP_LOGI(...) ((void)0)
static int calls,writes,inputs;
static void furi_hal_cortex_init_early(void) {}
static void furi_hal_gpio_init(const GpioPin* p,int mode,int pull,int speed) {
    assert(p->pin==43 || p->pin==44);assert(mode==GpioModeInput && pull==GpioPullUp && speed==GpioSpeedLow);
    ++calls;++inputs;
}
static void furi_hal_gpio_init_simple(const GpioPin* p,int mode) {
    assert(p->pin==43 || p->pin==44);assert(mode==GpioModeOutputPushPull);++calls;
}
static void furi_hal_gpio_write(const GpioPin* p,bool value) {
    assert(value==(p->pin==44));++writes;
}
'''
suffix=r'''
int main(void) {
    furi_hal_init_early();assert(calls==2);
#if CONFIG_T_EMBED_EXTERNAL_UART && defined(T_EMBED)
    assert(inputs==2 && writes==0);
    assert(furi_hal_external_uart_available()==(bool)CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG);
#else
    assert(!inputs && writes==2 && !furi_hal_external_uart_available());
#endif
    puts("PASS: boot GPIO directions and running-firmware capability");
}
'''
arguments=argparse.ArgumentParser()
arguments.add_argument('--output-dir',type=Path)
options=arguments.parse_args()
# Isolate test outputs from every preserved firmware/FAP build directory.
if options.output_dir:
    out=options.output_dir.resolve()
    assert out.is_relative_to((root/'.backups').resolve()),'Test output must stay in .backups'
    out.mkdir(parents=True,exist_ok=True)
else:
    (root/'.backups').mkdir(exist_ok=True)
    out=Path(tempfile.mkdtemp(prefix='bw16_boot_profiles_',dir=root/'.backups'))
test=out/'profile.c';test.write_text(prefix+source+suffix)
# (external UART, USB console, T-Embed board, BW16 I2C).
# Both transports enabled must retain UART inputs and never drive NRF outputs.
for profile,usb,board,i2c in ((0,0,1,0),(0,1,1,1),(1,0,1,0),(1,1,1,0),
                               (1,1,1,1),(1,0,1,1),(1,1,0,1)):
    exe=out/f'profile_{profile}_{usb}_{board}_{i2c}.exe'
    run(['gcc','-std=c17','-O1','-g','-fsanitize=address,undefined',
         f'-DCONFIG_T_EMBED_EXTERNAL_UART={profile}',f'-DCONFIG_ESP_CONSOLE_USB_SERIAL_JTAG={usb}',
         f'-DCONFIG_T_EMBED_BW16_I2C={i2c}',
         *(['-DT_EMBED=1'] if board else []),str(test),'-o',str(exe)],check=True)
    run([str(exe)],check=True)

print("PASS: original UART, Plus, combined UART+I2C, USB-console and other-board boot separation")
