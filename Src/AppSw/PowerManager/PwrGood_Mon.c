/**
 * @file    PwrGood_Mon.c
 * @brief   Power good debounce monitor implementation.
 */

#include "PwrGood_Mon.h"
#include "AppPin.h"
#include "Stm_Timer.h"
#include "Uart_Debug.h"
#include "IfxPort.h"
#include <string.h>

#define MAX_MONITORED_RAILS     12u

typedef struct
{
    const PwrRail_Cfg_t *rail;
    uint8                stableCount;   /* consecutive PG-low reads */
} MonEntry_t;

static MonEntry_t   s_mon[MAX_MONITORED_RAILS];
static uint8        s_monCount  = 0u;
static PgFaultCb_t  s_faultCb   = NULL_PTR;
static boolean      s_armed     = FALSE;

static boolean prv_ReadPg(const PwrRail_Cfg_t *rail)
{
    return (boolean)IfxPort_getPinState(
        AppPin_GetPort(rail->pgoodPin.portIdx), rail->pgoodPin.pinIdx);
}

void PwrGood_MonArm(const PwrRail_Cfg_t *rails, uint8 count, PgFaultCb_t faultCb)
{
    if ((rails == NULL_PTR) || (count == 0u) || (count > MAX_MONITORED_RAILS))
    {
        return;
    }

    s_monCount = count;
    s_faultCb  = faultCb;

    for (uint8 i = 0u; i < count; i++)
    {
        s_mon[i].rail        = &rails[i];
        s_mon[i].stableCount = 0u;
    }

    s_armed = TRUE;
}

void PwrGood_MonDisarm(void)
{
    s_armed    = FALSE;
    s_monCount = 0u;
    s_faultCb  = NULL_PTR;
}

void PwrGood_MonRun(void)
{
    if (!s_armed)
    {
        return;
    }

    for (uint8 i = 0u; i < s_monCount; i++)
    {
        if (prv_ReadPg(s_mon[i].rail) == FALSE)
        {
            /* PG deasserted — increment debounce counter */
            s_mon[i].stableCount++;
            if (s_mon[i].stableCount >= PM_PG_DEBOUNCE_POLLS)
            {
                /* Confirmed loss — disarm before callback to avoid re-entry */
                PwrGood_MonDisarm();
                if (s_faultCb != NULL_PTR)
                {
                    s_faultCb(s_mon[i].rail, i);
                }
                return;
            }
        }
        else
        {
            /* PG asserted — reset debounce counter */
            s_mon[i].stableCount = 0u;
        }
    }
}

boolean PwrGood_WaitAllPg(const PwrRail_Cfg_t *rails, uint8 count,
                           uint32 rampDelayMs, uint32 timeoutMs,
                           uint8 *failIdx)
{
    /* Wait for VRM ramp before checking PG. */
    Stm_DelayMs(rampDelayMs);

    uint32 deadline = Stm_GetTimeMs() + timeoutMs;

    for (uint8 i = 0u; i < count; i++)
    {
        /* Wait for each rail in sequence. */
        while (prv_ReadPg(&rails[i]) == FALSE)
        {
            if (Stm_GetTimeMs() > deadline)
            {
                if (failIdx != NULL_PTR)
                {
                    *failIdx = i;
                }
                return FALSE;
            }
        }
        Debug_Printf("[PM] PG OK: %s\r\n", rails[i].name);
    }

    return TRUE;
}
