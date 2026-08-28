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
 * @file    UsbPd_Cfg.c
 * @brief   CYPD6129 device configuration table — runtime initialisation.
 *
 * AppPin_t members are uint8 only — struct assignment from extern const
 * AppPin_t objects is a plain runtime copy, no Tasking E306 restriction.
 */

#include "UsbPd_Cfg.h"

Cypd_DevCfg_t CYPD_DEVICES[CYPD_DEVICE_COUNT];

void UsbPd_CfgInit(void)
{
    CYPD_DEVICES[0].i2cAddr  = CYPD_PORT0_I2C_ADDR;
    CYPD_DEVICES[0].i2cBus   = 0u;   /* I2C0 on robotics carrier */
    CYPD_DEVICES[0].intPin   = PIN_CYPD0_INT_L;
    CYPD_DEVICES[0].resetPin = PIN_CYPD0_RESET_L;
    CYPD_DEVICES[0].name     = "CYPD_P0";

    CYPD_DEVICES[1].i2cAddr  = CYPD_PORT1_I2C_ADDR;
    CYPD_DEVICES[1].i2cBus   = 0u;   /* I2C0 - Shared USB-PD sideband */
    CYPD_DEVICES[1].intPin   = PIN_CYPD1_INT_L;
    CYPD_DEVICES[1].resetPin = PIN_CYPD1_RESET_L;
    CYPD_DEVICES[1].name     = "CYPD_P1";
}