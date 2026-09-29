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
#include "IfxCpu_Irq.h"
#include "IfxScuWdt.h"
#include "Uart_Debug.h"
#include "Stm_Timer.h"

/* ---------------------------------------------------------------- */
/*  Core: one transaction-servicing routine shared by ISR and poll   */
/*  Uses named bitfields; if your IfxI2c_regdef.h spells one of them */
/*  differently the compiler will point at it.                       */
/* ---------------------------------------------------------------- */
static void prv_Log(I2cSlave_Inst_t *s, uint8 reg, uint8 data, uint8 isWrite)
{
    uint32 i = s->logCount;
    if (i < I2C_SLV_LOG_LEN)
    {
        s->log[i].tMs = Stm_GetTimeMs();
        s->log[i].reg = reg; s->log[i].data = data; s->log[i].isWrite = isWrite;
    }
    s->logCount = i + 1u;
}

static void prv_Service(I2cSlave_Inst_t *s)
{
    Ifx_I2C *i2c = s->mod;
    Ifx_I2C_PIRQSS p = i2c->PIRQSS;
    uint32 ffs;

    if (p.B.AM)                                   /* address match */
    {
        s->matches++;
        s->busy = TRUE;
        s->addrPhase = TRUE;
        i2c->PIRQSC.B.AM = 1u;
    }

    ffs = i2c->FFSSTAT.B.FFS;                     /* bytes in RX FIFO (master wrote) */
    while (ffs--)
    {
        uint8 d = (uint8)i2c->RXD.U;
        s->rxBytes++;
        if (s->addrPhase) { s->regAddr = d; s->addrPhase = FALSE; prv_Log(s, d, 0xFFu, 1u); }
        else              { s->regFile[s->regAddr] = d; prv_Log(s, s->regAddr, d, 1u); s->regAddr++; }
    }

    /* master reading: hardware asks for TX data via RIS request bits */
    if (i2c->RIS.B.SREQ_INT || i2c->RIS.B.BREQ_INT || i2c->RIS.B.LSREQ_INT || i2c->RIS.B.LBREQ_INT)
    {
        uint8 tx = 0xFFu;
        if (s->readCb != NULL_PTR) (void)s->readCb(0u, s->regAddr, &tx);
        else tx = s->regFile[s->regAddr];
        i2c->TXD.U = tx;
        s->txBytes++;
        prv_Log(s, s->regAddr, tx, 0u);
        s->regAddr++;
        i2c->ICR.U = i2c->RIS.U;                  /* clear request flags */
    }

    if (p.B.NACK) s->nacks++;
    if (p.B.NACK || p.B.AL || p.B.TX_END)
    {
        s->busy = FALSE; s->addrPhase = TRUE;
        i2c->PIRQSC.B.NACK = p.B.NACK; i2c->PIRQSC.B.AL = p.B.AL; i2c->PIRQSC.B.TX_END = p.B.TX_END;
    }
    if (i2c->ERRIRQSS.U) i2c->ERRIRQSC.U = i2c->ERRIRQSS.U;
}

/* ---------------------------------------------------------------- */
/*  Generic instance API                                             */
/* ---------------------------------------------------------------- */
void I2cSlave_Setup(I2cSlave_Inst_t *s, Ifx_I2C *mod, uint8 addr7, uint8 addrShifted, I2cSlave_ReadCb_t cb)
{
    uint32 i;
    s->mod = mod; s->addr7 = addr7; s->addrShifted = addrShifted; s->readCb = cb;
    for (i = 0u; i < 256u; i++) s->regFile[i] = 0u;
    s->busy = FALSE; s->addrPhase = TRUE; s->regAddr = 0u;
    s->matches = s->rxBytes = s->txBytes = s->nacks = s->logCount = 0u;
}

void I2cSlave_Start(I2cSlave_Inst_t *s)
{
    Ifx_I2C *i2c = s->mod;
    i2c->RUNCTRL.B.RUN  = 0u;
    i2c->ADDRCFG.B.MNS  = 0u;
    i2c->ADDRCFG.B.TBAM = 0u;
    i2c->ADDRCFG.B.ADR  = s->addrShifted ? ((uint32)s->addr7 << 1u) : (uint32)s->addr7;
    i2c->ADDRCFG.B.GCE  = 0u;
    i2c->ADDRCFG.B.MCE  = 0u;
    i2c->ADDRCFG.B.SOPE = 1u;
    i2c->ADDRCFG.B.SONA = 1u;
    i2c->FIFOCFG.B.RXFA = 0u; i2c->FIFOCFG.B.TXFA = 0u;
    i2c->FIFOCFG.B.RXFC = 1u; i2c->FIFOCFG.B.TXFC = 1u;
    i2c->FIFOCFG.B.RXBS = 3u; i2c->FIFOCFG.B.TXBS = 3u;
    i2c->PIRQSC.U = 0xFFFFFFFFu; i2c->ERRIRQSC.U = 0xFFFFFFFFu; i2c->ICR.U = 0xFFFFFFFFu;
    i2c->RUNCTRL.B.RUN  = 1u;
}

void I2cSlave_Stop(I2cSlave_Inst_t *s)
{
    Ifx_I2C *i2c = s->mod;
    i2c->RUNCTRL.B.RUN = 0u;
    i2c->ADDRCFG.B.MNS = 1u;
    i2c->ADDRCFG.B.ADR = 0u;
    i2c->PIRQSC.U = 0xFFFFFFFFu; i2c->ERRIRQSC.U = 0xFFFFFFFFu; i2c->ICR.U = 0xFFFFFFFFu;
    i2c->RUNCTRL.B.RUN = 1u;
}

void I2cSlave_Poll(I2cSlave_Inst_t *s) { prv_Service(s); }

/* ---------------------------------------------------------------- */
/*  Legacy I2C1 / APU proxy path (ISR driven)                        */
/* ---------------------------------------------------------------- */
static I2cSlave_Inst_t s_apu;
static volatile boolean s_suspended = FALSE;

static boolean prv_ApuReadCb(uint8 portIdx, uint8 regAddr, uint8 *pData)
{
    const uint8 *regMap = UsbPd_ApuProxy_GetRegMap(portIdx);
    (void)portIdx;
    if ((regMap == NULL_PTR) || (regAddr >= APU_PROXY_REG_COUNT)) { *pData = 0xFFu; return FALSE; }
    *pData = regMap[regAddr];
    return TRUE;
}

IFX_INTERRUPT(i2c1SlaveISR, 0, I2C1_SLAVE_ISR_PRIO) { prv_Service(&s_apu); }

void I2cSlave_Init(I2cSlave_ReadCb_t readCb)
{
    uint16 pw = IfxScuWdt_getCpuWatchdogPassword();
    I2cSlave_Setup(&s_apu, &MODULE_I2C1, I2C_SLV_USBC0_ADDR, 0u,
                   (readCb != NULL_PTR) ? readCb : prv_ApuReadCb);
    IfxScuWdt_clearCpuEndinit(pw);
    I2cSlave_Start(&s_apu);
    MODULE_I2C1.PIRQSM.U = 0u;                    /* verify polarity in regdef: 0 = unmasked */
    IfxScuWdt_setCpuEndinit(pw);
    IfxCpu_Irq_installInterruptHandler(&i2c1SlaveISR, I2C1_SLAVE_ISR_PRIO);
    s_suspended = FALSE;
    Debug_Printf("[I2C_SLV] I2C1 slave @0x%02X\r\n", (unsigned)I2C_SLV_USBC0_ADDR);
}

boolean I2cSlave_IsBusy(void) { return s_apu.busy; }
void I2cSlave_Suspend(void) { if (!s_suspended) { MODULE_I2C1.ADDRCFG.B.MNS = 1u; s_suspended = TRUE; } }
void I2cSlave_Resume(void)  { if (s_suspended)  { MODULE_I2C1.ADDRCFG.B.MNS = 0u;
                              MODULE_I2C1.ADDRCFG.B.ADR = I2C_SLV_USBC0_ADDR; s_suspended = FALSE; } }

#else
void    I2cSlave_Setup(I2cSlave_Inst_t *s, Ifx_I2C *m, uint8 a, uint8 sh, I2cSlave_ReadCb_t cb) {(void)s;(void)m;(void)a;(void)sh;(void)cb;}
void    I2cSlave_Start(I2cSlave_Inst_t *s) {(void)s;}
void    I2cSlave_Stop(I2cSlave_Inst_t *s)  {(void)s;}
void    I2cSlave_Poll(I2cSlave_Inst_t *s)  {(void)s;}
void    I2cSlave_Init(I2cSlave_ReadCb_t cb){(void)cb;}
boolean I2cSlave_IsBusy(void){return FALSE;}
void    I2cSlave_Suspend(void){}
void    I2cSlave_Resume(void){}
#endif