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
 * @file    PwrGood_Mon.c
 * @brief   Power good debounce monitor implementation.
 */

#include "PwrGood_Mon.h"
#include "AppPin.h"
#include "Stm_Timer.h"
#include "Uart_Debug.h"
#include "IfxPort.h"
#include <string.h>

#define MAX_MONITORED_RAILS     16u

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

    uint8 i;
    for (i = 0u; i < count; i++)
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
    uint8 i;

    if (!s_armed)
    {
        return;
    }
    for ( i = 0u; i < s_monCount; i++)
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
    uint8 i;
    for (i = 0u; i < count; i++)
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
