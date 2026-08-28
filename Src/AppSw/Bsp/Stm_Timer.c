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
 * @file    Stm_Timer.c
 * @brief   STM0-based timer implementation.
 */

#include "Stm_Timer.h"
#include "IfxStm.h"
#include "IfxStm_reg.h"

/* STM0 is used exclusively by this module. */
static Ifx_STM * const STM_BASE = &MODULE_STM0;

/* Ticks per microsecond — computed from actual STM frequency at init. */
static uint32 s_ticksPerUs  = 0u;
static uint32 s_ticksPerMs  = 0u;

void Stm_Init(void)
{
    uint32 stmFreqHz = IfxStm_getFrequency(STM_BASE);
    s_ticksPerUs = stmFreqHz / 1000000u;
    s_ticksPerMs = stmFreqHz / 1000u;
}

uint64 Stm_GetTimeMicros(void)
{
    if (s_ticksPerUs == 0u)
    {
        return 0u;
    }
    return IfxStm_get(STM_BASE) / (uint64)s_ticksPerUs;
}

uint32 Stm_GetTimeMs(void)
{
    if (s_ticksPerMs == 0u)
    {
        return 0u;
    }
    return (uint32)(IfxStm_get(STM_BASE) / (uint64)s_ticksPerMs);
}

void Stm_DelayMs(uint32 ms)
{
    uint64 start = IfxStm_get(STM_BASE);
    uint64 ticks = (uint64)ms * (uint64)s_ticksPerMs;
    while ((IfxStm_get(STM_BASE) - start) < ticks)
    {
        /* spin */
    }
}

void Stm_DelayUs(uint32 us)
{
    uint64 start = IfxStm_get(STM_BASE);
    uint64 ticks = (uint64)us * (uint64)s_ticksPerUs;
    while ((IfxStm_get(STM_BASE) - start) < ticks)
    {
        /* spin */
    }
}

boolean Stm_IsElapsedMs(uint32 *pTimestamp, uint32 periodMs)
{
    uint32 now = Stm_GetTimeMs();
    if ((now - *pTimestamp) >= periodMs)
    {
        *pTimestamp = now;
        return TRUE;
    }
    return FALSE;
}
