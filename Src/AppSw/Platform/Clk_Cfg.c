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
    IfxScuCcu_Config ccuCfg;
    IfxScuCcu_initConfig(&ccuCfg);
    IfxScuCcu_init(&ccuCfg);
}
