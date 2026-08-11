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
#include "I2c_Master.h"


#define SYSMON_POLL_INTERVAL_MS     5u   /* main-loop poll rate for PROCHOT# */
#define SYSMON_LOG_INTERVAL_MS      1000u /* re-log PROCHOT assertion once/sec */

#define SYSMON_THERMAL_POLL_MS      100u

/* Temperature thresholds in degrees C — update from AMD thermal spec */
#define SYSMON_WARNING_TEMP_C       85      /* assert PROCHOT above this     */
#define SYSMON_WARNING_HYST_C       75      /* release PROCHOT below this    */
#define SYSMON_SHUTDOWN_TEMP_C      105
#define SYSMON_TEMP_INVALID         (-128)

#define SBTSI_I2C_ADDR_7BIT     0x4Cu
#define SBTSI_REG_CPU_TEMP_INT  0x01u
#define SBTSI_REG_CPU_TEMP_DEC  0x10u


#if (SYSMON_CARRIER_HOT_ENABLE == 1u)
static boolean s_carrierHotState   = FALSE;
static uint32  s_carrierClearMs    = 0u;
#endif

static SysMonitor_ShutdownCb_t s_shutdownCb = NULL_PTR;

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

static sint16 prv_ReadApuTempC(void)
{
    uint8 tempInt;
    uint8 tempDec;
    I2c_Status_t st;

    tempInt = 0u;
    tempDec = 0u;

    /* Read integer first — latches decimal (atomic read, ReadOrder=0) */
    st = I2cMaster_ApmlReadByte(SBTSI_I2C_ADDR_7BIT,
                                SBTSI_REG_CPU_TEMP_INT, &tempInt);
    if (st != I2C_OK)
    {
        return SYSMON_TEMP_INVALID;
    }

    /* Read latched decimal */
    st = I2cMaster_ApmlReadByte(SBTSI_I2C_ADDR_7BIT,
                                SBTSI_REG_CPU_TEMP_DEC, &tempDec);
    if (st != I2C_OK)
    {
        return SYSMON_TEMP_INVALID;
    }

    if ((tempDec >> 5u) >= 4u)
    {
        return (sint16)tempInt + 1;
    }

    return (sint16)tempInt;
}

/* ---- Private state ------------------------------------------------------- */

static boolean s_prochotActive = FALSE;   /* last observed PROCHOT state */
static boolean s_thermalThrottle = FALSE;

/* ---- Public API ---------------------------------------------------------- */

void SysMonitor_RegisterShutdownCb(SysMonitor_ShutdownCb_t cb)
{
    s_shutdownCb = cb;
}

void SysMonitor_Init(void)
{
    /* APU_PROCHOT_L (P11.9): open-drain output, default HIGH (released).
     * The pin direction is open-drain as configured in Port_Init.c.
     * Driving HIGH releases the open-drain line; APU can still pull it LOW. */
    prv_SetPin(&PIN_APU_PROCHOT_L, FALSE);

    /* PROCHOT# (P2.10): COM-HPC carrier output, active low.
     * Default HIGH = deasserted (no thermal event). */
    prv_SetPin(&PIN_PROCHOT_L, TRUE);

    /* CATERR# (P2.11): COM-HPC carrier output, active low.
     * Default HIGH = deasserted (no catastrophic error). */
    prv_SetPin(&PIN_CATERR_L, TRUE);

    s_prochotActive = FALSE;

    Debug_Print("[SYS] SysMonitor: APU_PROCHOT_L=H, PROCHOT#=H, CATERR#=H\r\n");

#if (SYSMON_CARRIER_HOT_ENABLE == 1u)
    s_carrierHotState = FALSE;
    s_carrierClearMs  = 0u;
#endif
}

void SysMonitor_Run(void)
{
    static uint32 s_lastPoll    = 0u;
    static uint32 s_lastLog     = 0u;
    static uint32 s_lastThermal = 0u;

    if (!Stm_IsElapsedMs(&s_lastPoll, SYSMON_POLL_INTERVAL_MS))
    {
        return;
    }

    /* ---- APML Thermal Polling -------------------------------------------
     * Read APU die temperature via I2C1 (SIC/SID, P11.13/P11.14).
     * Compare against warning (PROCHOT) and error (shutdown) thresholds.
     *
     * APML register map is pending AMD documentation for Glacier Peak.
     * Once available, replace prv_ReadApuTempC() with the actual I2C
     * transaction targeting the correct APML register address.
     * ------------------------------------------------------------------ */
    if (Stm_IsElapsedMs(&s_lastThermal, SYSMON_THERMAL_POLL_MS))
    {
        sint16 tempC = prv_ReadApuTempC();

        if (tempC != SYSMON_TEMP_INVALID)
        {
            if (tempC >= SYSMON_SHUTDOWN_TEMP_C)
            {
                Debug_Printf("[SYS] THERMAL SHUTDOWN: APU die %dC >= %dC\r\n",
                             (int)tempC, (int)SYSMON_SHUTDOWN_TEMP_C);
                SysMonitor_AssertApuProchot();
                prv_SetPin(&PIN_PROCHOT_L, FALSE);
                if (s_shutdownCb != NULL_PTR)
                {
                    s_shutdownCb();
                }
            }
            else if (tempC >= SYSMON_WARNING_TEMP_C)
            {
                /* Assert PROCHOT to throttle APU */
                if (!s_thermalThrottle)
                {
                    s_thermalThrottle = TRUE;
                    Debug_Printf("[SYS] THERMAL WARNING: APU die %dC >= %dC, "
                                 "asserting PROCHOT\r\n",
                                 (int)tempC, (int)SYSMON_WARNING_TEMP_C);
                }
                SysMonitor_AssertApuProchot();
            }
            else if (s_thermalThrottle &&
                    tempC < SYSMON_WARNING_HYST_C)
            {
                s_thermalThrottle = FALSE;
                Debug_Printf("[SYS] THERMAL CLEAR: APU die %dC, releasing PROCHOT\r\n",
                             (int)tempC);
#if (SYSMON_CARRIER_HOT_ENABLE == 1u)
                if (!s_carrierHotState)
#endif
                {
                    SysMonitor_DeassertApuProchot();
                }
            }
        }
    }
        /* ---- APU-side PROCHOT detection via SB-TSI status register -----------
        * P11.9 drives the gate of Q53 (BSS138), so reading the GPIO only
        * returns what the Aurix wrote.  Poll SB-TSI status register 0x02
        * bit[4] over the existing APML I2C bus to detect APU-initiated
        * PROCHOT instead.
        * ------------------------------------------------------------------ */
        uint8 sbtsiStatus = 0u;
        I2c_Status_t st;
        boolean apuProchot = FALSE;

        st = I2cMaster_ApmlReadByte(SBTSI_I2C_ADDR_7BIT, 0x02u, &sbtsiStatus);
        if (st == I2C_OK)
        {
            apuProchot = (boolean)((sbtsiStatus & 0x10u) != 0u);
        }

        if (apuProchot)
        {
            prv_SetPin(&PIN_PROCHOT_L, FALSE);   /* mirror to carrier */

            if (!s_prochotActive)
            {
                s_prochotActive = TRUE;
                s_lastLog = 0u;
            }

            if (Stm_IsElapsedMs(&s_lastLog, SYSMON_LOG_INTERVAL_MS))
            {
                Debug_Print("[SYS] PROCHOT# asserted — APU SB-TSI status\r\n");
            }
        }
        else
        {
            /* Only release carrier PROCHOT if we're not throttling from
             * the Aurix side (thermal or CARRIER_HOT) */
            if (s_prochotActive && !s_thermalThrottle
#if (SYSMON_CARRIER_HOT_ENABLE == 1u)
                && !s_carrierHotState
#endif
            )
            {
                prv_SetPin(&PIN_PROCHOT_L, TRUE);
                s_prochotActive = FALSE;
                Debug_Print("[SYS] PROCHOT# deasserted — APU SB-TSI clear\r\n");
            }
        }

        /* ---- CARRIER_HOT# monitoring ---------------------------------------- */
#if (SYSMON_CARRIER_HOT_ENABLE == 1u)
    {
        boolean carrierHot = !prv_ReadPin(&PIN_CARRIER_HOT);

        if (carrierHot)
        {
            if (!s_carrierHotState)
            {
                s_carrierHotState = TRUE;
                SysMonitor_AssertApuProchot();
                prv_SetPin(&PIN_PROCHOT_L, FALSE);
                Debug_Print("[SYS] CARRIER_HOT# asserted — PROCHOT asserted\r\n");
            }
            s_carrierClearMs = 0u;
        }
        else if (s_carrierHotState)
        {
            if (s_carrierClearMs == 0u)
            {
                s_carrierClearMs = Stm_GetTimeMs();
            }
            else if (Stm_GetTimeMs() - s_carrierClearMs >= SYSMON_CARRIER_DWELL_MS)
            {
                s_carrierHotState = FALSE;
                Debug_Print("[SYS] CARRIER_HOT# dwell complete\r\n");

                if (!s_thermalThrottle)
                {
                    SysMonitor_DeassertApuProchot();
                    Debug_Print("[SYS] PROCHOT released (carrier + thermal clear)\r\n");
                }
                else
                {
                    Debug_Print("[SYS] PROCHOT held — thermal throttle still active\r\n");
                }
            }
        }
    }
#else
    /* CARRIER_HOT# disabled — debug-only edge logging */
    {
        static boolean s_lastCarrierHot = FALSE;
        boolean hot = !prv_ReadPin(&PIN_CARRIER_HOT);

        if (hot && !s_lastCarrierHot)
        {
            Debug_Print("[SYS] CARRIER_HOT# asserted (debug only — not acting)\r\n");
        }
        else if (!hot && s_lastCarrierHot)
        {
            Debug_Print("[SYS] CARRIER_HOT# deasserted (debug only)\r\n");
        }
        s_lastCarrierHot = hot;
    }
#endif
}

void SysMonitor_AssertApuProchot(void)
{
    /* Drive APU_PROCHOT_L LOW via the open-drain output.
     * This throttles the APU from the TC387 side (e.g. platform-level
     * power limit enforcement via future APML control). */
    prv_SetPin(&PIN_APU_PROCHOT_L, TRUE);
    Debug_Print("[SYS] APU_PROCHOT_L asserted LOW by TC387\r\n");
}

void SysMonitor_DeassertApuProchot(void)
{
    /* Release the open-drain drive — APU_PROCHOT_L floats HIGH via pull-up. */
    prv_SetPin(&PIN_APU_PROCHOT_L, FALSE);
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

boolean SysMonitor_IsThrottling(void)
{
    return s_thermalThrottle
#if (SYSMON_CARRIER_HOT_ENABLE == 1u)
        || s_carrierHotState
#endif
    ;
}
