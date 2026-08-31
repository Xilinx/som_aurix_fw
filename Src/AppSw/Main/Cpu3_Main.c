/**
 * @file    Cpu3_Main.c
 * @brief   CPU3 — IDLE Core
 *
 * Owns: Nothing, set to IDLE
 *
 * Design: minimal shared data flow, CPU2 writes only, others read.
 * Peripheral access is exclusive — no other core touches EVADC/QSPI2/QSPI3.
 */


#include "Ifx_Types.h"
#include "IfxCpu.h"
#include "IfxScuWdt.h"
#include "IfxPort.h"
#include "Ipc.h"
#include "Stm_Timer.h"
#include "Uart_Debug.h"
#include "VoltMon.h"
#include "Tlf35585.h"
#include "FusaSpi.h"
#include "Platform_Cfg.h"
#include "Platform_PinCfg.h"
#include "AppPin.h"

extern IfxCpu_syncEvent g_cpuSyncEvent;

void core3_main(void)
{
    *(volatile uint32 *)0xB0050008u = 0xC3C3C3C3u;
    IfxScuWdt_disableCpuWatchdog(IfxScuWdt_getCpuWatchdogPassword());
    IfxCpu_emitEvent(&g_cpuSyncEvent);
    while ((g_cpuSyncEvent & CORE_SYNC_MASK) != CORE_SYNC_MASK){}
    for (;;) { __nop(); }
}
