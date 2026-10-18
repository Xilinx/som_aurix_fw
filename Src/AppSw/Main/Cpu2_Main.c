/**
 * @file    Cpu2_Main.c
 * @brief   CPU2 — running, no peripherals in this step
 *
 * FuSa (VoltMon, TLF service, FusaSpi) runs on CPU0.
 * This core must not wait on g_wdtOwner. That wait used to hang
 * the core once ownership stopped moving off CPU0.
 */

#include "Ifx_Types.h"
#include "IfxCpu.h"
#include "IfxScuWdt.h"
#include "Ipc.h"
#include "Stm_Timer.h"
#include "Uart_Debug.h"

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
        uint32 elapsed = Stm_GetTimeMs() - loopStartMs;

        if (elapsed < CPU2_LOOP_PERIOD_MS)
        {
            Stm_DelayMs(CPU2_LOOP_PERIOD_MS - elapsed);
        }
    }
}
