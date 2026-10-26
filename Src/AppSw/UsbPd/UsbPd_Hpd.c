/**
 * @file    UsbPd_Hpd.c
 * @brief   Virtual HPD GPIO driver for DisplayPort Alt Mode
 */

#include "UsbPd_Hpd.h"
#include "Platform_PinCfg.h"
#include "AppPin.h"
#include "Stm_Timer.h"
#include "Uart_Debug.h"
#include "NvLog.h"
#include "IfxPort.h"

/* VESA IRQ_HPD is a 0.5-1 ms low pulse. A low longer than 2 ms is an
 * unplug. Do not wait for UsbPd_Hpd_Run() on the 50 ms CPU3 loop. */
#define HPD_IRQ_PULSE_US    1000u

/* ================================================================== */
/*  State — two ports                                                 */
/* ================================================================== */

static UsbPd_HpdState_t s_hpd[2];

/* ================================================================== */
/*  Private helpers                                                   */
/* ================================================================== */

static void prv_SetHpdPin(uint8 portIdx, boolean high)
{
    const AppPin_t *pin;

    if (portIdx == 0u)
        pin = &PIN_DP2_HPD;
    else
        pin = &PIN_DP3_HPD;

    if (high)
        IfxPort_setPinHigh(AppPin_GetPort(pin->portIdx), pin->pinIdx);
    else
        IfxPort_setPinLow(AppPin_GetPort(pin->portIdx), pin->pinIdx);
}

/* ================================================================== */
/*  Public API                                                        */
/* ================================================================== */

void UsbPd_Hpd_Init(void)
{
    uint8 i;
    for (i = 0u; i < 2u; i++)
    {
        s_hpd[i].hpdLevel    = FALSE;
        s_hpd[i].irqPending  = FALSE;
        s_hpd[i].irqStartMs  = 0u;
        s_hpd[i].lastEventMs = 0u;
        s_hpd[i].lastEvent   = HPD_EVENT_NONE;
        prv_SetHpdPin(i, FALSE);
    }
    Debug_Print("[HPD] Init: DP2_HPD=L, DP3_HPD=L\r\n");
}

void UsbPd_Hpd_ProcessEvent(uint8 portIdx, UsbPd_HpdEvent_t event)
{
    if (portIdx > 1u) return;

    s_hpd[portIdx].lastEvent   = event;
    s_hpd[portIdx].lastEventMs = Stm_GetTimeMs();

    switch (event)
    {
        case HPD_EVENT_HIGH:
            s_hpd[portIdx].hpdLevel = TRUE;
            prv_SetHpdPin(portIdx, TRUE);
            Debug_Printf("[HPD] Port %u: HIGH (DP connected)\r\n",
                         (unsigned)portIdx);
            NvLog_WriteU32(NVLOG_EVT_USBPD_HPD, NVLOG_SRC_USBPD,
                           NVLOG_SEV_INFO,
                           ((uint32)portIdx << 8u) | (uint32)event);
            break;

        case HPD_EVENT_LOW:
            s_hpd[portIdx].hpdLevel = FALSE;
            s_hpd[portIdx].irqPending = FALSE;
            prv_SetHpdPin(portIdx, FALSE);
            Debug_Printf("[HPD] Port %u: LOW (DP disconnected)\r\n",
                         (unsigned)portIdx);
            NvLog_WriteU32(NVLOG_EVT_USBPD_HPD, NVLOG_SRC_USBPD,
                           NVLOG_SEV_INFO,
                           ((uint32)portIdx << 8u) | (uint32)event);
            break;

        case HPD_EVENT_IRQ:
            /* Finish the pulse here. Cpu3_Main only calls Run() every
             * 50 ms, which would look like an unplug. */
            if (s_hpd[portIdx].hpdLevel)
            {
                prv_SetHpdPin(portIdx, FALSE);
                Stm_DelayUs(HPD_IRQ_PULSE_US);
                prv_SetHpdPin(portIdx, TRUE);
                s_hpd[portIdx].irqPending = FALSE;
                Debug_Printf("[HPD] Port %u: IRQ pulse\r\n",
                             (unsigned)portIdx);
            }
            break;

        default:
            break;
    }
}

void UsbPd_Hpd_Run(void)
{
    uint8 i;
    for (i = 0u; i < 2u; i++)
    {
        if (s_hpd[i].irqPending)
        {
            prv_SetHpdPin(i, TRUE);
            s_hpd[i].irqPending = FALSE;
        }
    }
}

void UsbPd_Hpd_DeassertAll(void)
{
    uint8 i;
    for (i = 0u; i < 2u; i++)
    {
        s_hpd[i].hpdLevel   = FALSE;
        s_hpd[i].irqPending = FALSE;
        prv_SetHpdPin(i, FALSE);
    }
}

boolean UsbPd_Hpd_GetLevel(uint8 portIdx)
{
    if (portIdx > 1u) return FALSE;
    return s_hpd[portIdx].hpdLevel;
}