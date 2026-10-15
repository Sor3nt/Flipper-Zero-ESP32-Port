from pathlib import Path
import os,sys,tempfile
R=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(R/'tests/host'))
from host_build import run
if os.name=='nt' and not os.environ.get('ZIG_CC'):
    os.environ['ZIG_CC']=str(R/'.backups/bw16-evidence/native/ziglang/zig.exe')
# Each invocation owns a new workspace directory; preserved builds stay read-only.
(R/'.backups').mkdir(exist_ok=True)
B=Path(tempfile.mkdtemp(prefix='bw16_i2c_profiles_',dir=R/'.backups'))
(B/'driver').mkdir(exist_ok=True);(B/'freertos').mkdir(exist_ok=True)
(B/'sdkconfig.h').write_text("")
(B/'board_mock.h').write_text("#ifdef BOARD_TEST\n#define BOARD_EXTERNAL_UART_TX 43\n#define BOARD_PIN_NFC_SDA TEST_SDA\n#define BOARD_PIN_NFC_SCL TEST_SCL\n#endif\n")
(B/'esp_err.h').write_text("typedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_ERR_NOT_SUPPORTED 1\n#define ESP_ERR_INVALID_ARG 2\n")
(B/'freertos/FreeRTOS.h').write_text("#define pdMS_TO_TICKS(x) ((x)/10)\n")
(B/'driver/i2c.h').write_text("#include <stddef.h>\n#include <stdint.h>\n#define I2C_NUM_0 0\nint i2c_master_write_to_device(int,int,const uint8_t*,size_t,unsigned);\nint i2c_master_read_from_device(int,int,uint8_t*,size_t,unsigned);\n")
s=(R/'components/furi_hal/furi_hal_bw16_i2c.c').read_text().replace('"boards/board.h"','"board_mock.h"')
(B/'production.c').write_text(s)
(B/'test.c').write_text(r'''#include "furi_hal_bw16_i2c.h"
#include <assert.h>
#include <stdio.h>
static int calls;
int i2c_master_write_to_device(int bus,int address,const uint8_t* data,size_t size,unsigned timeout) {
 assert(bus==0 && address==0x42 && data && size==64 && timeout==3);++calls;return 19;
}
int i2c_master_read_from_device(int bus,int address,uint8_t* data,size_t size,unsigned timeout) {
 assert(bus==0 && address==0x42 && data && size==64 && timeout==3);++calls;return 23;
}
int main(void) {
 uint8_t data[64]={0};
#if CONFIG_T_EMBED_BW16_I2C && defined(BOARD_TEST) && TEST_SDA == 8 && TEST_SCL == 18
 assert(furi_hal_bw16_i2c_available());
 assert(furi_hal_bw16_i2c_write(data,64)==19);assert(furi_hal_bw16_i2c_read(data,64)==23);
 assert(furi_hal_bw16_i2c_write(NULL,64)==ESP_ERR_INVALID_ARG);
 assert(furi_hal_bw16_i2c_read(NULL,64)==ESP_ERR_INVALID_ARG);
 assert(furi_hal_bw16_i2c_write(data,65)==ESP_ERR_INVALID_ARG);
 assert(furi_hal_bw16_i2c_read(data,63)==ESP_ERR_INVALID_ARG);assert(calls==2);
#else
 assert(!furi_hal_bw16_i2c_available());
 assert(furi_hal_bw16_i2c_write(data,64)==ESP_ERR_NOT_SUPPORTED);
 assert(furi_hal_bw16_i2c_read(data,64)==ESP_ERR_NOT_SUPPORTED);assert(!calls);
#endif
 puts("PASS: HAL profile gate, address0x42,64byte limit,30ms timeout, driver errors propagated");
}
''')
for enabled,uart,board,sda,scl in ((1,0,1,8,18),(0,1,1,8,18),(1,1,1,8,18),
                                   (0,0,1,8,18),(1,0,0,8,18),(1,1,0,8,18),
                                   (1,1,1,7,18),(1,1,1,8,17)):
 exe=B/f'hal_{enabled}_{uart}_{board}_{sda}_{scl}.exe'
 run(['gcc','-std=c17','-Wall','-Wextra','-Werror',f'-DCONFIG_T_EMBED_BW16_I2C={enabled}',f'-DCONFIG_T_EMBED_EXTERNAL_UART={uart}',f'-DTEST_SDA={sda}',f'-DTEST_SCL={scl}',*(['-DBOARD_TEST=1'] if board else []),'-I',str(B),'-I',str(R/'components/furi_hal'),str(B/'production.c'),str(B/'test.c'),'-o',str(exe)],check=True)
 run([exe],check=True)
# Exercise production early boot and UART availability for the same profiles.
run([sys.executable,str(R/'tests/bw16/test_profile.py'),'--output-dir',str(B/'boot')],check=True)
print('PASS: independent I2C/UART profile matrix; isolated test outputs:',B)
