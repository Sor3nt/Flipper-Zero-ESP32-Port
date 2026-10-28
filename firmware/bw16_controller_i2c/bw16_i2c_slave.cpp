/* SPDX-License-Identifier: GPL-3.0-or-later
 * Local AmebaD 3.1.9 I2C0 slave. Does not use Wire or the SDK slave task.
 * Every IRQ services at most 64 RX + 64 TX bytes, with no waits/allocations.
 * Host writes exactly 64 bytes with STOP, then reads exactly 64 with NACK+STOP.
 * Write->read repeated START is also accepted. Other incomplete transactions
 * are discarded; retries are deduplicated by the link protocol above this layer.
 */
#include "bw16_i2c_slave.h"
#include <Arduino.h>
extern "C" {
#include <ameba_soc.h>
#include <PinNames.h>
#include <FreeRTOS.h>
#include <task.h>
}
#include <string.h>

static I2C_TypeDef* i2c_device;
static uint8_t rx_frame[BWI_HW_FRAME_SIZE], tx_frame[BWI_HW_FRAME_SIZE];
static unsigned rx_used, tx_used;
static bool rx_overflow, tx_active, transaction_active;
static volatile bool recovering;
static uint32_t transaction_started;
static BwiHwWrite on_write;
static BwiHwRead on_read;
static const uint32_t base_mask = BIT_IC_INTR_MASK_M_RX_FULL |
    BIT_IC_INTR_MASK_M_RX_OVER | BIT_IC_INTR_MASK_M_RX_UNDER |
    BIT_IC_INTR_MASK_M_RD_REQ | BIT_IC_INTR_MASK_M_RX_DONE |
    BIT_IC_INTR_MASK_M_TX_ABRT | BIT_IC_INTR_MASK_M_TX_OVER |
    BIT_IC_INTR_MASK_M_STOP_DET;

static void reset_transaction() {
    rx_used = tx_used = 0; rx_overflow = tx_active = transaction_active = false;
}
static void begin_recovery() {
    /* Disable flushes both FIFOs. IC_ENABLE_STATUS may settle asynchronously. */
    i2c_device->IC_INTR_MASK = 0;
    I2C_Cmd(i2c_device, DISABLE);
    recovering = true;
    reset_transaction();
}
static void finish_write() {
    if(rx_used == BWI_HW_FRAME_SIZE && !rx_overflow && on_write) on_write(rx_frame);
    rx_used = 0; rx_overflow = false;
}
static void fill_tx() {
    for(unsigned budget = 0; budget < BWI_HW_FRAME_SIZE && tx_used < BWI_HW_FRAME_SIZE; ++budget) {
        if(!I2C_CheckFlagState(i2c_device, BIT_IC_STATUS_TFNF)) break;
        I2C_SlaveSend(i2c_device, tx_frame[tx_used++]);
    }
    I2C_INTConfig(i2c_device, BIT_IC_INTR_MASK_M_TX_EMPTY,
                  tx_used < BWI_HW_FRAME_SIZE ? ENABLE : DISABLE);
}
static uint32_t slave_irq(void*) {
    if(recovering) return 0;
    const uint32_t status = I2C_GetINT(i2c_device);
    if(!transaction_active && (status & (BIT_IC_INTR_STAT_R_RX_FULL | BIT_IC_INTR_STAT_R_RD_REQ))) {
        transaction_active = true; transaction_started = millis();
    }
    if(status & BIT_IC_INTR_STAT_R_RX_OVER) {
        rx_overflow = true;
        I2C_ClearINT(i2c_device, BIT_IC_INTR_STAT_R_RX_OVER);
    }
    if(status & BIT_IC_INTR_STAT_R_RX_UNDER) I2C_ClearINT(i2c_device, BIT_IC_INTR_STAT_R_RX_UNDER);
    /* Drain before STOP/RD_REQ so a final short FIFO batch belongs to this write. */
    for(unsigned budget = 0; budget < BWI_HW_FRAME_SIZE; ++budget) {
        if(!I2C_CheckFlagState(i2c_device, BIT_IC_STATUS_RFNE)) break;
        const uint8_t byte = I2C_ReceiveData(i2c_device);
        if(rx_used < BWI_HW_FRAME_SIZE) rx_frame[rx_used++] = byte;
        else rx_overflow = true;
    }
    if(I2C_CheckFlagState(i2c_device, BIT_IC_STATUS_RFNE)) rx_overflow = true;
    /* A new slave-read request can flush stale TX FIFO data through TX_ABRT.
     * Clear it before filling; do not mistake that flush for a new link session. */
    if(status & BIT_IC_INTR_STAT_R_TX_ABRT) {
        I2C_ClearINT(i2c_device, BIT_IC_INTR_STAT_R_TX_ABRT);
        tx_active = false; tx_used = 0;
        I2C_INTConfig(i2c_device, BIT_IC_INTR_MASK_M_TX_EMPTY, DISABLE);
    }
    if(status & BIT_IC_INTR_STAT_R_TX_OVER) {
        I2C_ClearINT(i2c_device, BIT_IC_INTR_STAT_R_TX_OVER);
        begin_recovery(); return 0;
    }
    if(status & BIT_IC_INTR_STAT_R_RX_DONE) {
        /* Master NACK: no more bytes may be queued for this read. STOP below
         * flushes any unread prefetched bytes, including an aborted short read. */
        I2C_ClearINT(i2c_device, BIT_IC_INTR_STAT_R_RX_DONE);
        I2C_INTConfig(i2c_device, BIT_IC_INTR_MASK_M_TX_EMPTY, DISABLE);
        tx_active = false;
    }
    if(status & BIT_IC_INTR_STAT_R_STOP_DET) {
        const bool had_read = tx_used != 0;
        I2C_ClearINT(i2c_device, BIT_IC_INTR_STAT_R_STOP_DET);
        if(rx_used || rx_overflow) finish_write();
        transaction_active = false;
        if(had_read) {
            /* Normal64-byte NACK+STOP consumed every queued byte. TFE is the
             * peripheral's TX-FIFO-empty bit: no stale byte remains to flush.
             * Keep the slave enabled for an immediate next host transaction. */
            if(tx_used == BWI_HW_FRAME_SIZE && I2C_CheckFlagState(i2c_device, BIT_IC_STATUS_TFE)) {
                I2C_INTConfig(i2c_device, BIT_IC_INTR_MASK_M_TX_EMPTY, DISABLE);
                reset_transaction();
            } else { begin_recovery(); return 0; }
        }
    }
    if(status & BIT_IC_INTR_STAT_R_RD_REQ) {
        if(rx_used || rx_overflow) finish_write();
        if(!tx_active) {
            memset(tx_frame, 0, sizeof(tx_frame));
            if(on_read) on_read(tx_frame);
            tx_used = 0; tx_active = true;
        }
        fill_tx();
        /* An invalid overread gets bounded zero padding, never a clock-stretch
         * spin or the beginning of a second protocol frame. */
        if(tx_used == BWI_HW_FRAME_SIZE && I2C_CheckFlagState(i2c_device, BIT_IC_STATUS_TFE))
            I2C_SlaveSend(i2c_device, 0);
        I2C_ClearINT(i2c_device, BIT_IC_INTR_STAT_R_RD_REQ);
    } else if((status & BIT_IC_INTR_STAT_R_TX_EMPTY) && tx_active) {
        fill_tx();
    }
    return 0;
}
void bwi_hw_begin(BwiHwWrite write_callback, BwiHwRead read_callback) {
    i2c_device = I2C_DEV_TABLE[0].I2Cx;
    on_write = write_callback; on_read = read_callback;
    reset_transaction(); recovering = false;
    I2C_InitTypeDef config;
    memset(&config, 0, sizeof(config));
    I2C_StructInit(&config);
    config.I2CIdx = 0; config.I2CMaster = I2C_SLAVE_MODE;
    config.I2CAckAddr = BWI_HW_ADDRESS; config.I2CAddrMod = I2C_ADDR_7BIT;
    config.I2CSlvAckGC = DISABLE; config.I2CRXTL = 0; config.I2CTXTL = 0;
    Pinmux_Config(PA_26, PINMUX_FUNCTION_I2C);
    Pinmux_Config(PA_25, PINMUX_FUNCTION_I2C);
    PAD_PullCtrl(PA_26, GPIO_PuPd_UP); PAD_PullCtrl(PA_25, GPIO_PuPd_UP);
    I2C_Init(i2c_device, &config);
    /* SDK initializes two slave addresses: only our 0x42 address may ACK. */
    i2c_device->IC_CON |= BIT_CTRL_IC_CON_IC_SLAVE_DISABLE_1;
    I2C_ClearAllINT(i2c_device);
    InterruptRegister((IRQ_FUN)slave_irq, I2C0_IRQ_LP, 0, 7);
    i2c_device->IC_INTR_MASK = base_mask;
    InterruptEn(I2C0_IRQ_LP, 7);
    I2C_Cmd(i2c_device, ENABLE);
}
void bwi_hw_poll(uint32_t now) {
    taskENTER_CRITICAL();
    if(!recovering && transaction_active && (uint32_t)(now - transaction_started) >= BWI_HW_TRANSACTION_MS)
        begin_recovery();
    if(recovering && !(i2c_device->IC_ENABLE_STATUS & 1U)) {
        I2C_ClearAllINT(i2c_device);
        reset_transaction(); recovering = false;
        I2C_Cmd(i2c_device, ENABLE);
        i2c_device->IC_INTR_MASK = base_mask;
    }
    taskEXIT_CRITICAL();
}
