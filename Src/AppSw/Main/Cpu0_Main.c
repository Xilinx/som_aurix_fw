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
#include "Eru_FaultIsr.h"
#include "VoltMon.h"
#include "Tlf35585.h"

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
    Eru_RegisterCallback(ERU_CB_WD_STROBE, ComHpcWdt_OnStrobeIsr);
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
    UsbPdManager_Init();
    Debug_Print("[SYS] Init: UsbPdManager OK\r\n");
#endif

    Debug_Print("[SYS] Entering main loop\r\n");

    for (;;)
    {
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
        SysMonitor_Run();
        ComHpcWdt_Run();

        if (PowerManager_GetState() == PM_STATE_ON)
        {
            static uint32 s_usbPdLastMs = 0u;
            uint32 nowMs = Stm_GetTimeMs();
            if ((nowMs - s_usbPdLastMs) >= 5u)
            {
                s_usbPdLastMs = nowMs;
                UsbPdManager_Run();
            }
        }
#endif
    }

    return 0;
}
