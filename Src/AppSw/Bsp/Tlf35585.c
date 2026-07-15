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

    s = Tlf35585_WriteReg(TLF_REG_PROTREG, TLF_UNLOCK_KEY0);
    if (s != TLF_OK) return s;

    s = Tlf35585_WriteReg(TLF_REG_PROTREG, TLF_UNLOCK_KEY1);
    return s;
}

Tlf35585_Status_t Tlf35585_Init(void)
{
    Tlf35585_Status_t status;
    uint16 devstat;

    Debug_Print("[TLF] Init: configuring QSPI2 SpiMaster...\r\n");

    status = prv_SpiInit();
    if (status != TLF_OK)
    {
        Debug_Print("[TLF] ERROR: QSPI2 init failed\r\n");
        return TLF_ERR_SPI;
    }

    Stm_DelayMs(2u);

    devstat = 0u;
    status = Tlf35585_ReadReg(TLF_REG_DEVSTAT, &devstat);
    if (status != TLF_OK)
    {
        Debug_Print("[TLF] ERROR: DEVSTAT read failed\r\n");
        return TLF_ERR_SPI;
    }
    Debug_Printf("[TLF] DEVSTAT = 0x%04X\r\n", (unsigned)devstat);

    status = prv_Unlock();
    if (status != TLF_OK)
    {
        Debug_Print("[TLF] ERROR: protection unlock failed\r\n");
        return TLF_ERR_SPI;
    }

    status = Tlf35585_WriteReg(TLF_REG_WDCFG0, 0x00FFu);
    if (status != TLF_OK)
    {
        Debug_Print("[TLF] WARNING: WDCFG0 write failed\r\n");
    }

    s_wdtClosedWindowMs = 10u;
    s_wdtOpenWindowMs   = 200u;

    s_wdtLastServiceMs = Stm_GetTimeMs();
    status = Tlf35585_WriteReg(TLF_REG_WWDSCMD, 0x0000u);
    if (status != TLF_OK)
    {
        Debug_Print("[TLF] WARNING: initial WDT service failed\r\n");
    }

    s_wdiPinState = !s_wdiPinState;
    if (s_wdiPinState)
        IfxPort_setPinHigh(AppPin_GetPort(PIN_TLF_WDI.portIdx), PIN_TLF_WDI.pinIdx);
    else
        IfxPort_setPinLow(AppPin_GetPort(PIN_TLF_WDI.portIdx), PIN_TLF_WDI.pinIdx);

    status = prv_Unlock();
    if (status == TLF_OK)
    {
        status = Tlf35585_WriteReg(TLF_REG_DEVCTRL, TLF_GOTO_NORMAL);
    }
    if (status != TLF_OK)
    {
        Debug_Print("[TLF] WARNING: NORMAL state transition may have failed\r\n");
    }

    s_initialised = TRUE;
    Debug_Print("[TLF] Init complete. PMIC watchdog serviced.\r\n");

    return TLF_OK;
}

void Tlf35585_ServiceWdt(void)
{
    uint32 elapsedMs;

    if (!s_initialised)
    {
        return;
    }

    elapsedMs = Stm_GetTimeMs() - s_wdtLastServiceMs;

    if ((elapsedMs >= s_wdtClosedWindowMs) &&
        (elapsedMs < (s_wdtClosedWindowMs + s_wdtOpenWindowMs)))
    {
        s_wdtLastServiceMs = Stm_GetTimeMs();

        (void)Tlf35585_WriteReg(TLF_REG_WWDSCMD, 0x0000u);

        s_wdiPinState = !s_wdiPinState;
        if (s_wdiPinState)
            IfxPort_setPinHigh(AppPin_GetPort(PIN_TLF_WDI.portIdx), PIN_TLF_WDI.pinIdx);
        else
            IfxPort_setPinLow(AppPin_GetPort(PIN_TLF_WDI.portIdx), PIN_TLF_WDI.pinIdx);
    } else if (elapsedMs >= (s_wdtClosedWindowMs + s_wdtOpenWindowMs))
    {
        /* Missed the open window — service immediately and log.
         * The PMIC may have already triggered a reset. */
        s_wdtLastServiceMs = Stm_GetTimeMs();
        (void)Tlf35585_WriteReg(TLF_REG_WWDSCMD, 0x0000u);
        Debug_Print("[TLF] WARNING: watchdog window missed\r\n");
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