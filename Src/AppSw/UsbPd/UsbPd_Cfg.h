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
 * @file    UsbPd_Cfg.h
 * @brief   USB PD port configuration for two CYPD6129 devices.
 *
 * I2C addresses are defined in Platform_Cfg.h.
 * INT_L and RESET_L pin assignments are in Platform_PinCfg.h.
 */

#ifndef USBPD_CFG_H
#define USBPD_CFG_H

#include "Platform_Cfg.h"
#include "Platform_PinCfg.h"
#include "AppPin.h"

/** Total number of CYPD6129 devices on the board. */
#define CYPD_DEVICE_COUNT       2u
#define USBPD_FEATURE_ENABLE   1u

/**
 * @brief Static configuration for each CYPD6129 instance.
 */
typedef struct
{
    uint8        i2cAddr;   /* 7-bit I2C address */
    uint8        i2cBus;    /* 0 = I2C0, 1 = I2C1 */
    AppPin_t     intPin;    /* INT_L — active low input  */
    AppPin_t     resetPin;  /* RESET_L — active low output */
    const char  *name;      /* for debug logging */
} Cypd_DevCfg_t;

/**
 * Per-device configuration table — defined and populated in UsbPd_Cfg.c.
 * NOT const: Tasking ctc E306 rejects hardware register pointers
 * (IfxPort_Pin.port = &MODULE_Pxx) in any file-scope aggregate initialiser.
 * Table is filled at runtime by UsbPd_CfgInit() before first use.
 */
extern Cypd_DevCfg_t CYPD_DEVICES[CYPD_DEVICE_COUNT];

/**
 * @brief Populate CYPD_DEVICES[]. Must be called once before any
 *        Cypd_xxx() or UsbPdManager_xxx() call.
 */
void UsbPd_CfgInit(void);

#endif /* USBPD_CFG_H */
