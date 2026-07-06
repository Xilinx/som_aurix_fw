/**
 * @file    CpuIdle.c
 * @brief   Idle entry points for CPU1, CPU2, CPU3.
 *
 * The TC387 iLLD startup code (Ifx_Ssw_Tc1/2/3) requires a core entry point
 * symbol for every TriCore core present in the device. Cores not used by this
 * application are parked here in an infinite idle loop.
 *
 * Future functionality targeting CPU1 or CPU2 should replace the relevant
 * stub with a real implementation in its own translation unit.
 */

#include "Ifx_Types.h"
#include "IfxCpu.h"
#include "IfxScuWdt.h"

void core1_main(void)
{
    IfxScuWdt_disableCpuWatchdog(IfxScuWdt_getCpuWatchdogPassword());
    for (;;)
    {
        __nop();    /* CPU1 idle — reserved for future use */
    }
}

void core2_main(void)
{
    IfxScuWdt_disableCpuWatchdog(IfxScuWdt_getCpuWatchdogPassword());
    for (;;)
    {
        __nop();    /* CPU2 idle — reserved for future use */
    }
}

void core3_main(void)
{
    IfxScuWdt_disableCpuWatchdog(IfxScuWdt_getCpuWatchdogPassword());
    for (;;)
    {
        __nop();    /* CPU3 idle — reserved for future use */
    }
}
