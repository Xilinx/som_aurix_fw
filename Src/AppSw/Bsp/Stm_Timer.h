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
 * @file    Stm_Timer.h
 * @brief   STM-based microsecond / millisecond timer for CPU0.
 *
 * Uses STM0 (System Timer Module 0) which is always-on and clocked from
 * the SRI clock. Provides a monotonic 64-bit tick counter and blocking
 * delay helpers used by the power manager and I2C timeout logic.
 */

#ifndef STM_TIMER_H
#define STM_TIMER_H

#include "Ifx_Types.h"

/**
 * @brief Initialise STM0 for use by CPU0.
 *        Must be called after clock initialisation.
 */
void Stm_Init(void);

/**
 * @brief Return the current time in microseconds (wraps after ~584,542 years).
 */
uint64 Stm_GetTimeMicros(void);

/**
 * @brief Return the current time in milliseconds.
 */
uint32 Stm_GetTimeMs(void);

/**
 * @brief Blocking delay. Spins for at least the requested number of milliseconds.
 *        Do not call from an interrupt context.
 */
void Stm_DelayMs(uint32 ms);

/**
 * @brief Blocking delay in microseconds.
 */
void Stm_DelayUs(uint32 us);

/**
 * @brief Return TRUE if the given number of milliseconds has elapsed since
 *        *pTimestamp. On first call pass 0 in *pTimestamp.
 *        Updates *pTimestamp when the timeout fires so it can be reused as a
 *        periodic interval timer.
 */
boolean Stm_IsElapsedMs(uint32 *pTimestamp, uint32 periodMs);

#endif /* STM_TIMER_H */
