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
 * @file    Eru_FaultIsr.h
 * @brief   ERU-based edge-triggered fault interrupts for safety-critical pins.
 *
 * Configures the SCU ERU to generate interrupts on:
 *   ERUIN2  P10.2  CARRIER_HOT    — falling edge (active low)
 *   ERUIN3  P10.3  THERMTRIP#     — falling edge (active low)
 *   ERUIN7  P20.9  WD_STROBE#     — rising edge  (host heartbeat)
 *
 * Each ISR invokes a registered callback.  PowerManager and SysMonitor
 * register their handlers during init via Eru_RegisterCallback().
 *
 * iLLD headers: IfxScuEru.h, IfxSrc.h
 */

#ifndef ERU_FAULTISR_H
#define ERU_FAULTISR_H

#include "Ifx_Types.h"

/* ISR priority assignments — keep unique across the project.
 * Update Ifx_IntPrioDef.h if the project uses a centralised table. */
#define ERU_PRIO_THERMTRIP      20u
#define ERU_PRIO_CARRIER_HOT    21u
#define ERU_PRIO_WD_STROBE      22u

/* Callback identifiers */
typedef enum
{
    ERU_CB_THERMTRIP    = 0u,
    ERU_CB_CARRIER_HOT  = 1u,
    ERU_CB_WD_STROBE    = 2u,
    ERU_CB_COUNT        = 3u
} Eru_CallbackId_t;

/* Callback function type — called from ISR context, keep short. */
typedef void (*Eru_Callback_t)(void);

/**
 * @brief  Initialise the ERU channels and enable interrupts.
 *
 * Must be called after Port_Init() so pins are already configured
 * as inputs.  Does NOT register callbacks — call Eru_RegisterCallback()
 * before enabling interrupts if you need a handler from the first edge.
 */
void Eru_FaultIsr_Init(void);

/**
 * @brief  Register a callback for a specific ERU event.
 * @param  id   Which event (THERMTRIP, CARRIER_HOT, WD_STROBE)
 * @param  cb   Function pointer, or NULL_PTR to clear
 */
void Eru_RegisterCallback(Eru_CallbackId_t id, Eru_Callback_t cb);

#endif /* ERU_FAULTISR_H */