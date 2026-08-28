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
 * @file    Clk_Cfg.c
 * @brief   SCU / PLL clock initialisation for TC387 at 300 MHz.
 *
 * Tasking iLLD watchdog API differences vs HIGHTEC iLLD:
 *   IfxScuWdt_getCpuWatchdogPassword()      — no argument
 *   IfxScuWdt_disableCpuWatchdog(password)  — password only
 *   IfxScuWdt_enableCpuWatchdog(password)   — password only
 */

#include "Clk_Cfg.h"
#include "IfxScuCcu.h"
#include "IfxScuWdt.h"

void Clk_Init(void)
{
    IfxScuCcu_Config ccuCfg;
    IfxScuCcu_initConfig(&ccuCfg);
    IfxScuCcu_init(&ccuCfg);
}
