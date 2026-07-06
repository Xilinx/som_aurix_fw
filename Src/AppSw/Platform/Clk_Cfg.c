/**
 * @file    Clk_Cfg.c
 * @brief   SCU / PLL clock initialisation for TC387 at 300 MHz.
 *
 * Tasking iLLD watchdog API differences vs HIGHTEC iLLD:
 *   IfxScuWdt_getCpuWatchdogPassword()      — no argument
 *   IfxScuWdt_disableCpuWatchdog(password)  — password only
 *   IfxScuWdt_enableCpuWatchdog(password)   — password only
 */

#include "Clk_Cfg.h"
#include "IfxScuCcu.h"
#include "IfxScuWdt.h"

void Clk_Init(void)
{
    /* Declare all locals at top of block (C89 requirement). */
    uint16           endinitPw;
    uint16           safetyPw;
    IfxScuCcu_Config ccuCfg;

    /* Disable watchdogs before reconfiguring clocks. */
    endinitPw = IfxScuWdt_getCpuWatchdogPassword();
    safetyPw  = IfxScuWdt_getSafetyWatchdogPassword();

    IfxScuWdt_disableCpuWatchdog(endinitPw);
    IfxScuWdt_disableSafetyWatchdog(safetyPw);

    /* Configure PLL to IFX_CFG_SCU_PLL_FREQUENCY (300 MHz).
     * Reads IFX_CFG_SCU_XTAL_FREQUENCY from Ifx_Cfg.h for divider calc. */
    IfxScuCcu_initConfig(&ccuCfg);
    IfxScuCcu_init(&ccuCfg);

    /* Re-enable watchdogs. */
    IfxScuWdt_enableCpuWatchdog(endinitPw);
    IfxScuWdt_enableSafetyWatchdog(safetyPw);
}
