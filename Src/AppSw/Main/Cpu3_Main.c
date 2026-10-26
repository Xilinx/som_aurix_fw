/**
 * @file    Cpu3_Main.c
 * @brief   CPU3 — USB-PD core
 *
 * Owns: UsbPd_Hpd, UsbPd_ApuProxy, UsbPd_Manager, and the I2C0
 * sideband slave at 0x54. CYPD master and that slave share MODULE_I2C0;
 * master transfers park the slave and restore it.
 * Reads: g_ipcShared.pmc.pmState (published by CPU1) to gate HPI polling.
 * Does not touch I2C1 (APML, CPU1, master only).
 *
 * Loop is 50 ms. USB-PD is not a 5 ms safety task.
 */

#include "Ifx_Types.h"
#include "IfxCpu.h"
#include "IfxScuWdt.h"
#include "Ipc.h"
#include "Stm_Timer.h"
#include "Uart_Debug.h"
#include "PowerManager.h"
#include "UsbPd_Manager.h"
#include "UsbPd_Hpd.h"
#include "UsbPd_ApuProxy.h"
#include "UsbPd_Cfg.h"
#include "I2c_Slave.h"

#ifndef USBPD_LOOP_PERIOD_MS
#define USBPD_LOOP_PERIOD_MS    50u
#endif

void core3_main(void)
{
    IfxScuWdt_disableCpuWatchdog(IfxScuWdt_getCpuWatchdogPassword());
    IfxCpu_enableInterrupts();

    Ipc_WaitReleasedBarrier();

#if !defined(TARGET_EVAL_BOARD) && (USBPD_FEATURE_ENABLE == 1u)
    Debug_Print("[CPU3] Init: UsbPd...\r\n");
    UsbPd_Hpd_Init();
    UsbPd_ApuProxy_Init();
    I2cSlave_Init(NULL_PTR);
    UsbPdManager_Init();
    Debug_Print("[CPU3] Init: UsbPd OK\r\n");
#else
    Debug_Print("[CPU3] UsbPd skipped\r\n");
#endif

    Debug_Print("[CPU3] Ready\r\n");
    g_ipcShared.cpu3Ready = TRUE;
    __dsync();

    for (;;)
    {
        uint32 loopStartMs = Stm_GetTimeMs();

#if !defined(TARGET_EVAL_BOARD) && (USBPD_FEATURE_ENABLE == 1u)
        UsbPd_Hpd_Run();
        /* ON is CPU1's sequencer state, published into LMU before APML.
         * Do not call PowerManager_GetState() — that static lives with CPU1. */
        if (g_ipcShared.pmc.pmState == (uint32)PM_STATE_ON)
        {
            UsbPdManager_Run();
        }
#endif

        {
            uint32 elapsed = Stm_GetTimeMs() - loopStartMs;
            if (elapsed < USBPD_LOOP_PERIOD_MS)
            {
                Stm_DelayMs(USBPD_LOOP_PERIOD_MS - elapsed);
            }
        }
    }
}
