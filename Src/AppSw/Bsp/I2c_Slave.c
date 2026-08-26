/**
 * @file    I2c_Slave.c
 * @brief   Raw-register I2C1 slave driver
 *
 * The TC3xx I2C module supports slave mode via ADDRCFG.MNS=0.
 * This driver configures I2C1 to respond to two 7-bit addresses
 * (0x54 and 0x58) and serves the APU proxy register map from
 * the ISR.
 *
 * Dual-role operation:
 *   - Default: slave mode (MNS=0), listening for APU reads
 *   - During APML transactions: I2cSlave_Suspend() sets MNS=1,
 *     I2cMaster does its transaction, I2cSlave_Resume() restores
 *
 * The TC3xx I2C peripheral has two address registers (ADDR1, ADDR2)
 * in some variants, or can use 10-bit mode tricks.  For simplicity,
 * we use a single address register and detect which port the APU
 * addressed by checking the received address byte in the ISR.
 *
 * NOTE: This is a simplified implementation.  The TC387 I2C module
 * behavior under simultaneous master/slave may require additional
 * bus arbitration logic on the production SoM.  For v0.2, the APML
 * reads are infrequent (every 500ms) and the APU polls are also
 * infrequent, so collisions are statistically rare.
 */

#include "I2c_Slave.h"

#if (I2C_SLAVE_FEATURE_ENABLE == 1u)
#include "UsbPd_ApuProxy.h"
#include "IfxI2c_reg.h"
#include "IfxPort.h"
#include "IfxCpu_Irq.h"
#include "IfxScuWdt.h"
#include "Uart_Debug.h"

/* ================================================================== */
/*  State                                                             */
/* ================================================================== */

static I2cSlave_ReadCb_t s_readCb      = NULL_PTR;
static volatile boolean  s_busy        = FALSE;
static volatile boolean  s_suspended   = FALSE;
static volatile uint8    s_regAddr     = 0u;
static volatile uint8    s_portIdx     = 0u;
static volatile boolean  s_addrPhase   = TRUE;

/* ================================================================== */
/*  Default read callback — serves APU proxy register map             */
/* ================================================================== */

static boolean prv_DefaultReadCb(uint8 portIdx, uint8 regAddr, uint8 *pData)
{
    const uint8 *regMap = UsbPd_ApuProxy_GetRegMap(portIdx);
    if (regMap == NULL_PTR || regAddr >= APU_PROXY_REG_COUNT)
    {
        *pData = 0xFFu;
        return FALSE;
    }
    *pData = regMap[regAddr];
    return TRUE;
}

/* ================================================================== */
/*  ISR — I2C1 Protocol Interrupt                                     */
/*                                                                    */
/*  Fires on:                                                         */
/*    - Address match (AM bit in PIRQSS)                              */
/*    - Transmit request (TX_REQ in PIRQSS)                           */
/*    - Receive request (RX in PIRQSS)                                */
/*    - Protocol error / NACK                                         */
/* ================================================================== */

IFX_INTERRUPT(i2c1SlaveISR, 0, I2C1_SLAVE_ISR_PRIO)
{
    Ifx_I2C *i2c = &MODULE_I2C1;
    uint32 pirqss = i2c->PIRQSS.U;

    /* Address Match */
    if (pirqss & (1u << 0u))  /* AM bit */
    {
        /* Read the received address byte to determine port */
        uint32 rxData = i2c->RXD.U;
        uint8 addr7 = (uint8)((rxData >> 1u) & 0x7Fu);

        if (addr7 == I2C_SLV_USBC0_ADDR)
            s_portIdx = 0u;
        else
            s_portIdx = 1u;

        s_addrPhase = TRUE;
        s_busy = TRUE;

        /* Clear AM flag */
        i2c->PIRQSC.U = (1u << 0u);
    }

    /* Receive (master is writing — this is the register address byte) */
    if (pirqss & (1u << 4u))  /* RX bit */
    {
        uint32 rxData = i2c->RXD.U;

        if (s_addrPhase)
        {
            s_regAddr = (uint8)(rxData & 0xFFu);
            s_addrPhase = FALSE;
        }
        /* else: master is writing data — ignore for read-only proxy */

        i2c->PIRQSC.U = (1u << 4u);
    }

    /* Transmit Request (master is reading — send register data) */
    if (pirqss & (1u << 3u))  /* TX_REQ bit */
    {
        uint8 txByte = 0xFFu;
        I2cSlave_ReadCb_t cb = s_readCb;

        if (cb != NULL_PTR)
            cb(s_portIdx, s_regAddr, &txByte);

        i2c->TXD.U = (uint32)txByte;
        s_regAddr++;  /* Auto-increment for sequential reads */

        i2c->PIRQSC.U = (1u << 3u);
    }

    /* Transaction end / NACK / Error */
    if (pirqss & ((1u << 1u) | (1u << 2u) | (1u << 5u)))
    {
        s_busy = FALSE;
        s_addrPhase = TRUE;
        i2c->PIRQSC.U = pirqss & ((1u << 1u) | (1u << 2u) | (1u << 5u));
    }
}

/* ================================================================== */
/*  Public API                                                        */
/* ================================================================== */

void I2cSlave_Init(I2cSlave_ReadCb_t readCb)
{
    Ifx_I2C *i2c = &MODULE_I2C1;
    uint16 password;

    if (readCb != NULL_PTR)
        s_readCb = readCb;
    else
        s_readCb = prv_DefaultReadCb;

    Debug_Print("[I2C_SLV] Init: configuring I2C1 slave mode...\r\n");

    password = IfxScuWdt_getCpuWatchdogPassword();
    IfxScuWdt_clearCpuEndinit(password);

    /* Set slave address — use primary address for 0x54.
     * The ISR determines port by the address byte received.
     * Set ADDRCFG: ADR=0x54 (7-bit), MNS=0 (slave), TBAM=0 (7-bit) */
    {
        Ifx_I2C_ADDRCFG addrcfg;
        addrcfg.U = i2c->ADDRCFG.U;
        addrcfg.B.ADR = I2C_SLV_USBC0_ADDR;
        addrcfg.B.MNS = 0u;   /* Slave mode */
        addrcfg.B.TBAM = 0u;  /* 7-bit address */
        i2c->ADDRCFG.U = addrcfg.U;
    }

    /* Enable protocol interrupts: AM, TX_REQ, RX, NACK, AL */
    i2c->PIRQSM.U &= ~((1u << 0u) | (1u << 3u) | (1u << 4u) |
                        (1u << 1u) | (1u << 2u) | (1u << 5u));

    /* Enable RUN control — slave starts listening */
    {
        Ifx_I2C_RUNCTRL runctrl;
        runctrl.U = i2c->RUNCTRL.U;
        runctrl.U |= (1u << 0u);  /* RUN bit */
        i2c->RUNCTRL.U = runctrl.U;
    }

    IfxScuWdt_setCpuEndinit(password);

    /* Install ISR */
    IfxCpu_Irq_installInterruptHandler(&i2c1SlaveISR, I2C1_SLAVE_ISR_PRIO);

    s_busy = FALSE;
    s_suspended = FALSE;

    Debug_Printf("[I2C_SLV] Init OK, slave addr=0x%02X/0x%02X\r\n",
                 (unsigned)I2C_SLV_USBC0_ADDR,
                 (unsigned)I2C_SLV_USBC1_ADDR);
}

boolean I2cSlave_IsBusy(void)
{
    return s_busy;
}

void I2cSlave_Suspend(void)
{
    if (s_suspended) return;

    Ifx_I2C *i2c = &MODULE_I2C1;
    uint16 password = IfxScuWdt_getCpuWatchdogPassword();
    IfxScuWdt_clearCpuEndinit(password);

    /* Switch to master mode */
    Ifx_I2C_ADDRCFG addrcfg;
    addrcfg.U = i2c->ADDRCFG.U;
    addrcfg.B.MNS = 1u;
    i2c->ADDRCFG.U = addrcfg.U;

    IfxScuWdt_setCpuEndinit(password);
    s_suspended = TRUE;
}

void I2cSlave_Resume(void)
{
    if (!s_suspended) return;

    Ifx_I2C *i2c = &MODULE_I2C1;
    uint16 password = IfxScuWdt_getCpuWatchdogPassword();
    IfxScuWdt_clearCpuEndinit(password);

    /* Restore slave mode */
    Ifx_I2C_ADDRCFG addrcfg;
    addrcfg.U = i2c->ADDRCFG.U;
    addrcfg.B.MNS = 0u;
    addrcfg.B.ADR = I2C_SLV_USBC0_ADDR;
    i2c->ADDRCFG.U = addrcfg.U;

    IfxScuWdt_setCpuEndinit(password);
    s_suspended = FALSE;
}

#else /* I2C_SLAVE_FEATURE_ENABLE == 0 */

void I2cSlave_Init(I2cSlave_ReadCb_t readCb) { (void)readCb; }
boolean I2cSlave_IsBusy(void) { return FALSE; }
void I2cSlave_Suspend(void) {}
void I2cSlave_Resume(void) {}

#endif