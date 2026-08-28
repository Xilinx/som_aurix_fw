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
 * @file    Eru_FaultIsr.c
 * @brief   ERU edge-triggered fault interrupt configuration and ISR handlers.
 *
 * ERU channel mapping (from GP_AURIX_Subsystem_PinDefn.xlsx):
 *   Input Ch 2  P10.2  CARRIER_HOT   -> OGU0  -> SRC_SCUERU0  (falling edge)
 *   Input Ch 3  P10.3  THERMTRIP#    -> OGU1  -> SRC_SCUERU1  (falling edge)
 *   Input Ch 7  P20.9  WD_STROBE#    -> OGU2  -> SRC_SCUERU2  (rising edge)
 *
 * iLLD: IfxScuEru.h (Scu/Std), IfxSrc.h (Src/Std)
 *
 * NOTE: If IfxScuEru_ExternalInputSelection values don't match your iLLD,
 *       grep IfxScuEru_PinMap.h or IfxScuEru.h for the P10.2/P10.3/P20.9
 *       input selection enum values.
 */

#include "Eru_FaultIsr.h"
#include "IfxScuEru.h"
#include "IfxSrc.h"
#include "IfxCpu_Irq.h"
#include "Uart_Debug.h"

/* ---- Callback table ----------------------------------------------------- */

static Eru_Callback_t s_callbacks[ERU_CB_COUNT] = {
    NULL_PTR,   /* ERU_CB_THERMTRIP */
    NULL_PTR,   /* ERU_CB_CARRIER_HOT */
    NULL_PTR    /* ERU_CB_WD_STROBE */
};

/* ---- ISR Handlers -------------------------------------------------------
 * IFX_INTERRUPT(name, coreId, priority)
 * These run on CPU0.  Keep handlers minimal — set a flag or call a
 * short callback.  Heavy processing belongs in the main loop.
 * --------------------------------------------------------------------- */

IFX_INTERRUPT(eruCarrierHotISR, 0, ERU_PRIO_CARRIER_HOT)
{
    if (s_callbacks[ERU_CB_CARRIER_HOT] != NULL_PTR)
    {
        s_callbacks[ERU_CB_CARRIER_HOT]();
    }
}

IFX_INTERRUPT(eruThermtripISR, 0, ERU_PRIO_THERMTRIP)
{
    if (s_callbacks[ERU_CB_THERMTRIP] != NULL_PTR)
    {
        s_callbacks[ERU_CB_THERMTRIP]();
    }
}

IFX_INTERRUPT(eruWdStrobeISR, 0, ERU_PRIO_WD_STROBE)
{
    if (s_callbacks[ERU_CB_WD_STROBE] != NULL_PTR)
    {
        s_callbacks[ERU_CB_WD_STROBE]();
    }
}

/* ---- Public API --------------------------------------------------------- */

void Eru_RegisterCallback(Eru_CallbackId_t id, Eru_Callback_t cb)
{
    if (id < ERU_CB_COUNT)
    {
        s_callbacks[id] = cb;
    }
}

void Eru_FaultIsr_Init(void)
{
    volatile Ifx_SRC_SRCR *src;

    Debug_Print("[ERU] Init: configuring fault interrupts...\r\n");

    /* ==================================================================
     * CARRIER_HOT — ERU Input Channel 2, P10.2, falling edge -> OGU0
     * ================================================================== */

    /* Select which physical pin drives input channel 2.
     * ExternalInputSelection_0 is typically the default for the primary
     * pin.  If P10.2 isn't selection 0 for channel 2 on TC387, check:
     *   grep "InputChannel_2" $(ILLD_ROOT)/Scu/Std/IfxScuEru.h
     * or use IfxScuEru_initReqPin() with the pin map object. */
    IfxScuEru_selectExternalInput(IfxScuEru_InputChannel_2,
                                  IfxScuEru_ExternalInputSelection_0);

    IfxScuEru_enableFallingEdgeDetection(IfxScuEru_InputChannel_2);
    IfxScuEru_disableRisingEdgeDetection(IfxScuEru_InputChannel_2);
    IfxScuEru_enableTriggerPulse(IfxScuEru_InputChannel_2);
    IfxScuEru_connectTrigger(IfxScuEru_InputChannel_2,
                             IfxScuEru_OutputChannel_0);
    IfxScuEru_setInterruptGatingPattern(IfxScuEru_OutputChannel_0,
                                        IfxScuEru_InterruptGatingPattern_alwaysActive);

    /* Enable SRN for OGU0 */
    src = &MODULE_SRC.SCU.SCUERU[0];
    IfxSrc_init(src, IfxSrc_Tos_cpu0, ERU_PRIO_CARRIER_HOT);
    IfxSrc_enable(src);

    /* Install ISR */
    IfxCpu_Irq_installInterruptHandler(&eruCarrierHotISR, ERU_PRIO_CARRIER_HOT);

    Debug_Print("[ERU] CARRIER_HOT (P10.2): falling edge -> OGU0\r\n");

    /* ==================================================================
     * THERMTRIP# — ERU Input Channel 3, P10.3, falling edge -> OGU1
     * ================================================================== */

    IfxScuEru_selectExternalInput(IfxScuEru_InputChannel_3,
                                  IfxScuEru_ExternalInputSelection_0);

//  IfxScuEru_enableFallingEdgeDetection(IfxScuEru_InputChannel_3);
//  IfxScuEru_disableRisingEdgeDetection(IfxScuEru_InputChannel_3);
    IfxScuEru_disableFallingEdgeDetection(IfxScuEru_InputChannel_3);    //;THERMTRIL
    IfxScuEru_enableRisingEdgeDetection(IfxScuEru_InputChannel_3);      //;THERMTRIL
    IfxScuEru_enableTriggerPulse(IfxScuEru_InputChannel_3);
    IfxScuEru_connectTrigger(IfxScuEru_InputChannel_3,
                             IfxScuEru_OutputChannel_1);
    IfxScuEru_setInterruptGatingPattern(IfxScuEru_OutputChannel_1,
                                        IfxScuEru_InterruptGatingPattern_alwaysActive);

    src = &MODULE_SRC.SCU.SCUERU[1];
    IfxSrc_init(src, IfxSrc_Tos_cpu0, ERU_PRIO_THERMTRIP);
    IfxSrc_enable(src);

    IfxCpu_Irq_installInterruptHandler(&eruThermtripISR, ERU_PRIO_THERMTRIP);

//  Debug_Print("[ERU] THERMTRIP# (P10.3): falling edge -> OGU1\r\n");
    Debug_Print("[ERU] THERMTRIP# (P10.3): Rising edge -> OGU1\r\n");

    /* ==================================================================
     * WD_STROBE# — ERU Input Channel 7, P20.9, rising edge -> OGU2
     *
     * Rising edge: the host asserts a strobe pulse to prevent the
     * watchdog from timing out.  The ISR resets the WDT counter.
     * ================================================================== */

    IfxScuEru_selectExternalInput(IfxScuEru_InputChannel_7,
                                  IfxScuEru_ExternalInputSelection_0);

    IfxScuEru_disableFallingEdgeDetection(IfxScuEru_InputChannel_7);
    IfxScuEru_enableRisingEdgeDetection(IfxScuEru_InputChannel_7);
    IfxScuEru_enableTriggerPulse(IfxScuEru_InputChannel_7);
    IfxScuEru_connectTrigger(IfxScuEru_InputChannel_7,
                             IfxScuEru_OutputChannel_2);
    IfxScuEru_setInterruptGatingPattern(IfxScuEru_OutputChannel_2,
                                        IfxScuEru_InterruptGatingPattern_alwaysActive);

    src = &MODULE_SRC.SCU.SCUERU[2];
    IfxSrc_init(src, IfxSrc_Tos_cpu0, ERU_PRIO_WD_STROBE);
    IfxSrc_enable(src);

    IfxCpu_Irq_installInterruptHandler(&eruWdStrobeISR, ERU_PRIO_WD_STROBE);

    Debug_Print("[ERU] WD_STROBE# (P20.9): rising edge -> OGU2\r\n");

    Debug_Print("[ERU] Init complete.\r\n");
}