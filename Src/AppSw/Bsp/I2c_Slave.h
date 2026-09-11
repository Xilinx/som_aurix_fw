/**
 * @file    I2c_Slave.h
 * @brief   I2C1 slave driver for APU USB-C sideband proxy
 *
 * The TC3xx I2C peripheral supports simultaneous master and slave
 * operation.  This driver configures I2C1 to respond to slave
 * addresses 0x54 and 0x58 while the existing I2c_Master driver
 * continues to use I2C1 for APML reads.
 *
 * The iLLD does not provide an I2C slave wrapper, so this driver
 * accesses the I2C1 registers directly.
 *
 * When the APU reads from 0x54, the slave serves the port-0
 * APU proxy register map.  When it reads 0x58, port-1.
 *
 * The slave response is interrupt-driven: the Protocol Interrupt
 * (I2C_PIRQSM) fires on address match, and the slave loads the
 * TX FIFO with the proxy register bytes.
 *
 * IMPORTANT: On the eval board (TARGET_EVAL_BOARD), this is
 * compiled but I2C1 has no APU connected.  The slave address
 * match will simply never fire.
 */

#ifndef I2C_SLAVE_H
#define I2C_SLAVE_H
#include "Ifx_Types.h"
#include "IfxI2c_reg.h"

#ifndef I2C_SLAVE_FEATURE_ENABLE
#define I2C_SLAVE_FEATURE_ENABLE 1u
#endif

#define I2C_SLV_USBC0_ADDR   0x54u
#define I2C_SLV_USBC1_ADDR   0x58u
#define I2C1_SLAVE_ISR_PRIO  50u
#define I2C_SLV_LOG_LEN      128u

typedef boolean (*I2cSlave_ReadCb_t)(uint8 portIdx, uint8 regAddr, uint8 *pData);

typedef struct
{
    uint32 tMs;      /* time of transaction               */
    uint8  reg;      /* register address byte             */
    uint8  data;     /* data byte (0xFF if none)          */
    uint8  isWrite;  /* 1 = master wrote, 0 = master read */
} I2cSlave_LogEntry_t;

typedef struct
{
    Ifx_I2C          *mod;
    uint8             addr7;
    uint8             addrShifted;     /* 1: ADR = addr7<<1, 0: ADR = addr7 */
    I2cSlave_ReadCb_t readCb;
    uint8             regFile[256];    /* retimer emulation: writes land here, reads served from here */
    /* transaction state */
    volatile boolean  busy, addrPhase;
    volatile uint8    regAddr;
    /* stats + log */
    volatile uint32   matches, rxBytes, txBytes, nacks;
    I2cSlave_LogEntry_t log[I2C_SLV_LOG_LEN];
    volatile uint32   logCount;
} I2cSlave_Inst_t;

/* generic instance API (used by CLI on I2C0) */
void    I2cSlave_Setup (I2cSlave_Inst_t *s, Ifx_I2C *mod, uint8 addr7, uint8 addrShifted, I2cSlave_ReadCb_t cb);
void    I2cSlave_Start (I2cSlave_Inst_t *s);
void    I2cSlave_Stop  (I2cSlave_Inst_t *s);        /* leaves module in master mode, RUN=1 */
void    I2cSlave_Poll  (I2cSlave_Inst_t *s);        /* call in a loop; services AM/RX/TX/errors */

/* legacy I2C1 / APU proxy API (unchanged behaviour) */
void    I2cSlave_Init   (I2cSlave_ReadCb_t readCb);
boolean I2cSlave_IsBusy (void);
void    I2cSlave_Suspend(void);
void    I2cSlave_Resume (void);

#endif