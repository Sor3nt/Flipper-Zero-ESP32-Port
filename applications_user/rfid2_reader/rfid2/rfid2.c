#include "rfid2.h"

#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <esp_log.h>

#define TAG "rfid2"

// MFRC522 registers
#define REG_COMMAND     0x01
#define REG_COMIEN      0x02
#define REG_COMIRQ      0x04
#define REG_DIVIRQ      0x05
#define REG_ERROR       0x06
#define REG_FIFODATA    0x09
#define REG_FIFOLEVEL   0x0A
#define REG_CONTROL     0x0C
#define REG_BITFRAMING  0x0D
#define REG_COLL        0x0E
#define REG_MODE        0x11
#define REG_TXMODE      0x12
#define REG_RXMODE      0x13
#define REG_TXCONTROL   0x14
#define REG_TXASK       0x15
#define REG_MODWIDTH    0x24
#define REG_TMODE       0x2A
#define REG_TPRESCALER  0x2B
#define REG_TRELOADH    0x2C
#define REG_TRELOADL    0x2D
#define REG_VERSION     0x37

// MFRC522 commands
#define CMD_IDLE        0x00
#define CMD_TRANSCEIVE  0x0C
#define CMD_SOFTRESET   0x0F

// PICC commands
#define PICC_REQA       0x26
#define PICC_ANTICOLL1  0x93
#define PICC_ANTICOLL2  0x95
#define PICC_ANTICOLL3  0x97

static bool rd_write(uint8_t reg, uint8_t val) {
    uint8_t b[2] = {reg, val};
    return i2c_master_write_to_device(RFID2_I2C_PORT, RFID2_I2C_ADDR, b, 2, pdMS_TO_TICKS(30)) ==
           ESP_OK;
}

static uint8_t rd_read(uint8_t reg) {
    uint8_t val = 0;
    i2c_master_write_read_device(
        RFID2_I2C_PORT, RFID2_I2C_ADDR, &reg, 1, &val, 1, pdMS_TO_TICKS(30));
    return val;
}

static void rd_setbits(uint8_t reg, uint8_t mask) {
    rd_write(reg, rd_read(reg) | mask);
}
static void rd_clrbits(uint8_t reg, uint8_t mask) {
    rd_write(reg, rd_read(reg) & (~mask));
}

static void antenna_on(void) {
    if((rd_read(REG_TXCONTROL) & 0x03) != 0x03) rd_setbits(REG_TXCONTROL, 0x03);
}

static bool i2c_bus_up(void);

uint8_t rfid2_version(void) {
    return rd_read(REG_VERSION);
}

int rfid2_scan(uint8_t* out, int max) {
    if(!i2c_bus_up()) return -1;
    int n = 0;
    uint8_t dummy = 0;
    char list[128];
    int off = 0;
    list[0] = 0;
    for(uint8_t a = 0x08; a <= 0x77 && n < max; a++) {
        // a 1-byte write; ESP_OK means the address was ACKed = device present
        if(i2c_master_write_to_device(RFID2_I2C_PORT, a, &dummy, 1, pdMS_TO_TICKS(8)) == ESP_OK) {
            out[n++] = a;
            if(off < (int)sizeof(list) - 6)
                off += snprintf(list + off, sizeof(list) - off, "0x%02X ", a);
        }
    }
    ESP_LOGI(TAG, "scan: SDA=%d SCL=%d found %d device(s): [ %s]",
             RFID2_SDA_PIN, RFID2_SCL_PIN, n, list);
    return n;
}

static bool i2c_bus_up(void) {
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = RFID2_SDA_PIN,
        .scl_io_num = RFID2_SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    // The Grove bus (I2C_NUM_1, G2/G1) is normally already up — the PN532 NFC HAL
    // installs it at boot on the SAME pins. Configure best-effort and DON'T gate on
    // the result: the legacy i2c driver returns an error (ESP_FAIL) when the driver
    // already exists, but the bus still works. Real problems surface as failed reads
    // (version reads back 0x00). Both param_config and install here are best-effort.
    i2c_param_config(RFID2_I2C_PORT, &conf);
    i2c_driver_install(RFID2_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    return true;
}

bool rfid2_init(void) {
    if(!i2c_bus_up()) return false;
    rd_write(REG_COMMAND, CMD_SOFTRESET);
    vTaskDelay(pdMS_TO_TICKS(50));
    // wait for reset to clear (PowerDown bit)
    for(int i = 0; i < 20 && (rd_read(REG_COMMAND) & 0x10); i++) vTaskDelay(pdMS_TO_TICKS(5));

    // timer: TAuto, 25us..~25ms
    rd_write(REG_TMODE, 0x80);
    rd_write(REG_TPRESCALER, 0xA9);
    rd_write(REG_TRELOADH, 0x03);
    rd_write(REG_TRELOADL, 0xE8);
    rd_write(REG_TXASK, 0x40); // 100% ASK
    rd_write(REG_MODE, 0x3D); // CRC preset 0x6363
    antenna_on();

    uint8_t v = rfid2_version();
    ESP_LOGI(TAG, "version 0x%02X", v);
    return v != 0x00 && v != 0xFF; // 0x91/0x92 genuine; WS1850S clones vary but not 00/FF
}

// Transceive `send_len` bytes, collect up to `recv_max` reply bytes. `bits` is the
// number of valid bits in the last sent byte (0 = full byte). Returns recv byte count,
// or -1 on error/timeout.
static int transceive(
    const uint8_t* send,
    int send_len,
    uint8_t bits,
    uint8_t* recv,
    int recv_max) {
    rd_write(REG_COMMAND, CMD_IDLE);
    rd_write(REG_COMIRQ, 0x7F); // clear IRQs
    rd_setbits(REG_FIFOLEVEL, 0x80); // flush FIFO
    for(int i = 0; i < send_len; i++) rd_write(REG_FIFODATA, send[i]);
    rd_write(REG_BITFRAMING, bits & 0x07);
    rd_write(REG_COMMAND, CMD_TRANSCEIVE);
    rd_setbits(REG_BITFRAMING, 0x80); // StartSend

    // wait for RxIRq or IdleIRq (bit5/bit4) or timer (bit0)
    int i;
    for(i = 0; i < 50; i++) {
        uint8_t irq = rd_read(REG_COMIRQ);
        if(irq & 0x30) break; // Rx or Idle
        if(irq & 0x01) return -1; // timer expired, no card
        esp_rom_delay_us(200);
    }
    if(i >= 50) return -1;
    rd_clrbits(REG_BITFRAMING, 0x80);

    if(rd_read(REG_ERROR) & 0x13) return -1; // BufferOvfl/Coll/Protocol/Parity

    int n = rd_read(REG_FIFOLEVEL);
    if(n > recv_max) n = recv_max;
    for(int j = 0; j < n; j++) recv[j] = rd_read(REG_FIFODATA);
    return n;
}

// One anticollision cascade level. Returns 5 bytes (4 UID/CT + BCC) into out5.
static bool anticoll(uint8_t sel_cmd, uint8_t* out5) {
    rd_write(REG_COLL, 0x00); // clear ValuesAfterColl
    uint8_t cmd[2] = {sel_cmd, 0x20};
    int n = transceive(cmd, 2, 0, out5, 5);
    if(n != 5) return false;
    uint8_t bcc = out5[0] ^ out5[1] ^ out5[2] ^ out5[3];
    return bcc == out5[4];
}

bool rfid2_read_uid(uint8_t* uid, size_t uid_max, size_t* uid_len, uint8_t* sak) {
    // REQA (7-bit frame). Expect 2-byte ATQA.
    uint8_t atqa[2];
    if(transceive((const uint8_t[]){PICC_REQA}, 1, 7, atqa, 2) != 2) return false;

    size_t len = 0;
    uint8_t cl[5];

    // Cascade level 1
    if(!anticoll(PICC_ANTICOLL1, cl)) return false;
    if(cl[0] == 0x88) {
        // 7/10-byte UID: CT + 3 UID bytes here, rest in CL2
        for(int i = 1; i < 4 && len < uid_max; i++) uid[len++] = cl[i];
        uint8_t cl2[5];
        if(!anticoll(PICC_ANTICOLL2, cl2)) return false;
        for(int i = 0; i < 4 && len < uid_max; i++) uid[len++] = cl2[i];
    } else {
        for(int i = 0; i < 4 && len < uid_max; i++) uid[len++] = cl[i];
    }

    *uid_len = len;
    if(sak) *sak = 0; // full SELECT (for SAK) omitted; UID is enough for identification
    return len > 0;
}
