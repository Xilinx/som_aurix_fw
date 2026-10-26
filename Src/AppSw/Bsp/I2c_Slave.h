/**
 * @file    I2c_Slave.h
 * @brief   I2C0 slave for the Ryzen USB-C sideband (UsbPd_ApuProxy)
 *
 * I2C1 is APML / SB-TSI and is master-only. The sideband slave lives
 * on I2C0, the same module CPU3 uses as master to the carrier CYPD
 * controllers. Call I2cSlave_Init() from CPU3 only.
 *
 * The TC387 ADDRCFG register has one ADR field, so the hardware match
 * is 0x54 (port 0). 0x58 remains the port-1 address in the config
 * block; this peripheral cannot ACK a second slave address.
 *
 * CYPD master transfers and this slave share MODULE_I2C0.
 * I2cMaster suspends the slave around bus-0 transfers and resumes it
 * after. I2C1 never calls Suspend/Resume.
 *
 * The ISR is routed to CPU3 (protocol + FIFO service requests).
 */

#ifndef I2C_SLAVE_H
#define I2C_SLAVE_H
#include "Ifx_Types.h"
#include "IfxI2c_reg.h"

#ifndef I2C_SLAVE_FEATURE_ENABLE
#define I2C_SLAVE_FEATURE_ENABLE 1u
#endif

#define I2C_SLV_USBC0_ADDR   0x54u
#define I2C_SLV_USBC1_ADDR   0x58u   /* config only; ADDRCFG can match one address */
#define I2C0_SLAVE_ISR_PRIO  50u     /* CPU3 vector; protocol and DTR share it */
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

/* APU sideband on MODULE_I2C0. Init from CPU3, after UsbPd_ApuProxy_Init. */
void    I2cSlave_Init   (I2cSlave_ReadCb_t readCb);
boolean I2cSlave_IsBusy (void);
void    I2cSlave_Suspend(void);  /* mask CPU3 IRQ, park I2C0 as master */
void    I2cSlave_Resume (void);  /* restore 0x54 listen and CPU3 IRQ */

#endif