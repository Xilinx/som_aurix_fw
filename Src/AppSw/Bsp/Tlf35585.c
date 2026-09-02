/**
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/**
 * @file    Tlf35585.c
 * @brief   TLF35585/TLF35585 PMIC driver — 16-bit SPI protocol
 *
 * Based on Infineon code example SPI_TLF_1_KIT_TC397_TFT.
 *
 * Key differences from the previous 32-bit driver:
 *   - dataWidth = 15 (QSPI auto-inserts parity → 16 clocks)
 *   - HW parity (parityCheck=TRUE, even parity)
 *   - 8-bit register data, not 16-bit
 *   - Separate read/write register addresses
 *   - No software parity calculation
 *
 * QSPI2 pin mapping:
 *   SCLK : P15.3 (both eval and SoM)
 *   MTSR : P15.6 (both)
 *   MRST : P15.7 (both)
 *   CS   : P14.2 / SLSO1 (eval)  or  P15.2 / SLSO0 (SoM)
 */

#include "Tlf35585.h"
#include "Platform_PinCfg.h"
#include "AppPin.h"
#include "Stm_Timer.h"
#include "Uart_Debug.h"
#include "IfxPort.h"
#include "IfxQspi_SpiMaster.h"
#include "IfxCpu_Irq.h"

/* ================================================================== */
/*  SPI configuration constants                                       */
/* ================================================================== */
#define QSPI2_MODULE            &MODULE_QSPI2
#define SPI_BAUDRATE            5000000.0f      /* 5 MHz per TLF DS */
#define SPI_DATA_WIDTH          15u             /* 15 data bits + 1 HW parity = 16 clocks */
#define SPI_BUFFER_SIZE         1u              /* One frame at a time */

#define QSPI2_TX_ISR_PRIO      30u
#define QSPI2_RX_ISR_PRIO      31u
#define QSPI2_ER_ISR_PRIO      32u

#define IE_PROBE(tag) { uint8 ie; Tlf35585_ReadReg(TLF_RW_INITERR, &ie); \
                            Debug_Printf("[TLF] IE@%s=0x%02X\r\n", tag, (unsigned)ie); }

/* ================================================================== */
/*  Driver state                                                      */
/* ================================================================== */
static IfxQspi_SpiMaster         s_spiMaster;
static IfxQspi_SpiMaster_Channel s_spiChannel;
static boolean                   s_initialised       = FALSE;

/* SPI TX/RX buffers (uint16 for 15-bit + parity frames) */
static uint16                    s_txBuf[SPI_BUFFER_SIZE];
static uint16                    s_rxBuf[SPI_BUFFER_SIZE];

/* Window watchdog timing */
static uint32                    s_wdtLastServiceMs   = 0u;
static uint32                    s_wdtOpenWindowMs    = 200u;
static uint32                    s_wdtClosedWindowMs  = 10u;
static uint32                    s_wdtServiceTargetMs = 0u;
static boolean                   s_wdiPinState        = FALSE;

/* Fault monitoring */
static Tlf35585_FaultCb_t        s_faultCb            = NULL_PTR;
static uint32                    s_lastFaultPollMs    = 0u;

/* Counters */
static uint32                    s_wwdServiceCount    = 0u;
static uint32                    s_wwdMissCount       = 0u;
static uint8                     s_devctrlShadow      = 0u;
static uint32                    s_wwdRecoveredCount = 0u;
/* ================================================================== */
/*  ISRs — must be above prv_SpiInit so they're visible               */
/* ================================================================== */
IFX_INTERRUPT(qspi2TxISR, 0, QSPI2_TX_ISR_PRIO)
{
    IfxCpu_enableInterrupts();
    IfxQspi_SpiMaster_isrTransmit(&s_spiMaster);
}

IFX_INTERRUPT(qspi2RxISR, 0, QSPI2_RX_ISR_PRIO)
{
    IfxCpu_enableInterrupts();
    IfxQspi_SpiMaster_isrReceive(&s_spiMaster);
}

IFX_INTERRUPT(qspi2ErISR, 0, QSPI2_ER_ISR_PRIO)
{
    IfxCpu_enableInterrupts();
    IfxQspi_SpiMaster_isrError(&s_spiMaster);
}

/* ================================================================== */
/*  SPI frame builder                                                 */
/*                                                                    */
/*  Packs CMD + ADDR + DATA into a 15-bit value.  The QSPI hardware  */
/*  inserts the parity bit at bit position 8 automatically, so the    */
/*  on-wire frame is 16 bits.                                         */
/*                                                                    */
/*  Bitfield layout (15 bits, MSB first):                             */
/*    [14]   = CMD (0=read, 1=write)                                  */
/*    [13:8] = ADDR (6 bits)                                          */
/*    [7:0]  = DATA (8 bits)                                          */
/*  HW inserts parity between ADDR and DATA on the wire.              */
/* ================================================================== */
static uint16 prv_BuildFrame(boolean write, uint8 addr, uint8 data)
{
    uint16 frame = 0u;
    frame |= ((uint16)data);                      /* bits [7:0]  */
    frame |= ((uint16)(addr & 0x3Fu) << 8u);      /* bits [13:8] */
    if (write)
        frame |= (1u << 14u);                     /* bit [14]    */
    return frame;
}

/* ================================================================== */
/*  Low-level SPI transfer                                            */
/* ================================================================== */
static Tlf35585_Status_t prv_SpiTransfer(uint16 txFrame, uint16 *rxFrame)
{
    uint32 startMs;

    s_txBuf[0] = txFrame;
    s_rxBuf[0] = 0u;

    IfxQspi_SpiMaster_exchange(&s_spiChannel, s_txBuf, s_rxBuf, SPI_BUFFER_SIZE);

    startMs = Stm_GetTimeMs();

    if (IfxCpu_getCoreIndex() == 2u)
    {
        /* CPU2, post-handover: ISRs on this core advance the state machine */
        while (IfxQspi_SpiMaster_getStatus(&s_spiChannel) == IfxQspi_Status_busy)
        {
            if ((Stm_GetTimeMs() - startMs) > 5u)
                return TLF_ERR_SPI_TIMEOUT;
        }
    }
    else
    {
        /* CPU0, pre-handover (incl. under disabled interrupts): pump SRR */
        while (IfxQspi_SpiMaster_getStatus(&s_spiChannel) == IfxQspi_Status_busy)
        {
            if (SRC_QSPI_QSPI2_TX.B.SRR)
            {
                IfxSrc_clearRequest(&SRC_QSPI_QSPI2_TX);
                IfxQspi_SpiMaster_isrTransmit(&s_spiMaster);
            }
            if (SRC_QSPI_QSPI2_RX.B.SRR)
            {
                IfxSrc_clearRequest(&SRC_QSPI_QSPI2_RX);
                IfxQspi_SpiMaster_isrReceive(&s_spiMaster);
            }
            if (SRC_QSPI_QSPI2_ERR.B.SRR)
            {
                IfxSrc_clearRequest(&SRC_QSPI_QSPI2_ERR);
                IfxQspi_SpiMaster_isrError(&s_spiMaster);
            }
            if ((Stm_GetTimeMs() - startMs) > 5u)
                return TLF_ERR_SPI_TIMEOUT;
        }
        /* belt-and-braces: drain a pending RX event getStatus may have
         * raced past, so the received frame lands in s_rxBuf */
        if (SRC_QSPI_QSPI2_RX.B.SRR)
        {
            IfxSrc_clearRequest(&SRC_QSPI_QSPI2_RX);
            IfxQspi_SpiMaster_isrReceive(&s_spiMaster);
        }
    }

    if (rxFrame != NULL_PTR)
        *rxFrame = s_rxBuf[0];

    return TLF_OK;
}

/* ================================================================== */
/*  QSPI2 hardware init                                               */
/* ================================================================== */
static Tlf35585_Status_t prv_SpiInit(void)
{
    /* ---- Master config ---- */
    IfxQspi_SpiMaster_Config masterCfg;
    IfxQspi_SpiMaster_initModuleConfig(&masterCfg, QSPI2_MODULE);

    masterCfg.mode = IfxQspi_Mode_master;

    const IfxQspi_SpiMaster_Pins qspi2Pins = {
        &IfxQspi2_SCLK_P15_3_OUT,  IfxPort_OutputMode_pushPull,
        &IfxQspi2_MTSR_P15_6_OUT,  IfxPort_OutputMode_pushPull,
        &IfxQspi2_MRSTB_P15_7_IN,  IfxPort_InputMode_pullDown,
        IfxPort_PadDriver_cmosAutomotiveSpeed3
    };
    masterCfg.pins = &qspi2Pins;

    masterCfg.txPriority  = QSPI2_TX_ISR_PRIO;
    masterCfg.rxPriority  = QSPI2_RX_ISR_PRIO;
    masterCfg.erPriority  = QSPI2_ER_ISR_PRIO;
    masterCfg.isrProvider = IfxSrc_Tos_cpu0;

    IfxQspi_SpiMaster_initModule(&s_spiMaster, &masterCfg);
    IfxSrc_disable(&SRC_QSPI_QSPI2_TX);
    IfxSrc_disable(&SRC_QSPI_QSPI2_RX);
    IfxSrc_disable(&SRC_QSPI_QSPI2_ERR);
    IfxCpu_Irq_installInterruptHandler(&qspi2TxISR, QSPI2_TX_ISR_PRIO);
    IfxCpu_Irq_installInterruptHandler(&qspi2RxISR, QSPI2_RX_ISR_PRIO);
    IfxCpu_Irq_installInterruptHandler(&qspi2ErISR, QSPI2_ER_ISR_PRIO);

    /* ---- Channel config ---- */
    IfxQspi_SpiMaster_ChannelConfig chCfg;
    IfxQspi_SpiMaster_initChannelConfig(&chCfg, &s_spiMaster);

    chCfg.ch.baudrate                = SPI_BAUDRATE;
    chCfg.ch.mode.dataWidth          = SPI_DATA_WIDTH;    /* 15 bits (HW adds parity → 16 clocks) */
    chCfg.ch.mode.shiftClock         = IfxQspi_ShiftClock_shiftTransmitDataOnTrailingEdge;
    chCfg.ch.mode.parityCheck        = TRUE;              /* HW parity generation */
    chCfg.ch.mode.parityMode         = IfxQspi_ParityMode_even;
    chCfg.ch.mode.csTrailDelay       = 2u;
    chCfg.ch.mode.csInactiveDelay    = 2u;

    /* Slave select — different pin on eval vs SoM */
    {
#if defined(TARGET_EVAL_BOARD)
        const IfxQspi_SpiMaster_Output slaveSelect = {
            &IfxQspi2_SLSO1_P14_2_OUT, IfxPort_OutputMode_pushPull,
            IfxPort_PadDriver_cmosAutomotiveSpeed1
        };
#else
        const IfxQspi_SpiMaster_Output slaveSelect = {
            &IfxQspi2_SLSO0_P15_2_OUT, IfxPort_OutputMode_pushPull,
            IfxPort_PadDriver_cmosAutomotiveSpeed1
        };
#endif
        chCfg.sls.output = slaveSelect;
    }

    IfxQspi_SpiMaster_initChannel(&s_spiChannel, &chCfg);

    return TLF_OK;
}

/* ================================================================== */
/*  Unlock / lock protected registers                                 */
/* ================================================================== */
static Tlf35585_Status_t prv_Unlock(void)
{
    uint8 p = 0u;
    Tlf35585_WriteReg(TLF_W_PROTCFG, TLF_UNLOCK_KEY0);
    Tlf35585_WriteReg(TLF_W_PROTCFG, TLF_UNLOCK_KEY1);
    Tlf35585_WriteReg(TLF_W_PROTCFG, TLF_UNLOCK_KEY2);
    Tlf35585_WriteReg(TLF_W_PROTCFG, TLF_UNLOCK_KEY3);
    Tlf35585_ReadReg(TLF_R_PROTSTAT, &p);
    if (p != 0xF0u)
    {
        Debug_Printf("[TLF] ERROR: unlock rejected, PROTSTAT=0x%02X\r\n", (unsigned)p);
        return TLF_ERR_PROT;
    }
    return TLF_OK;
}

static Tlf35585_Status_t prv_Lock(void)
{
    uint8 p = 0u, spisf = 0u, syssf = 0u;
    Tlf35585_WriteReg(TLF_W_PROTCFG, TLF_LOCK_KEY0);
    Tlf35585_WriteReg(TLF_W_PROTCFG, TLF_LOCK_KEY1);
    Tlf35585_WriteReg(TLF_W_PROTCFG, TLF_LOCK_KEY2);
    Tlf35585_WriteReg(TLF_W_PROTCFG, TLF_LOCK_KEY3);
    Stm_DelayMs(1u);                         /* >60us before config is active */
    Tlf35585_ReadReg(TLF_R_PROTSTAT, &p);
    Tlf35585_ReadReg(TLF_RW_SPISF,   &spisf);
    Tlf35585_ReadReg(TLF_RW_SYSSF,   &syssf);
    Debug_Printf("[TLF] post-lock PROTSTAT=0x%02X SPISF=0x%02X SYSSF=0x%02X\r\n",
                 (unsigned)p, (unsigned)spisf, (unsigned)syssf);   /* was discarded! */
    if (((p & 0x01u) == 0u) || ((spisf & TLF_SPISF_LOCK) != 0u))
        return TLF_ERR_PROT;
    if ((syssf & TLF_SYSSF_CFGE) != 0u)
        return TLF_ERR_VERIFY;               /* double-bit err: rewrite ALL regs */
    return TLF_OK;
}

static Tlf35585_Status_t prv_ApplyProtectedConfig(void)
{
    Tlf35585_Status_t s;
    uint8 v, i;
    uint8 syspcfg0 = 0u, wdcfg1 = 0u, fwdcfg = 0u;
    uint8 wdcfg0   = TLF_WDCFG0_RUNTIME;
#if (TLF_FWD_ENABLE == 1u)
    wdcfg0 |= TLF_WDCFG0_FWDEN;
#endif

    /* Preserve what we don't manage: re-write the currently ACTIVE values */
    Tlf35585_ReadReg(TLF_R_SYSPCFG0, &syspcfg0);
    Tlf35585_ReadReg(TLF_R_WDCFG1,   &wdcfg1);
    Tlf35585_ReadReg(TLF_R_FWDCFG,   &fwdcfg);

    const TlfProtReg_t regs[7] = {
        { TLF_W_SYSPCFG0, TLF_R_SYSPCFG0, syspcfg0, "SYSPCFG0" },
        { TLF_W_SYSPCFG1, TLF_R_SYSPCFG1, 0x00u,    "SYSPCFG1" }, /* ERREN=0, both targets */
        { TLF_W_WDCFG0,   TLF_R_WDCFG0,   wdcfg0,   "WDCFG0"   },
        { TLF_W_WDCFG1,   TLF_R_WDCFG1,   wdcfg1,   "WDCFG1"   },
        { TLF_W_FWDCFG,   TLF_R_FWDCFG,   fwdcfg,   "FWDCFG"   },
        { TLF_W_WWDCFG0,  TLF_R_WWDCFG0,  0x01u,    "WWDCFG0"  }, /* closed = 1 unit  */
        { TLF_W_WWDCFG1,  TLF_R_WWDCFG1,  0x03u,    "WWDCFG1"  }, /* open   = 3 units */
    };

    s = prv_Unlock();
    if (s != TLF_OK) return s;

    /* Write ALL protected registers in this single session (partial writes
     * across repeated unlock/lock sessions can raise a false SYSSF.CFGE). */
    for (i = 0u; i < 7u; i++)
        Tlf35585_WriteReg(regs[i].reqAddr, regs[i].value);

    /* --- ECHO CHECK: request regs must read back what we wrote --------- */
    for (i = 0u; i < 7u; i++)
    {
        Tlf35585_ReadReg(regs[i].reqAddr, &v);
        if ((uint8)(v ^ regs[i].value) != 0xFFu)
        {
            Debug_Printf("[TLF] WARN: %s echo (req 0x%02X wrote 0x%02X "
                         "read 0x%02X, expect ~0x%02X)\r\n", regs[i].name,
                         (unsigned)regs[i].reqAddr, (unsigned)regs[i].value,
                         (unsigned)v, (unsigned)(0xFFu ^ regs[i].value));
            /* warning only — activation check below is the gate */
        }
    }

    s = prv_Lock();                  /* verifies PROTSTAT + SPISF.LOCK, waits 60us+ */
    if (s != TLF_OK) return s;

    /* --- ACTIVATION CHECK: active copies must now match ---------------- */
    for (i = 0u; i < 7u; i++)
    {
        Tlf35585_ReadReg(regs[i].actAddr, &v);
        if (v != regs[i].value)
        {
            uint8 syssf = 0u;
            Tlf35585_ReadReg(TLF_RW_SYSSF, &syssf);
            Debug_Printf("[TLF] ERROR: %s not ACTIVATED (act 0x%02X=0x%02X, "
                         "want 0x%02X, SYSSF=0x%02X)\r\n", regs[i].name,
                         (unsigned)regs[i].actAddr, (unsigned)v,
                         (unsigned)regs[i].value, (unsigned)syssf);
            return TLF_ERR_VERIFY;
        }
    }

    /* --- Derive WWD service timing from ACTIVATED values, never from
     *     intent.  Window = CW/OW * 50 watchdog ticks; tick = WDCYC
     *     (bit0: 0 = 0.1 ms, 1 = 1 ms).                                  */
    {
        uint8  ww0 = 0u, ww1 = 0u;
        uint32 unitMs = ((wdcfg0 & TLF_WDCFG0_WDCYC) != 0u) ? 50u : 5u;
        Tlf35585_ReadReg(TLF_R_WWDCFG0, &ww0);
        Tlf35585_ReadReg(TLF_R_WWDCFG1, &ww1);
        s_wdtClosedWindowMs  = (uint32)ww0 * unitMs;
        s_wdtOpenWindowMs    = (uint32)ww1 * unitMs;
        s_wdtServiceTargetMs = s_wdtClosedWindowMs + (s_wdtOpenWindowMs / 2u);
        Debug_Printf("[TLF] WWD active: closed=%ums open=%ums target=%ums\r\n",
                     (unsigned)s_wdtClosedWindowMs,
                     (unsigned)s_wdtOpenWindowMs,
                     (unsigned)s_wdtServiceTargetMs);
    }
    return TLF_OK;
}


/* ================================================================== */
/*  State transition helper                                           */
/* ================================================================== */
static Tlf35585_Status_t prv_GoToState(uint8 stateReq, const char *name)
{
    /* Read current DEVCTRL (same address for read and write) */
    s_devctrlShadow = (s_devctrlShadow & 0xF8u) | (stateReq & 0x07u);   /* ADDED */
    Tlf35585_WriteReg(TLF_W_DEVCTRL,  s_devctrlShadow);
    Tlf35585_WriteReg(TLF_W_DEVCTRLN, (uint8)(~s_devctrlShadow));
    if ((stateReq == TLF_STATE_SLEEP) || (stateReq == TLF_STATE_STANDBY))
    {   Debug_Printf("[TLF] -> %s (requested)\r\n", name);  return TLF_OK; }

    uint32 startMs = Stm_GetTimeMs();
    while (Tlf35585_GetDevState() != stateReq)
    {
        if ((Stm_GetTimeMs() - startMs) > 10u)
        {
            uint8 syssf = 0u;
            Tlf35585_ReadReg(TLF_RW_SYSSF, &syssf);
            if ((syssf & TLF_SYSSF_NOOP) != 0u)
                Debug_Print("[TLF] STATEREQ rejected (NO_OP: DEVCTRL/N mismatch)\r\n");
            Tlf35585_LogEvent(TLF_EVT_STATE_CHANGE);
            return TLF_ERR_STATE;
        }
    }
    return TLF_OK;
}

/* ================================================================== */
/*  WDI pin toggle                                                    */
/* ================================================================== */
static void prv_ToggleWdi(void)
{
    s_wdiPinState = !s_wdiPinState;
    if (s_wdiPinState)
        IfxPort_setPinHigh(AppPin_GetPort(PIN_TLF_WDI.portIdx), PIN_TLF_WDI.pinIdx);
    else
        IfxPort_setPinLow(AppPin_GetPort(PIN_TLF_WDI.portIdx), PIN_TLF_WDI.pinIdx);
}

/* ================================================================== */
/*  Public API — Register access                                      */
/* ================================================================== */

Tlf35585_Status_t Tlf35585_ReadReg(Tlf_RegAddr_t addr, uint8 *data)
{
    uint16 rxFrame = 0u;
    uint16 txFrame = prv_BuildFrame(FALSE, (uint8)addr, 0x00u);

    Tlf35585_Status_t s = prv_SpiTransfer(txFrame, &rxFrame);
    if (s != TLF_OK) return s;

    if (data != NULL_PTR)
        *data = (uint8)(rxFrame & 0xFFu);

    return TLF_OK;
}

Tlf35585_Status_t Tlf35585_WriteReg(Tlf_RegAddr_t addr, uint8 data)
{
    uint16 txFrame = prv_BuildFrame(TRUE, (uint8)addr, data);
    return prv_SpiTransfer(txFrame, NULL_PTR);
}

uint8 Tlf35585_GetDevState(void)
{
    uint8 val = 0u;
    Tlf35585_ReadReg(TLF_R_DEVSTAT, &val);
    return (val & TLF_DEVSTAT_STATE_MASK);
}

/* ================================================================== */
/*  Public API — Init (PMC-PMIC-001)                                  */
/* ================================================================== */

void Tlf35585_EarlyInit(void)
{
    uint8 val;

    prv_SpiInit();

    /* Enable voltage supply rails */
    s_devctrlShadow = TLF_DEVCTRL_VREFEN | TLF_DEVCTRL_COMEN
                | TLF_DEVCTRL_TRK1EN | TLF_DEVCTRL_TRK2EN;    /* known state from scratch */
    Tlf35585_WriteReg(TLF_W_DEVCTRL,  s_devctrlShadow);
    Tlf35585_WriteReg(TLF_W_DEVCTRLN, (uint8)(~s_devctrlShadow));

    /* Clear status flags */
    Tlf35585_WriteReg(TLF_RW_SYSSF, TLF_CLEAR_STATUS);
    Tlf35585_WriteReg(TLF_RW_SPISF, TLF_CLEAR_STATUS);

    /* 60us delay per datasheet then NORMAL */
    //Stm_DelayMs(1u);

    //s_devctrlShadow = (s_devctrlShadow & 0xF8u) | TLF_STATE_NORMAL;
    //Tlf35585_WriteReg(TLF_W_DEVCTRL,  s_devctrlShadow);
    //Tlf35585_WriteReg(TLF_W_DEVCTRLN, (uint8)(~s_devctrlShadow));
}

Tlf35585_Status_t Tlf35585_Init(void)
{
    Tlf35585_Status_t s;

    Debug_Print("[TLF] Init: QSPI2...\r\n");
    s = prv_SpiInit();
    if (s != TLF_OK)
    {
        Debug_Print("[TLF] ERROR: QSPI2 init failed\r\n");
        return TLF_ERR_SPI;
    }
    Debug_Print("[TLF] SPI OK\r\n");
    Stm_DelayMs(2u);

    /* ---- Restart cause: log it, then retire it (W1C) -------------- */
    {
        uint8 initerr = 0u, sysfail = 0u;
        Tlf35585_ReadReg(TLF_RW_INITERR, &initerr);
        Tlf35585_ReadReg(TLF_RW_SYSFAIL, &sysfail);
        Debug_Printf("[TLF] Restart cause: INITERR=0x%02X SYSFAIL=0x%02X\r\n",
                     (unsigned)initerr, (unsigned)sysfail);
        if (initerr != 0u) Tlf35585_WriteReg(TLF_RW_INITERR, TLF_CLEAR_STATUS);
        if (sysfail != 0u) Tlf35585_WriteReg(TLF_RW_SYSFAIL, TLF_CLEAR_STATUS);
    }

    /* ---- Clear stale flags so post-lock checks see fresh state ---- */
    Tlf35585_WriteReg(TLF_RW_SPISF, TLF_CLEAR_STATUS);
    Tlf35585_WriteReg(TLF_RW_SYSSF, TLF_CLEAR_STATUS);

    /* ---- Protected config: THE one and only session ---------------
     * unlock (verified) -> write all 7 -> echo check -> lock
     * (verified, prints post-lock PROTSTAT/SPISF/SYSSF) -> activation
     * check on 0x0B..0x11 -> derive WWD timing from ACTIVE values.   */
    s = prv_ApplyProtectedConfig();
    if (s != TLF_OK)
    {
        Tlf35585_DumpStatus("prot-config-failed");
        return s;
    }

    /* ---- Enable rails, settle, retire power-up voltage latches ---- */
    s_devctrlShadow |= TLF_DEVCTRL_VREFEN | TLF_DEVCTRL_COMEN;
    Tlf35585_WriteReg(TLF_W_DEVCTRL,  s_devctrlShadow);
    Tlf35585_WriteReg(TLF_W_DEVCTRLN, (uint8)(~s_devctrlShadow));
    Stm_DelayMs(8u);

    Tlf35585_WriteReg(TLF_R_MONSF0, TLF_CLEAR_STATUS);  /* 0x20..0x23, W1C */
    Tlf35585_WriteReg(TLF_R_MONSF1, TLF_CLEAR_STATUS);
    Tlf35585_WriteReg(TLF_R_MONSF2, TLF_CLEAR_STATUS);
    Tlf35585_WriteReg(TLF_R_MONSF3, TLF_CLEAR_STATUS);
    Tlf35585_WriteReg(TLF_RW_SYSSF, TLF_CLEAR_STATUS);

    /* ---- First WWD service — inside INIT's long open window ------- */
    {
        uint8 wwdCmd = 0u;
        Tlf35585_ReadReg(TLF_RW_WWDSCMD, &wwdCmd);
        Tlf35585_WriteReg(TLF_RW_WWDSCMD,
            ((wwdCmd & TLF_WWDSCMD_TRIG_STATUS) != 0u) ? 0x00u : TLF_WWDSCMD_TRIG);
    }
    Stm_DelayMs(1u);                  /* >60 us after last service      */

    /* ---- NORMAL — last step --------------------------------------- */
    s = prv_GoToState(TLF_STATE_NORMAL, "NORMAL");   /* polls DEVSTAT   */
    if (s != TLF_OK)
    {
        uint8 syssf = 0u, wwdstat = 0u, ie = 0u, spisf = 0u;
        Tlf35585_ReadReg(TLF_RW_SYSSF,   &syssf);
        Tlf35585_ReadReg(TLF_R_WWDSTAT,  &wwdstat);
        Tlf35585_ReadReg(TLF_RW_INITERR, &ie);
        Tlf35585_ReadReg(TLF_RW_SPISF,   &spisf);
        Debug_Printf("[TLF] ERROR: NORMAL refused (SYSSF=0x%02X WWDSTAT=0x%02X "
                     "IE=0x%02X SPISF=0x%02X ERRpin=%u SS=%u)\r\n",
                     (unsigned)syssf, (unsigned)wwdstat, (unsigned)ie,
                     (unsigned)spisf,
                     (unsigned)Tlf35585_IsErrActive(),
                     (unsigned)Tlf35585_IsSafeStateActive());
        return TLF_ERR_STATE;
    }

    /* ---- Runtime bookkeeping -------------------------------------- */
    s_wdtLastServiceMs = Stm_GetTimeMs();
    s_lastFaultPollMs  = Stm_GetTimeMs();
    s_initialised      = TRUE;
    Tlf35585_LogEvent(TLF_EVT_INIT);
    Debug_Print("[TLF] Init complete\r\n");
    return TLF_OK;
}

/* ================================================================== */
/*  Window Watchdog Service (PMC-PMIC-004, 006)                       */
/* ================================================================== */

void Tlf35585_ServiceWdt(void)
{
    uint32 nowMs, elapsedMs;
    uint8  wwdCmd;

    if (!s_initialised) return;

    nowMs     = Stm_GetTimeMs();
    elapsedMs = nowMs - s_wdtLastServiceMs;

    /* ---- Trigger when we're past the closed window ---------------- */
    if (elapsedMs >= s_wdtServiceTargetMs)
    {
        boolean isMiss = (elapsedMs >= (s_wdtClosedWindowMs + s_wdtOpenWindowMs));

        s_wdtLastServiceMs = nowMs;
        if (isMiss)
        {
            s_wwdMissCount++;
            Debug_Printf("[TLF] WDT MISS #%u (elapsed=%ums)\r\n",
                         (unsigned)s_wwdMissCount, (unsigned)elapsedMs);
        }
        else
        {
            s_wwdServiceCount++;
        }

        Tlf35585_ReadReg(TLF_RW_WWDSCMD, &wwdCmd);
        Tlf35585_WriteReg(TLF_RW_WWDSCMD,
            ((wwdCmd & TLF_WWDSCMD_TRIG_STATUS) != 0u) ? 0x00u : TLF_WWDSCMD_TRIG);
        prv_ToggleWdi();
    }

    /* ---- Heartbeat: INDEPENDENT of service outcome ---------------- */
    {
        static uint32 s_lastStatMs = 0u;
        if ((nowMs - s_lastStatMs) >= 5000u)
        {
            uint8 devstat = 0u, syssf = 0u, wwdstat = 0u;
            s_lastStatMs = nowMs;
            Tlf35585_ReadReg(TLF_R_DEVSTAT, &devstat);
            Tlf35585_ReadReg(TLF_RW_SYSSF,  &syssf);
            Tlf35585_ReadReg(TLF_R_WWDSTAT, &wwdstat);   /* TLF's OWN error counter */
            //Debug_Printf("[TLF] WDT: svc=%u miss=%u WWDSTAT=0x%02X DEV=0x%02X "
                        // "SYSSF=0x%02X ERR=%u SS=%u\r\n",
                         //(unsigned)s_wwdServiceCount, (unsigned)s_wwdMissCount,
                        // (unsigned)wwdstat, (unsigned)devstat, (unsigned)syssf,
                        // (unsigned)Tlf35585_IsErrActive(),
                        // (unsigned)Tlf35585_IsSafeStateActive());
   
        }
    }
}
/* ================================================================== */
/*  Functional Watchdog Q&A (PMC-PMIC-005)                            */
/* ================================================================== */

Tlf35585_Status_t Tlf35585_ServiceFwd(void)
{
#if (TLF_FWD_ENABLE == 1u)
    uint8 question = 0u;
    uint8 answer;

    if (!s_initialised) return TLF_OK;

    Tlf35585_ReadReg(TLF_R_FWDSTAT0, &question);
    answer = (~question) & 0x0Fu;
    Tlf35585_WriteReg(TLF_W_FWDRSP, answer);

    return TLF_OK;
#else
    return TLF_OK;
#endif
}

/* ================================================================== */
/*  Fault Monitoring (PMC-PMIC-003, 008)                              */
/* ================================================================== */

void Tlf35585_RegisterFaultCb(Tlf35585_FaultCb_t cb)
{
    s_faultCb = cb;
}

void Tlf35585_CheckFaults(void)
{
    uint8  syssf = 0u, monsf0 = 0u, monsf1 = 0u, monsf2 = 0u, wwdstat = 0u;
    uint32 nowMs;
    uint32 sig;
    boolean safeState;

    static uint32 s_lastFaultSig = 0u;     /* 0 = no fault condition */

    if (!s_initialised) return;
    nowMs = Stm_GetTimeMs();
    if ((nowMs - s_lastFaultPollMs) < TLF_FAULT_POLL_INTERVAL_MS) return;
    s_lastFaultPollMs = nowMs;

    /* Capture BEFORE any recovery/clear runs this pass — the log must
     * record the fault as found, not as left */
    Tlf35585_ReadReg(TLF_RW_SYSSF,   &syssf);
    Tlf35585_ReadReg(TLF_R_MONSF0,   &monsf0);
    Tlf35585_ReadReg(TLF_R_MONSF1,   &monsf1);
    Tlf35585_ReadReg(TLF_R_MONSF2,   &monsf2);
    Tlf35585_ReadReg(TLF_R_WWDSTAT,  &wwdstat);
    safeState = Tlf35585_IsSafeStateActive();

    /* Fault signature: one word describing the current condition */
    sig = ((uint32)(syssf & (TLF_SYSSF_CFGE | TLF_SYSSF_WWDE | TLF_SYSSF_FWDE)) << 24)
        | ((uint32)monsf1 << 16)
        | ((uint32)monsf2 <<  8)
        | (safeState ? 1u : 0u);

    if (sig != 0u)
    {
        if (sig != s_lastFaultSig)
        {
            /* ---- EDGE: new or changed fault — report once ---- */
            if ((monsf1 != 0u) || (monsf2 != 0u))
                Debug_Printf("[TLF] VOLTAGE: MONSF1=0x%02X MONSF2=0x%02X\r\n",
                             (unsigned)monsf1, (unsigned)monsf2);
            if (safeState)
                Debug_Print("[TLF] SAFE STATE active\r\n");

            Tlf35585_LogEvent(TLF_EVT_FAULT);
        }
        /* level unchanged: fault persists, already reported — silent */
    }
    s_lastFaultSig = sig;                  /* clears the latch when sig==0,
                                            * re-arming edge detection    */
}

boolean Tlf35585_CheckVoltageStatus(uint8 *pMonsf1, uint8 *pMonsf2)
{
    uint8 m1 = 0u, m2 = 0u;
    Tlf35585_ReadReg(TLF_R_MONSF1, &m1);
    Tlf35585_ReadReg(TLF_R_MONSF2, &m2);
    if (pMonsf1 != NULL_PTR) *pMonsf1 = m1;
    if (pMonsf2 != NULL_PTR) *pMonsf2 = m2;
    return ((m1 != 0u) || (m2 != 0u));
}

/* ================================================================== */
/*  State Transitions (PMC-PMIC-009, 010)                             */
/* ================================================================== */

Tlf35585_Status_t Tlf35585_GoToNormal(void)  { return prv_GoToState(TLF_STATE_NORMAL,  "NORMAL"); }
Tlf35585_Status_t Tlf35585_GoToStandby(void) { return prv_GoToState(TLF_STATE_STANDBY, "STANDBY"); }
Tlf35585_Status_t Tlf35585_GoToSleep(void)   { return prv_GoToState(TLF_STATE_SLEEP,   "SLEEP"); }

/* ================================================================== */
/*  Pin Status                                                        */
/* ================================================================== */

boolean Tlf35585_IsErrActive(void)
{
    return (boolean)IfxPort_getPinState(
        AppPin_GetPort(PIN_TLF_ERR.portIdx), PIN_TLF_ERR.pinIdx);
}

boolean Tlf35585_IsSafeStateActive(void)
{
    return (IfxPort_getPinState(AppPin_GetPort(PIN_TLF_SS.portIdx),
                                PIN_TLF_SS.pinIdx) == FALSE);
}

/* ================================================================== */
/*  Event Logging (PMC-PMIC-011)                                      */
/* ================================================================== */

void Tlf35585_LogEvent(uint8 eventType)
{
    Tlf35585_Event_t evt;
    evt.timestampMs = Stm_GetTimeMs();
    evt.eventType   = eventType;
    evt.errPin      = (uint8)Tlf35585_IsErrActive();
    evt.ssPin       = (uint8)Tlf35585_IsSafeStateActive();

    Tlf35585_ReadReg(TLF_R_DEVSTAT, &evt.devstat);
    Tlf35585_ReadReg(TLF_RW_SYSSF,  &evt.syssf);
    Tlf35585_ReadReg(TLF_R_MONSF1,  &evt.monsf1);
    Tlf35585_ReadReg(TLF_R_MONSF2,  &evt.monsf2);
    Tlf35585_ReadReg(TLF_RW_INITERR, &evt.initerr);
    Tlf35585_ReadReg(TLF_R_WWDSTAT, &evt.wwdstat);
    Tlf35585_ReadReg(TLF_RW_SPISF,  &evt.spisf);
    

    if (IfxCpu_getCoreIndex() == 0u)
    {
        Debug_Printf("[TLF] EVT[%u] t=%ums DEV=0x%02X SYSSF=0x%02X "
                 "M1=0x%02X WDD=0x%02X IE=0x%02X WD=0x%02X SPI=0x%02X "
                 "ERR=%u SS=%u\r\n",
                 (unsigned)eventType, (unsigned)evt.timestampMs,
                 (unsigned)evt.devstat, (unsigned)evt.syssf,
                 (unsigned)evt.monsf1, (unsigned)evt.wwdstat,
                 (unsigned)evt.initerr, (unsigned)evt.wwdstat,
                 (unsigned)evt.spisf,
                 (unsigned)evt.errPin, (unsigned)evt.ssPin);
        uint32 pmicData[4] = { (uint32)evt.syssf, (uint32)evt.monsf1,
                            (uint32)evt.wwdstat, (uint32)evt.devstat };
        //NvLog_Write(NVLOG_EVT_PMIC_FAULT, NVLOG_SRC_PMIC, NVLOG_SEV_ERROR, pmicData);
    }
    else
    {
        //g_ipcShared.fusa.tlfEvtData[0] = (uint32)evt.syssf;
        //g_ipcShared.fusa.tlfEvtData[1] = (uint32)evt.monsf1;
        //g_ipcShared.fusa.tlfEvtData[2] = (uint32)evt.wwdstat;
        //g_ipcShared.fusa.tlfEvtData[3] = (uint32)evt.devstat;
        //__dsync();
        //g_ipcShared.fusa.tlfEvtSeq++;
        //__dsync();
    }
}

void Tlf35585_DumpStatus(const char *context)
{
    uint8 v = 0u;
    Debug_Printf("[TLF] === %s ===\r\n", context);

    Tlf35585_ReadReg(TLF_R_DEVSTAT, &v);  Debug_Printf("[TLF]   DEVSTAT =0x%02X\r\n", (unsigned)v);
    Tlf35585_ReadReg(TLF_RW_SYSSF, &v);   Debug_Printf("[TLF]   SYSSF  =0x%02X\r\n", (unsigned)v);
    Tlf35585_ReadReg(TLF_RW_SPISF, &v);   Debug_Printf("[TLF]   SPISF  =0x%02X\r\n", (unsigned)v);
    Tlf35585_ReadReg(TLF_R_MONSF0, &v);   Debug_Printf("[TLF]   MONSF0 =0x%02X\r\n", (unsigned)v);
    Tlf35585_ReadReg(TLF_R_MONSF1, &v);   Debug_Printf("[TLF]   MONSF1 =0x%02X\r\n", (unsigned)v);
    Tlf35585_ReadReg(TLF_R_MONSF2, &v);   Debug_Printf("[TLF]   MONSF2 =0x%02X\r\n", (unsigned)v);
    Tlf35585_ReadReg(TLF_RW_INITERR, &v);  Debug_Printf("[TLF]   INITERR=0x%02X\r\n", (unsigned)v);
    Tlf35585_ReadReg(TLF_R_WDCFG0, &v);   Debug_Printf("[TLF]   WDCFG0 =0x%02X\r\n", (unsigned)v);
    Tlf35585_ReadReg(TLF_R_WWDCFG0, &v);  Debug_Printf("[TLF]   WWDCFG0=0x%02X\r\n", (unsigned)v);
    Tlf35585_ReadReg(TLF_R_WWDCFG1, &v);  Debug_Printf("[TLF]   WWDCFG1=0x%02X\r\n", (unsigned)v);
    Tlf35585_ReadReg(TLF_R_WWDSTAT, &v);   Debug_Printf("[TLF]   WWDSTAT=0x%02X\r\n", (unsigned)v);

    Debug_Printf("[TLF] === end %s ===\r\n", context);
}

void Tlf35585_EnableIsrMode(void)
{
    /* Clear any SRR left over from CPU0's last polled transfer, so
     * CPU2 doesn't take a spurious interrupt the moment SRE goes on */
    IfxSrc_clearRequest(&SRC_QSPI_QSPI2_TX);
    IfxSrc_clearRequest(&SRC_QSPI_QSPI2_RX);
    IfxSrc_clearRequest(&SRC_QSPI_QSPI2_ERR);
    __dsync();
    IfxSrc_enable(&SRC_QSPI_QSPI2_TX);
    IfxSrc_enable(&SRC_QSPI_QSPI2_RX);
    IfxSrc_enable(&SRC_QSPI_QSPI2_ERR);
}

void Tlf35585_PublishRegSnapshot(void)          /* CPU2, ~1 Hz */
{
    uint8 v;
    Tlf35585_ReadReg(TLF_R_DEVSTAT,  &v); //g_ipcShared.fusa.tlfRegs[0]  = v;
    Tlf35585_ReadReg(TLF_RW_SYSSF,   &v); //g_ipcShared.fusa.tlfRegs[1]  = v;
    Tlf35585_ReadReg(TLF_RW_SPISF,   &v); //g_ipcShared.fusa.tlfRegs[2]  = v;
    Tlf35585_ReadReg(TLF_R_MONSF0,   &v); //g_ipcShared.fusa.tlfRegs[3]  = v;
    Tlf35585_ReadReg(TLF_R_MONSF1,   &v); //g_ipcShared.fusa.tlfRegs[4]  = v;
    Tlf35585_ReadReg(TLF_R_MONSF2,   &v); //g_ipcShared.fusa.tlfRegs[5]  = v;
    Tlf35585_ReadReg(TLF_RW_INITERR, &v); //g_ipcShared.fusa.tlfRegs[6]  = v;
    Tlf35585_ReadReg(TLF_R_WDCFG0,  &v); //g_ipcShared.fusa.tlfRegs[7]  = v;
    Tlf35585_ReadReg(TLF_R_WWDCFG0, &v); //g_ipcShared.fusa.tlfRegs[8]  = v;
    Tlf35585_ReadReg(TLF_R_WWDCFG1, &v); //g_ipcShared.fusa.tlfRegs[9]  = v;
    Tlf35585_ReadReg(TLF_R_WWDSTAT,  &v); // g_ipcShared.fusa.tlfRegs[10] = v;
    __dsync();
    //g_ipcShared.fusa.tlfRegsSeq++;
}