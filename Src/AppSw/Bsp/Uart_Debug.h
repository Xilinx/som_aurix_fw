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
 * @file    Uart_Debug.h
 * @brief   Polling UART debug output via ASCLIN0 (P14.0 TX / P14.1 RX).
 *
 * Provides printf-style logging. TX is fully polling — no DMA, no interrupts.
 * Call Debug_Init() once after clock and port initialisation.
 */

#ifndef UART_DEBUG_H
#define UART_DEBUG_H

#include "Ifx_Types.h"

/** Baud rate for the debug UART. */
#define UART_DEBUG_BAUD     115200u

/**
 * @brief Initialise ASCLIN0 for 115200 8N1 on P14.0/P14.1.
 */
void Debug_Init(void);

/**
 * @brief Transmit a null-terminated string (blocking).
 */
void Debug_Print(const char *str);

/**
 * @brief printf-style formatted output (blocking). Limited to 256 chars/call.
 */
void Debug_Printf(const char *fmt, ...);

#endif /* UART_DEBUG_H */
