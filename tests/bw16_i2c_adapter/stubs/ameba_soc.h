#pragma once
#include <stdint.h>
typedef uint32_t (*IRQ_FUN)(void*);
typedef struct { uint32_t IC_INTR_MASK, IC_CON, IC_ENABLE_STATUS; } I2C_TypeDef;
typedef struct { uint32_t I2CIdx,I2CMaster,I2CAckAddr,I2CAddrMod,I2CSlvAckGC,I2CRXTL,I2CTXTL; } I2C_InitTypeDef;
extern I2C_TypeDef mock_i2c;
#define I2C0_DEV 0x40003000U
typedef struct { I2C_TypeDef* I2Cx; } I2C_DevTable;
extern const I2C_DevTable I2C_DEV_TABLE[1];
#define ENABLE 1
#define DISABLE 0
#define I2C_SLAVE_MODE 0
#define I2C_ADDR_7BIT 0
#define PINMUX_FUNCTION_I2C 3
#define GPIO_PuPd_UP 1
#define I2C0_IRQ_LP 17
#define BIT_CTRL_IC_CON_IC_SLAVE_DISABLE_1 (1U<<7)
#define BIT_IC_INTR_STAT_R_RX_UNDER (1U<<0)
#define BIT_IC_INTR_STAT_R_RX_OVER (1U<<1)
#define BIT_IC_INTR_STAT_R_RX_FULL (1U<<2)
#define BIT_IC_INTR_STAT_R_TX_OVER (1U<<3)
#define BIT_IC_INTR_STAT_R_TX_EMPTY (1U<<4)
#define BIT_IC_INTR_STAT_R_RD_REQ (1U<<5)
#define BIT_IC_INTR_STAT_R_TX_ABRT (1U<<6)
#define BIT_IC_INTR_STAT_R_RX_DONE (1U<<7)
#define BIT_IC_INTR_STAT_R_STOP_DET (1U<<9)
#define BIT_IC_INTR_MASK_M_RX_UNDER BIT_IC_INTR_STAT_R_RX_UNDER
#define BIT_IC_INTR_MASK_M_RX_OVER BIT_IC_INTR_STAT_R_RX_OVER
#define BIT_IC_INTR_MASK_M_RX_FULL BIT_IC_INTR_STAT_R_RX_FULL
#define BIT_IC_INTR_MASK_M_TX_OVER BIT_IC_INTR_STAT_R_TX_OVER
#define BIT_IC_INTR_MASK_M_TX_EMPTY BIT_IC_INTR_STAT_R_TX_EMPTY
#define BIT_IC_INTR_MASK_M_RD_REQ BIT_IC_INTR_STAT_R_RD_REQ
#define BIT_IC_INTR_MASK_M_TX_ABRT BIT_IC_INTR_STAT_R_TX_ABRT
#define BIT_IC_INTR_MASK_M_RX_DONE BIT_IC_INTR_STAT_R_RX_DONE
#define BIT_IC_INTR_MASK_M_STOP_DET BIT_IC_INTR_STAT_R_STOP_DET
#define BIT_IC_STATUS_TFNF 2U
#define BIT_IC_STATUS_TFE 4U
#define BIT_IC_STATUS_RFNE 8U
uint32_t I2C_GetINT(I2C_TypeDef*);
void I2C_Cmd(I2C_TypeDef*,int);
void I2C_ClearINT(I2C_TypeDef*,uint32_t);
void I2C_ClearAllINT(I2C_TypeDef*);
int I2C_CheckFlagState(I2C_TypeDef*,uint32_t);
uint8_t I2C_ReceiveData(I2C_TypeDef*);
void I2C_SlaveSend(I2C_TypeDef*,uint8_t);
void I2C_INTConfig(I2C_TypeDef*,uint32_t,int);
void I2C_StructInit(I2C_InitTypeDef*);
void I2C_Init(I2C_TypeDef*,I2C_InitTypeDef*);
void Pinmux_Config(unsigned,unsigned);
void PAD_PullCtrl(unsigned,unsigned);
void InterruptRegister(IRQ_FUN,unsigned,uint32_t,unsigned);
void InterruptEn(unsigned,unsigned);
