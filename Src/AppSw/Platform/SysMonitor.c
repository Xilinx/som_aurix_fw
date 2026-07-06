/**
 * @file    SysMonitor.c
 * @brief   Platform signal monitoring — PROCHOT / CATERR default drive logic.
 *
 * APU_PROCHOT_L wiring:
 *   The TC387 pin is configured as open-drain output (see Port_Init.c).
 *   This allows the APU to independently pull the shared line LOW without
 *   bus contention.  Either side asserting LOW will be detected by reading
 *   the pad state with IfxPort_getPinState(), which reflects the actual
 *   voltage present on the pad regardless of the output latch.
 *
 * PROCHOT# propagation:
 *   Any LOW on APU_PROCHOT_L (from the APU or asserted by TC387 via
 *   SysMonitor_AssertApuProchot) is forwarded to the COM-HPC carrier
 *   PROCHOT# output so the carrier is aware of a thermal event.
 */

#include "SysMonitor.h"
#include "Platform_PinCfg.h"
#include "AppPin.h"
#include "Stm_Timer.h"
#include "Uart_Debug.h"
#include "IfxPort.h"

#define SYSMON_POLL_INTERVAL_MS     5u   /* main-loop poll rate for PROCHOT# */
#define SYSMON_LOG_INTERVAL_MS      1000u /* re-log PROCHOT assertion once/sec */

/* ---- Private helpers ----------------------------------------------------- */

static void prv_SetPin(const AppPin_t *pin, boolean high)
{
    if (high)
        IfxPort_setPinHigh(AppPin_GetPort(pin->portIdx), pin->pinIdx);
    else
        IfxPort_setPinLow(AppPin_GetPort(pin->portIdx),  pin->pinIdx);
}

static boolean prv_ReadPin(const AppPin_t *pin)
{
    return (boolean)IfxPort_getPinState(AppPin_GetPort(pin->portIdx), pin->pinIdx);
}

/* ---- Private state ------------------------------------------------------- */

static boolean s_prochotActive = FALSE;   /* last observed PROCHOT state */

/* ---- Public API ---------------------------------------------------------- */

void SysMonitor_Init(void)
{
    /* APU_PROCHOT_L (P11.9): open-drain output, default HIGH (released).
     * The pin direction is open-drain as configured in Port_Init.c.
     * Driving HIGH releases the open-drain line; APU can still pull it LOW. */
    prv_SetPin(&PIN_APU_PROCHOT_L, TRUE);

    /* PROCHOT# (P2.10): COM-HPC carrier output, active low.
     * Default HIGH = deasserted (no thermal event). */
    prv_SetPin(&PIN_PROCHOT_L, TRUE);

    /* CATERR# (P2.11): COM-HPC carrier output, active low.
     * Default HIGH = deasserted (no catastrophic error). */
    prv_SetPin(&PIN_CATERR_L, TRUE);

    s_prochotActive = FALSE;

    Debug_Print("[SYS] SysMonitor: APU_PROCHOT_L=H, PROCHOT#=H, CATERR#=H\r\n");
}

void SysMonitor_Run(void)
{
    boolean apuProchotLow;
    static uint32 s_lastPoll  = 0u;
    static uint32 s_lastLog   = 0u;

    if (!Stm_IsElapsedMs(&s_lastPoll, SYSMON_POLL_INTERVAL_MS))
    {
        return;
    }

    /* Read the actual pad state of APU_PROCHOT_L.
     * Returns FALSE (LOW) if the APU or TC387 is asserting the open-drain line. */
    apuProchotLow = !prv_ReadPin(&PIN_APU_PROCHOT_L);

    if (apuProchotLow)
    {
        /* Assert carrier PROCHOT# (drive LOW) to inform the carrier board. */
        prv_SetPin(&PIN_PROCHOT_L, FALSE);

        if (!s_prochotActive)
        {
            s_prochotActive = TRUE;
            s_lastLog = 0u;   /* force immediate log */
        }

        /* Periodic log while PROCHOT is active. */
        if (Stm_IsElapsedMs(&s_lastLog, SYSMON_LOG_INTERVAL_MS))
        {
            Debug_Print("[SYS] PROCHOT# asserted — APU_PROCHOT_L LOW\r\n");
        }
    }
    else
    {
        /* Release carrier PROCHOT# (drive HIGH). */
        prv_SetPin(&PIN_PROCHOT_L, TRUE);

        if (s_prochotActive)
        {
            s_prochotActive = FALSE;
            Debug_Print("[SYS] PROCHOT# deasserted — APU_PROCHOT_L returned HIGH\r\n");
        }
    }
}

void SysMonitor_AssertApuProchot(void)
{
    /* Drive APU_PROCHOT_L LOW via the open-drain output.
     * This throttles the APU from the TC387 side (e.g. platform-level
     * power limit enforcement via future APML control). */
    prv_SetPin(&PIN_APU_PROCHOT_L, FALSE);
    Debug_Print("[SYS] APU_PROCHOT_L asserted LOW by TC387\r\n");
}

void SysMonitor_DeassertApuProchot(void)
{
    /* Release the open-drain drive — APU_PROCHOT_L floats HIGH via pull-up. */
    prv_SetPin(&PIN_APU_PROCHOT_L, TRUE);
    Debug_Print("[SYS] APU_PROCHOT_L released HIGH by TC387\r\n");
}

void SysMonitor_AssertCaterr(void)
{
    prv_SetPin(&PIN_CATERR_L, FALSE);
    Debug_Print("[SYS] CATERR# asserted LOW\r\n");
}

void SysMonitor_DeassertCaterr(void)
{
    prv_SetPin(&PIN_CATERR_L, TRUE);
    Debug_Print("[SYS] CATERR# deasserted HIGH\r\n");
}
