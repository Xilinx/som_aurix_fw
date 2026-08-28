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
 * @file    Cpu0_Main.c
 * @brief   CPU0 entry point for the TC387 COM-HPC power and USB PD controller.
 * 
 * Initialisation order:
 *   1. Watchdog disable (development mode)
 *   2. SCU clock init — 300 MHz
 *   3. GPIO port direction init
 *   4. STM0 timer init
 *   5. ASCLIN0 debug UART init
 *   6. I2C master init
 *   7. Power manager init
 *   8. System monitor (SoM only)
 *   9. ERU fault ISRs
 *  10. Voltage monitoring
 *  11. COM-HPC watchdog (SoM only)
 *  12. USB PD manager (SoM only)
 * 
 * Build with BOARD=eval to exclude SoM-specific peripherals.
 */

#include "Ifx_Types.h"
#include "IfxCpu.h"
#include "IfxScuWdt.h"
#include "Clk_Cfg.h"
#include "Port_Init.h"
#include "Stm_Timer.h"
#include "Uart_Debug.h"
#include "I2c_Master.h"
#include "PowerManager.h"
#include "Platform_Cfg.h"
#include "Eru_FaultIsr.h"
#include "VoltMon.h"
#include "Tlf35585.h"
#include "UsbPd_Cfg.h"

#if !defined(TARGET_EVAL_BOARD)
#include "UsbPd_Manager.h"
#include "SysMonitor.h"
#include "ComHpcWdt.h"
#endif


/* Banner printed on UART at startup */
#if defined(TARGET_EVAL_BOARD)
#define FW_VERSION_STR  "TC387 COM-HPC Controller v0.1 [EVAL BOARD]\r\n"
#else
#define FW_VERSION_STR  "TC387 COM-HPC Controller v0.1\r\n"
#endif

int core0_main(void)
{
    IfxCpu_enableInterrupts();
    IfxScuWdt_disableCpuWatchdog(IfxScuWdt_getCpuWatchdogPassword());
    IfxScuWdt_disableSafetyWatchdog(IfxScuWdt_getSafetyWatchdogPassword());
    
    Clk_Init();
    Port_Init();
    Stm_Init();

    Debug_Init();
    Debug_Print("\r\n" FW_VERSION_STR);
    Debug_Print("[SYS] Init: UART OK\r\n");

    I2cMaster_Init();
    Debug_Print("[SYS] Init: I2C OK\r\n");

    PowerManager_Init();
    Debug_Print("[SYS] Init: PowerManager OK\r\n");

#if !defined(TARGET_EVAL_BOARD)
    SysMonitor_Init();
    SysMonitor_RegisterShutdownCb(PowerManager_OnThermtripIsr);
    Debug_Print("[SYS] Init: SysMonitor OK\r\n");
#endif

    Eru_RegisterCallback(ERU_CB_THERMTRIP, PowerManager_OnThermtripIsr);
#if !defined(TARGET_EVAL_BOARD)
#if (SYSMON_CARRIER_WD_ENABLE == 1u)
    Eru_RegisterCallback(ERU_CB_WD_STROBE, ComHpcWdt_OnStrobeIsr);
#endif
#endif
    Eru_FaultIsr_Init();
    Debug_Print("[SYS] Init: ERU fault ISRs OK\r\n");

    VoltMon_Init();
    VoltMon_RegisterFaultCb(PowerManager_OnVoltageFault);
    Debug_Print("[SYS] Init: VoltMon OK\r\n");

#if !defined(TARGET_EVAL_BOARD)
    ComHpcWdt_Init();
    Debug_Print("[SYS] Init: ComHpcWdt OK\r\n");
#endif

#if !defined(TARGET_EVAL_BOARD)
#if (USBPD_FEATURE_ENABLE == 1u)
    UsbPdManager_Init();
    Debug_Print("[SYS] Init: UsbPdManager OK\r\n");
#endif
#endif

    Debug_Print("[SYS] Entering main loop\r\n");


    for (;;)
    {
        uint32 loopStartMs = Stm_GetTimeMs();

        //IfxPort_togglePin(&MODULE_P34,4);

        PowerManager_Run();
        VoltMon_Scan();

#if defined(TARGET_EVAL_BOARD)
        {
            static uint32 s_lastReportMs = 0u;
            uint32 nowMs = Stm_GetTimeMs();
            if ((nowMs - s_lastReportMs) >= 2000u)
            {
                s_lastReportMs = nowMs;
                VoltMon_PrintReport();
            }
        }
#endif

#if !defined(TARGET_EVAL_BOARD)
        Tlf35585_ServiceWdt();
#if (SYSMON_CARRIER_WD_ENABLE == 1u)
        ComHpcWdt_Run();
#endif

        if (PowerManager_GetState() == PM_STATE_ON)
        {
            SysMonitor_Run();
#if (USBPD_FEATURE_ENABLE == 1u)
            UsbPdManager_Run();
#endif
        }
#endif

        /* Fixed-period pacing: makes debounce/timeout constants throughout
         * PowerManager/SysMonitor map to real elapsed time, and surfaces
         * WCET overruns instead of silently letting the loop free-run. */
        {
            static uint32 s_lastOverrunLogMs = 0u;
            uint32 elapsedMs = Stm_GetTimeMs() - loopStartMs;
            if (elapsedMs < MAIN_LOOP_PERIOD_MS)
            {
                Stm_DelayMs(MAIN_LOOP_PERIOD_MS - elapsedMs);
            }
            else
            {
                uint32 nowMs = Stm_GetTimeMs();
                if ((nowMs - s_lastOverrunLogMs) >= MAIN_LOOP_OVERRUN_LOG_INTERVAL_MS)
                {
                    s_lastOverrunLogMs = nowMs;
                    Debug_Printf("[MAIN] loop overrun: %ums (budget %ums)\r\n",
                                 (unsigned)elapsedMs, (unsigned)MAIN_LOOP_PERIOD_MS);
                }
            }
        }
    }
    return 0;
}
