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
 * @file    PwrGood_Mon.h
 * @brief   Power good polling and debounce monitor.
 *
 * PwrGood_Mon watches a set of rails and debounces their PG inputs.
 * It is called on every main loop iteration. When any monitored rail
 * loses power good the registered fault callback is invoked with the
 * index of the failing rail within the set.
 */

#ifndef PWRGOOD_MON_H
#define PWRGOOD_MON_H

#include "PowerManager_Cfg.h"
#include "Ifx_Types.h"

/** Callback invoked when a rail PG is lost. railIdx is the table index. */
typedef void (*PgFaultCb_t)(const PwrRail_Cfg_t *rail, uint8 railIdx);

/**
 * @brief Arm monitoring for a set of rails.
 *        Previous monitoring is cleared and replaced.
 * @param rails     Pointer to rail config array.
 * @param count     Number of rails in the array.
 * @param faultCb   Function called on PG loss.
 */
void PwrGood_MonArm(const PwrRail_Cfg_t *rails, uint8 count, PgFaultCb_t faultCb);

/**
 * @brief Disarm all monitoring (called during power-down or fault recovery).
 */
void PwrGood_MonDisarm(void);

/**
 * @brief Run one monitoring iteration.  Call from the main loop.
 *        Reads each armed rail's PG pin and debounces before reporting fault.
 */
void PwrGood_MonRun(void);

/**
 * @brief Poll until all rails in the set have asserted PG, or timeout.
 * @return TRUE if all PG asserted within timeout, FALSE on timeout.
 */
boolean PwrGood_WaitAllPg(const PwrRail_Cfg_t *rails, uint8 count,
                           uint32 rampDelayMs, uint32 timeoutMs,
                           uint8 *failIdx);

#endif /* PWRGOOD_MON_H */
