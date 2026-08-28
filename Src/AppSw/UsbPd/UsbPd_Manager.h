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
 * @file    UsbPd_Manager.h
 * @brief   Dual-port USB PD management interface.
 */

#ifndef USBPD_MANAGER_H
#define USBPD_MANAGER_H

#include "Ifx_Types.h"

#define USBPD_RUNTIME_FAIL_LIMIT   10u
#define USBPD_SPURIOUS_INT_LIMIT   100u

/** Per-port connection state tracked by the manager. */
typedef enum
{
    USBPD_PORT_DETACHED     = 0,
    USBPD_PORT_ATTACHED,
    USBPD_PORT_CONTRACT,        /* PD contract negotiated */
} UsbPd_PortState_t;

/**
 * @brief Initialise USB PD manager.
 *        Hard-resets both CYPD6129 devices and verifies HPI link.
 *        Call after I2cMaster_Init() and Port_Init().
 */
void UsbPdManager_Init(void);

/**
 * @brief Run one USB PD manager iteration. Call from main loop.
 *        Polls INT_L for each device; on assertion reads and dispatches events.
 */
void UsbPdManager_Run(void);

/**
 * @brief Return the current connection state of a port (0 or 1).
 */
UsbPd_PortState_t UsbPdManager_GetPortState(uint8 portIdx);

#endif /* USBPD_MANAGER_H */
