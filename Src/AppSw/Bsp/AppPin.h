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
 * @file    AppPin.h
 * @brief   Toolchain-agnostic GPIO pin reference type.
 *
 * Replaces IfxPort_Pin / IfxPort_Pxx_y throughout the application layer.
 * Tasking iLLD defines IfxPort_Pxx_y as extern variables (not macros), which
 * cannot appear in aggregate initialisers (Tasking E306/E272/E333).
 *
 * AppPin_t stores only uint8 integers — always valid constant expressions.
 * AppPin_GetPort() maps portIdx to the Ifx_P* register pointer at runtime.
 */

#ifndef APPPIN_H
#define APPPIN_H

#include "Ifx_Types.h"
#include "IfxPort.h"

/**
 * @brief Board-level GPIO pin reference.
 *        portIdx: TC387 port number (e.g. 0 for P0, 33 for P33).
 *        pinIdx:  pin index within that port (0–15).
 */
typedef struct
{
    uint8 portIdx;
    uint8 pinIdx;
} AppPin_t;

/**
 * @brief Return the Ifx_P* register base for the given port index.
 *        Returns NULL_PTR for unmapped indices.
 */
Ifx_P* AppPin_GetPort(uint8 portIdx);

#endif /* APPPIN_H */
