/**
 * @file    Cpu2_Main.c
 * @brief   CPU2 — firmware management and steady-state debug UART
 *
 * After CPU0 finishes boot, this core takes ASCLIN0, NvLog, FwUpdate,
 * the debug CLI, and PFlash updates. FuSa and the TLF watchdog stay
 * on CPU0, so a PFlash sector erase here does not stop the window
 * service. This core must not wait on g_wdtOwner.
 */

#include "Ifx_Types.h"
#include "IfxCpu.h"
#include "IfxScuWdt.h"
#include "Ipc.h"
#include "Stm_Timer.h"
#include "Uart_Debug.h"
#include "PFlash.h"
#include "NvLog.h"
#include "FwUpdate.h"
#include "DebugCli.h"
#include "SelfTest.h"

extern IfxCpu_syncEvent g_cpuSyncEvent;

#ifndef CPU2_LOOP_PERIOD_MS
#define CPU2_LOOP_PERIOD_MS     50u
#endif

void core2_main(void)
{
    *(volatile uint32 *)0xB0050004u = 0xC2C2C2C2u;
    IfxCpu_enableInterrupts();
    IfxScuWdt_disableCpuWatchdog(IfxScuWdt_getCpuWatchdogPassword());

    IfxCpu_emitEvent(&g_cpuSyncEvent);
    while ((g_cpuSyncEvent & CORE_SYNC_MASK) != CORE_SYNC_MASK){}

    Debug_Print("[CPU2] Ready (FuSa is on CPU0)\r\n");
    g_ipcShared.cpu2Ready = TRUE;
    __dsync();

    for (;;)
    {
        uint32 loopStartMs = Stm_GetTimeMs();

        if (g_ipcShared.uartOwner == 0u)
        {
            g_ipcShared.uartOwner = 1u;
            __dsync();
        }
        else if (g_ipcShared.uartOwner == 2u)
        {
            Debug_ClaimSteadyState();
            g_ipcShared.uartOwner = 3u;
            __dsync();
            /* 0x70100000 is this core's PSPR. Fill it before any erase. */
            PFlash_Init();
            DebugCli_Init();
            Debug_Print("[CPU2] debug UART, CLI, NvLog, FwUpdate\r\n");
        }

        if (g_ipcShared.uartOwner == 3u)
        {
            NvLog_Run();
            FwUpdate_Run();
            DebugCli_Run();
            DebugCli_Poll();
            prv_CommitSotaOnce();
            prv_ForwardVoltageFaults();
            prv_ForwardVoltageWarnings();
            prv_ForwardTlfEvents();
            Debug_DrainRings();
        }

        {
            uint32 elapsed = Stm_GetTimeMs() - loopStartMs;
            if (elapsed < CPU2_LOOP_PERIOD_MS)
            {
                Stm_DelayMs(CPU2_LOOP_PERIOD_MS - elapsed);
            }
        }
    }
}
