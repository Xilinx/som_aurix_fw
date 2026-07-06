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
 *   6. I2C0 master init
 *   7. Power manager init
 *   8. USB PD manager init
 *
 * Main loop executes the power manager state machine and USB PD polling
 * on every iteration. Timing is handled internally by each module using
 * the STM-based Stm_IsElapsedMs() helper.
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
#include "UsbPd_Manager.h"
#include "SysMonitor.h"

/* Banner printed on UART at startup */
#define FW_VERSION_STR  "TC387 COM-HPC Controller v0.1\r\n"

int core0_main(void)
{
    /* ---- 1. Disable watchdog (development) -------------------------------- */
    IfxScuWdt_disableCpuWatchdog(IfxScuWdt_getCpuWatchdogPassword());
    IfxScuWdt_disableSafetyWatchdog(IfxScuWdt_getSafetyWatchdogPassword());

    /* ---- 2. Clock: 300 MHz via PLL ---------------------------------------- */
    Clk_Init();

    /* ---- 3. GPIO: configure all board pins --------------------------------- */
    Port_Init();

    /* ---- 4. System timer --------------------------------------------------- */
    Stm_Init();

    /* ---- 5. Debug UART: 115200 8N1 on P14.0/P14.1 ------------------------- */
    Debug_Init();
    Debug_Print("\r\n" FW_VERSION_STR);
    Debug_Print("[SYS] Init: UART OK\r\n");

    /* ---- 6. I2C0 master: 400 kHz on P13.1/P13.2 --------------------------- */
    I2cMaster_Init();
    Debug_Print("[SYS] Init: I2C OK\r\n");

    /* ---- 7. Power manager -------------------------------------------------- */
    PowerManager_Init();
    Debug_Print("[SYS] Init: PowerManager OK\r\n");

    /* ---- 8. System monitor (PROCHOT / CATERR default drive) ---------------- */
    SysMonitor_Init();
    Debug_Print("[SYS] Init: SysMonitor OK\r\n");

    /* ---- 9. USB PD manager ------------------------------------------------- */
    /* Note: UsbPdManager_Init() holds I2C traffic; call after PowerManager
     * so that CYPD6129 supply rails are not assumed to be up here.
     * If the CYPD6129 devices are powered by always-on rails, move this call
     * after the power manager reaches PM_STATE_ON. */
    UsbPdManager_Init();
    Debug_Print("[SYS] Init: UsbPdManager OK\r\n");

    Debug_Print("[SYS] Entering main loop\r\n");

    /* ---- Main loop --------------------------------------------------------- */
    for (;;)
    {
        PowerManager_Run();
        SysMonitor_Run();
        UsbPdManager_Run();

        /* Future: service IPC calls from CPU1 / CPU2 here */
    }

    /* Unreachable */
    return 0;
}
