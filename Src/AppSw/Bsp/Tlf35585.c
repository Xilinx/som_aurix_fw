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
 * @brief   Infineon TLF35585 Safety PMIC driver — iLLD QSPI2 SpiMaster.
 *
 * Hardware connections (GP_AURIX_Subsystem_PinDefn.xlsx):
 *   QSPI2  P15.2  SLSO0 (CS)    P15.3  SCLK
 *          P15.6  MTSR  (MOSI)   P15.7  MRST (MISO)
 *   GPIO   P33.8  ERR   P33.9  SS   P33.10 WAKE   P33.11 WDI
 *
 * SPI Mode 1: CPOL=0 (idle low), CPHA=1 (shift TX on trailing edge).
 * 32-bit frame, 1 MHz, MSB first.
 *
 * iLLD version: 1.20.0  (TC3xx)
 */

#include "Tlf35585.h"
#include "Platform_PinCfg.h"
#include "AppPin.h"
#include "Stm_Timer.h"
#include "Uart_Debug.h"
#include "IfxPort.h"
#include "IfxQspi_SpiMaster.h"

/* ---- QSPI2 Driver State ------------------------------------------------ */

static IfxQspi_SpiMaster         s_spiMaster;
static IfxQspi_SpiMaster_Channel s_spiChannel;
static boolean                   s_initialised      = FALSE;
static uint32                    s_wdtLastServiceMs  = 0u;
static uint32                    s_wdtOpenWindowMs   = 200u;
static uint32                    s_wdtClosedWindowMs = 10u;
static boolean                   s_wdiPinState       = FALSE;

/* ---- SPI Frame Construction --------------------------------------------- */

static uint32 prv_BuildFrame(boolean write, uint8 addr, uint16 data)
{
    uint32 frame;
    uint32 parityBits;
    uint8  parity;
    uint8  i;

    frame  = 0u;
    frame |= (write ? (1u << 31u) : 0u);
    frame |= ((uint32)(addr & 0x3Fu) << 25u);
    frame |= ((uint32)data << 8u);

    parityBits = (frame >> 8u);
    parity = 0u;
    for (i = 0u; i < 24u; i++)
    {
        if ((parityBits & (1u << i)) != 0u)
        {
            parity ^= 1u;
        }
    }
    frame |= ((uint32)parity << 24u);

    return frame;
}

/* ---- Low-Level SPI Transfer --------------------------------------------- */

static Tlf35585_Status_t prv_SpiTransfer(uint32 txData, uint32 *rxData)
{
    uint32 tx;
    uint32 rx;

    tx = txData;
    rx = 0u;

    while (IfxQspi_SpiMaster_getStatus(&s_spiChannel) == IfxQspi_Status_busy)
    {
        /* Wait for any prior transfer to complete */
    }

    IfxQspi_SpiMaster_exchange(&s_spiChannel, &tx, &rx, 1u);

    while (IfxQspi_SpiMaster_getStatus(&s_spiChannel) == IfxQspi_Status_busy)
    {
        /* Spin — acceptable for single-frame PMIC transactions */
    }

    if (rxData != NULL_PTR)
    {
        *rxData = rx;
    }

    return TLF_OK;
}

/* ---- QSPI2 Hardware Init ------------------------------------------------ */

static Tlf35585_Status_t prv_SpiInit(void)
{
    /* ---- Module config -------------------------------------------------- */
    IfxQspi_SpiMaster_Config masterCfg;
    IfxQspi_SpiMaster_initModuleConfig(&masterCfg, &MODULE_QSPI2);

    masterCfg.mode = IfxQspi_Mode_master;

    /* Pin struct: {SCLK, sclkMode, MTSR, mtsrMode, MRST, mrstMode, driver}
     * Pin object names from iLLD _PinMap/IfxQspi_PinMap.h.
     * If MRSTA doesn't exist for your iLLD build, check for MRSTB or MRST. */
    {
        const IfxQspi_SpiMaster_Pins qspi2Pins = {
            &IfxQspi2_SCLK_P15_3_OUT,  IfxPort_OutputMode_pushPull,
            &IfxQspi2_MTSR_P15_6_OUT,  IfxPort_OutputMode_pushPull,
            &IfxQspi2_MRSTB_P15_7_IN,  IfxPort_InputMode_pullDown,
            IfxPort_PadDriver_cmosAutomotiveSpeed1
        };
        masterCfg.pins = &qspi2Pins;

        IfxQspi_SpiMaster_initModule(&s_spiMaster, &masterCfg);
    }

    /* ---- Channel config ------------------------------------------------- */
    {
        IfxQspi_SpiMaster_ChannelConfig chCfg;
        IfxQspi_SpiMaster_initChannelConfig(&chCfg, &s_spiMaster);

        chCfg.ch.baudrate            = 1000000.0f;   /* 1 MHz */
        chCfg.ch.mode.dataWidth      = 32u;          /* 32-bit frame */
        chCfg.ch.mode.clockPolarity  = IfxQspi_ClockPolarity_idleLow;               /* CPOL=0 */
        chCfg.ch.mode.shiftClock     = IfxQspi_ShiftClock_shiftTransmitDataOnTrailingEdge; /* CPHA=1 */
        chCfg.ch.mode.dataHeading    = IfxQspi_DataHeading_msbFirst;

        /* Slave select: SLSO0 on P15.2 */
        {
            const IfxQspi_SpiMaster_Output slaveSelect = {
                &IfxQspi2_SLSO0_P15_2_OUT, IfxPort_OutputMode_pushPull,
                IfxPort_PadDriver_cmosAutomotiveSpeed1
            };
            chCfg.sls.output = slaveSelect;
        }

        IfxQspi_SpiMaster_initChannel(&s_spiChannel, &chCfg);
    }

    return TLF_OK;
}

/* ---- Public API --------------------------------------------------------- */

Tlf35585_Status_t Tlf35585_ReadReg(uint8 addr, uint16 *data)
{
    uint32 txFrame;
    uint32 rxFrame;
    Tlf35585_Status_t status;

    txFrame = prv_BuildFrame(FALSE, addr, 0x0000u);
    rxFrame = 0u;

    status = prv_SpiTransfer(txFrame, &rxFrame);
    if (status != TLF_OK)
    {
        return status;
    }

    if (data != NULL_PTR)
    {
        *data = (uint16)((rxFrame >> 8u) & 0xFFFFu);
    }

    return TLF_OK;
}

Tlf35585_Status_t Tlf35585_WriteReg(uint8 addr, uint16 data)
{
    uint32 txFrame;

    txFrame = prv_BuildFrame(TRUE, addr, data);

    return prv_SpiTransfer(txFrame, NULL_PTR);
}

static Tlf35585_Status_t prv_Unlock(void)
{
    Tlf35585_Status_t s;

    Debug_Print("[TLF] Unlock: sending 4-key sequence...\r\n");

    s = Tlf35585_WriteReg(TLF_REG_PROTREG, TLF_UNLOCK_KEY0);
    if (s != TLF_OK)
    {
        Debug_Print("[TLF] Unlock: KEY0 failed\r\n");
        return s;
    }

    s = Tlf35585_WriteReg(TLF_REG_PROTREG, TLF_UNLOCK_KEY1);
    if (s != TLF_OK)
    {
        Debug_Print("[TLF] Unlock: KEY1 failed\r\n");
        return s;
    }

    s = Tlf35585_WriteReg(TLF_REG_PROTREG, TLF_UNLOCK_KEY2);
    if (s != TLF_OK)
    {
        Debug_Print("[TLF] Unlock: KEY2 failed\r\n");
        return s;
    }

    s = Tlf35585_WriteReg(TLF_REG_PROTREG, TLF_UNLOCK_KEY3);
    if (s != TLF_OK)
    {
        Debug_Print("[TLF] Unlock: KEY3 failed\r\n");
        return s;
    }

    Debug_Print("[TLF] Unlock: sequence complete\r\n");
    return TLF_OK;
}


/* ---- Debug: dump all relevant status registers -------------------------- */
static void prv_DumpStatus(const char *context)
{
    uint16 val = 0u;

    Debug_Printf("[TLF] === Status Dump: %s ===\r\n", context);

    if (Tlf35585_ReadReg(TLF_REG_DEVSTAT, &val) == TLF_OK)
        Debug_Printf("[TLF]   DEVSTAT   = 0x%04X\r\n", (unsigned)val);
    else
        Debug_Print("[TLF]   DEVSTAT   = READ FAILED\r\n");

    if (Tlf35585_ReadReg(TLF_REG_IF, &val) == TLF_OK)
        Debug_Printf("[TLF]   IF        = 0x%04X\r\n", (unsigned)val);
    else
        Debug_Print("[TLF]   IF        = READ FAILED\r\n");

    if (Tlf35585_ReadReg(TLF_REG_SYSSF, &val) == TLF_OK)
        Debug_Printf("[TLF]   SYSSF     = 0x%04X\r\n", (unsigned)val);
    else
        Debug_Print("[TLF]   SYSSF     = READ FAILED\r\n");

    if (Tlf35585_ReadReg(TLF_REG_MONSF1, &val) == TLF_OK)
        Debug_Printf("[TLF]   MONSF1    = 0x%04X\r\n", (unsigned)val);
    else
        Debug_Print("[TLF]   MONSF1    = READ FAILED\r\n");

    if (Tlf35585_ReadReg(TLF_REG_MONSF2, &val) == TLF_OK)
        Debug_Printf("[TLF]   MONSF2    = 0x%04X\r\n", (unsigned)val);
    else
        Debug_Print("[TLF]   MONSF2    = READ FAILED\r\n");

    if (Tlf35585_ReadReg(TLF_REG_INITERR, &val) == TLF_OK)
        Debug_Printf("[TLF]   INITERR   = 0x%04X\r\n", (unsigned)val);
    else
        Debug_Print("[TLF]   INITERR   = READ FAILED\r\n");

    if (Tlf35585_ReadReg(TLF_REG_WDCFG0, &val) == TLF_OK)
        Debug_Printf("[TLF]   WDCFG0    = 0x%04X\r\n", (unsigned)val);
    else
        Debug_Print("[TLF]   WDCFG0    = READ FAILED\r\n");

    if (Tlf35585_ReadReg(TLF_REG_WWDCFG0, &val) == TLF_OK)
        Debug_Printf("[TLF]   WWDCFG0   = 0x%04X\r\n", (unsigned)val);
    else
        Debug_Print("[TLF]   WWDCFG0   = READ FAILED\r\n");

    if (Tlf35585_ReadReg(TLF_REG_WWDCFG1, &val) == TLF_OK)
        Debug_Printf("[TLF]   WWDCFG1   = 0x%04X\r\n", (unsigned)val);
    else
        Debug_Print("[TLF]   WWDCFG1   = READ FAILED\r\n");

    if (Tlf35585_ReadReg(TLF_REG_WWDSCMD, &val) == TLF_OK)
        Debug_Printf("[TLF]   WWDSCMD   = 0x%04X\r\n", (unsigned)val);
    else
        Debug_Print("[TLF]   WWDSCMD   = READ FAILED\r\n");

    Debug_Printf("[TLF] === End Dump: %s ===\r\n", context);
}

Tlf35585_Status_t Tlf35585_Init(void)
{
    Tlf35585_Status_t status;

    Debug_Print("[TLF] Init: configuring QSPI2 SpiMaster...\r\n");

    status = prv_SpiInit();
    if (status != TLF_OK)
    {
        Debug_Print("[TLF] ERROR: QSPI2 init failed\r\n");
        return TLF_ERR_SPI;
    }
    Debug_Print("[TLF] SPI init OK\r\n");

    Stm_DelayMs(2u);

    /* ---- Read initial state --------------------------------------------- */
    prv_DumpStatus("POST-RESET");

    /* ---- Check ERR and SS pins ------------------------------------------ */
    Debug_Printf("[TLF] ERR pin = %u, SS pin = %u\r\n",
                 (unsigned)Tlf35585_IsErrActive(),
                 (unsigned)Tlf35585_IsSafeStateActive());

    /* ---- Unlock and configure watchdog ---------------------------------- */
    status = prv_Unlock();
    if (status != TLF_OK)
    {
        Debug_Print("[TLF] ERROR: protection unlock failed\r\n");
        return TLF_ERR_SPI;
    }

    /* SYSPCFG1: enable ERR pin monitoring */
    status = Tlf35585_WriteReg(TLF_REG_SYSPCFG1, TLF_SYSPCFG1_ERREN);
    Debug_Printf("[TLF] SYSPCFG1 write: 0x%04X -> %s\r\n",
                 (unsigned)TLF_SYSPCFG1_ERREN,
                 (status == TLF_OK) ? "OK" : "FAILED");

    /* WDCFG0: WWDEN=1, FWDEN=0, WWDTSEL=1 (SPI), WDCYC=0 (0.1ms) */
    {
        uint16 wdcfg0_val = TLF_WDCFG0_WWDEN | TLF_WDCFG0_WWDTSEL;
        uint16 readback = 0u;

        status = Tlf35585_WriteReg(TLF_REG_WDCFG0, wdcfg0_val);
        Debug_Printf("[TLF] WDCFG0 write: 0x%04X -> %s\r\n",
                     (unsigned)wdcfg0_val,
                     (status == TLF_OK) ? "OK" : "FAILED");

        (void)Tlf35585_ReadReg(TLF_REG_WDCFG0, &readback);
        Debug_Printf("[TLF] WDCFG0 readback: 0x%04X (expected 0x%04X)%s\r\n",
                     (unsigned)readback,
                     (unsigned)wdcfg0_val,
                     (readback == wdcfg0_val) ? "" : " *** MISMATCH ***");
    }

    /* WWDCFG0: closed window = (CW+1)*50*0.1ms, CW=0 → 5ms */
    {
        uint16 cw_val = 0x0000u;
        uint16 readback = 0u;

        status = Tlf35585_WriteReg(TLF_REG_WWDCFG0, cw_val);
        Debug_Printf("[TLF] WWDCFG0 write: 0x%04X (CW=5ms) -> %s\r\n",
                     (unsigned)cw_val,
                     (status == TLF_OK) ? "OK" : "FAILED");

        (void)Tlf35585_ReadReg(TLF_REG_WWDCFG0, &readback);
        Debug_Printf("[TLF] WWDCFG0 readback: 0x%04X%s\r\n",
                     (unsigned)readback,
                     (readback == cw_val) ? "" : " *** MISMATCH ***");
    }

    /* WWDCFG1: open window = (OW+1)*50*0.1ms, OW=1 → 10ms */
    {
        uint16 ow_val = 0x0001u;
        uint16 readback = 0u;

        status = Tlf35585_WriteReg(TLF_REG_WWDCFG1, ow_val);
        Debug_Printf("[TLF] WWDCFG1 write: 0x%04X (OW=10ms) -> %s\r\n",
                     (unsigned)ow_val,
                     (status == TLF_OK) ? "OK" : "FAILED");

        (void)Tlf35585_ReadReg(TLF_REG_WWDCFG1, &readback);
        Debug_Printf("[TLF] WWDCFG1 readback: 0x%04X%s\r\n",
                     (unsigned)readback,
                     (readback == ow_val) ? "" : " *** MISMATCH ***");
    }

    s_wdtClosedWindowMs = 5u;
    s_wdtOpenWindowMs   = 10u;
    s_wdtLastServiceMs  = Stm_GetTimeMs();

    Debug_Printf("[TLF] WDT window: closed=%ums, open=%ums\r\n",
                 (unsigned)s_wdtClosedWindowMs,
                 (unsigned)s_wdtOpenWindowMs);

    /* ---- First watchdog service ----------------------------------------- */
    {
        uint16 wwdCmd = 0u;
        status = Tlf35585_ReadReg(TLF_REG_WWDSCMD, &wwdCmd);
        Debug_Printf("[TLF] WWDSCMD read: 0x%04X -> %s\r\n",
                     (unsigned)wwdCmd,
                     (status == TLF_OK) ? "OK" : "FAILED");

        if (status == TLF_OK)
        {
            uint16 toggled = wwdCmd ^ 0x0001u;
            status = Tlf35585_WriteReg(TLF_REG_WWDSCMD, toggled);
            Debug_Printf("[TLF] WWDSCMD toggle: 0x%04X -> 0x%04X, %s\r\n",
                         (unsigned)wwdCmd,
                         (unsigned)toggled,
                         (status == TLF_OK) ? "OK" : "FAILED");
        }
        if (status != TLF_OK)
        {
            Debug_Print("[TLF] WARNING: initial WDT service failed\r\n");
        }
    }

    /* Toggle WDI pin */
    s_wdiPinState = !s_wdiPinState;
    if (s_wdiPinState)
        IfxPort_setPinHigh(AppPin_GetPort(PIN_TLF_WDI.portIdx), PIN_TLF_WDI.pinIdx);
    else
        IfxPort_setPinLow(AppPin_GetPort(PIN_TLF_WDI.portIdx), PIN_TLF_WDI.pinIdx);
    Debug_Printf("[TLF] WDI pin toggled -> %u\r\n", (unsigned)s_wdiPinState);

    /* ---- Transition to NORMAL ------------------------------------------- */
    status = prv_Unlock();
    if (status != TLF_OK)
    {
        Debug_Print("[TLF] ERROR: pre-NORMAL unlock failed\r\n");
        return TLF_ERR_SPI;
    }

    status = Tlf35585_WriteReg(TLF_REG_DEVCTRL, TLF_GOTO_NORMAL);
    Debug_Printf("[TLF] DEVCTRL write: 0x%04X -> %s\r\n",
                 (unsigned)TLF_GOTO_NORMAL,
                 (status == TLF_OK) ? "OK" : "FAILED");

    /* DEVCTRLN = inverted confirmation */
    status = Tlf35585_WriteReg(TLF_REG_DEVCTRLN, (uint16)(~TLF_GOTO_NORMAL));
    Debug_Printf("[TLF] DEVCTRLN write: 0x%04X -> %s\r\n",
                 (unsigned)(uint16)(~TLF_GOTO_NORMAL),
                 (status == TLF_OK) ? "OK" : "FAILED");

    Stm_DelayMs(2u);

    /* ---- Verify final state --------------------------------------------- */
    prv_DumpStatus("POST-INIT");

    Debug_Printf("[TLF] ERR pin = %u, SS pin = %u\r\n",
                 (unsigned)Tlf35585_IsErrActive(),
                 (unsigned)Tlf35585_IsSafeStateActive());

    s_initialised = TRUE;
    Debug_Print("[TLF] Init complete.\r\n");
    return TLF_OK;
}

/* ---- Watchdog Service --------------------------------------------------- */
void Tlf35585_ServiceWdt(void)
{
    uint32 elapsedMs;
    uint16 wwdCmd;
    Tlf35585_Status_t status;
    static uint32 s_serviceCount   = 0u;
    static uint32 s_missCount      = 0u;
    static uint32 s_lastStatDumpMs = 0u;

    if (!s_initialised)
    {
        return;
    }

    elapsedMs = Stm_GetTimeMs() - s_wdtLastServiceMs;

    if ((elapsedMs >= s_wdtClosedWindowMs) &&
        (elapsedMs < (s_wdtClosedWindowMs + s_wdtOpenWindowMs)))
    {
        s_wdtLastServiceMs = Stm_GetTimeMs();
        s_serviceCount++;

        /* Read WWDSCMD, toggle bit 0 */
        wwdCmd = 0u;
        status = Tlf35585_ReadReg(TLF_REG_WWDSCMD, &wwdCmd);
        if (status == TLF_OK)
        {
            uint16 toggled = wwdCmd ^ 0x0001u;
            status = Tlf35585_WriteReg(TLF_REG_WWDSCMD, toggled);
        }

        if (status != TLF_OK)
        {
            Debug_Printf("[TLF] ERROR: WDT service #%u SPI failed\r\n",
                         (unsigned)s_serviceCount);
        }

        /* Toggle WDI pin */
        s_wdiPinState = !s_wdiPinState;
        if (s_wdiPinState)
            IfxPort_setPinHigh(AppPin_GetPort(PIN_TLF_WDI.portIdx), PIN_TLF_WDI.pinIdx);
        else
            IfxPort_setPinLow(AppPin_GetPort(PIN_TLF_WDI.portIdx), PIN_TLF_WDI.pinIdx);

        /* Periodic status dump every 5 seconds */
        if ((Stm_GetTimeMs() - s_lastStatDumpMs) >= 5000u)
        {
            s_lastStatDumpMs = Stm_GetTimeMs();

            uint16 wwdstat = 0u;
            uint16 devstat = 0u;
            (void)Tlf35585_ReadReg(TLF_REG_WWDSTAT, &wwdstat);
            (void)Tlf35585_ReadReg(TLF_REG_DEVSTAT, &devstat);
            Debug_Printf("[TLF] WDT periodic: services=%u misses=%u "
                         "WWDSTAT=0x%04X DEVSTAT=0x%04X ERR=%u SS=%u\r\n",
                         (unsigned)s_serviceCount,
                         (unsigned)s_missCount,
                         (unsigned)wwdstat,
                         (unsigned)devstat,
                         (unsigned)Tlf35585_IsErrActive(),
                         (unsigned)Tlf35585_IsSafeStateActive());
        }
    }
    else if (elapsedMs >= (s_wdtClosedWindowMs + s_wdtOpenWindowMs))
    {
        /* Missed window */
        s_wdtLastServiceMs = Stm_GetTimeMs();
        s_missCount++;

        wwdCmd = 0u;
        status = Tlf35585_ReadReg(TLF_REG_WWDSCMD, &wwdCmd);
        if (status == TLF_OK)
        {
            wwdCmd ^= 0x0001u;
            status = Tlf35585_WriteReg(TLF_REG_WWDSCMD, wwdCmd);
        }

        s_wdiPinState = !s_wdiPinState;
        if (s_wdiPinState)
            IfxPort_setPinHigh(AppPin_GetPort(PIN_TLF_WDI.portIdx), PIN_TLF_WDI.pinIdx);
        else
            IfxPort_setPinLow(AppPin_GetPort(PIN_TLF_WDI.portIdx), PIN_TLF_WDI.pinIdx);

        Debug_Printf("[TLF] WARNING: WDT window missed #%u "
                     "(elapsed=%ums, window=%u-%ums)\r\n",
                     (unsigned)s_missCount,
                     (unsigned)elapsedMs,
                     (unsigned)s_wdtClosedWindowMs,
                     (unsigned)(s_wdtClosedWindowMs + s_wdtOpenWindowMs));

        /* Dump status on every miss to catch error counter climbing */
        {
            uint16 wwdstat = 0u;
            (void)Tlf35585_ReadReg(TLF_REG_WWDSTAT, &wwdstat);
            Debug_Printf("[TLF] WWDSTAT after miss: 0x%04X\r\n",
                         (unsigned)wwdstat);
        }
    }
}

boolean Tlf35585_IsErrActive(void)
{
    return (boolean)IfxPort_getPinState(
        AppPin_GetPort(PIN_TLF_ERR.portIdx), PIN_TLF_ERR.pinIdx);
}

boolean Tlf35585_IsSafeStateActive(void)
{
    return (boolean)IfxPort_getPinState(
        AppPin_GetPort(PIN_TLF_SS.portIdx), PIN_TLF_SS.pinIdx);
}