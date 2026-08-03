/**
 * @file    ComHpcWdt.c
 * @brief   COM-HPC host watchdog — Mode 1 implementation.
 */

#include "ComHpcWdt.h"
#include "Platform_PinCfg.h"
#include "AppPin.h"
#include "Stm_Timer.h"
#include "Uart_Debug.h"
#include "IfxPort.h"

/* ---- State -------------------------------------------------------------- */

typedef enum
{
    WDT_STATE_IDLE,         /* Init called but not enabled */
    WDT_STATE_ENABLE_DELAY, /* Counting down the enable delay */
    WDT_STATE_ACTIVE,       /* Monitoring for strobes */
    WDT_STATE_FIRED         /* Timeout — WD_OUT asserted, PLTRST# low */
} WdtState_t;

static WdtState_t       g_state             = WDT_STATE_IDLE;
static uint32           s_enableDelayEndMs  = 0u;
static uint32           s_timeoutMs         = COMHPC_WDT_DEFAULT_TIMEOUT_MS;
static volatile uint32  s_lastStrobeMs      = 0u;
static volatile boolean s_strobeReceived    = FALSE;

/* ---- GPIO helpers ------------------------------------------------------- */

static void prv_AssertWdOut(void)
{
    IfxPort_setPinHigh(AppPin_GetPort(PIN_WD_OUT.portIdx),
                       PIN_WD_OUT.pinIdx);
}

static void prv_DeassertWdOut(void)
{
    IfxPort_setPinLow(AppPin_GetPort(PIN_WD_OUT.portIdx),
                      PIN_WD_OUT.pinIdx);
}

static void prv_AssertPltrst(void)
{
    IfxPort_setPinLow(AppPin_GetPort(PIN_PLTRST_L.portIdx),
                      PIN_PLTRST_L.pinIdx);
}

static void prv_DeassertPltrst(void)
{
    IfxPort_setPinHigh(AppPin_GetPort(PIN_PLTRST_L.portIdx),
                       PIN_PLTRST_L.pinIdx);
}

/* ---- Public API --------------------------------------------------------- */

void ComHpcWdt_Init(void)
{
    g_state          = WDT_STATE_IDLE;
    s_strobeReceived = FALSE;
    prv_DeassertWdOut();
    Debug_Print("[WDT] Init: host watchdog idle.\r\n");
}

void ComHpcWdt_Enable(uint16 enableDelayS, uint32 timeoutMs)
{
    uint32 delayS;

    /* Clamp enable delay to spec range */
    delayS = (uint32)enableDelayS;
    if (delayS < COMHPC_WDT_ENABLE_DELAY_MIN_S)
    {
        delayS = COMHPC_WDT_ENABLE_DELAY_MIN_S;
    }
    if (delayS > COMHPC_WDT_ENABLE_DELAY_MAX_S)
    {
        delayS = COMHPC_WDT_ENABLE_DELAY_MAX_S;
    }

    s_timeoutMs        = timeoutMs;
    s_enableDelayEndMs = Stm_GetTimeMs() + (delayS * 1000u);
    s_strobeReceived   = FALSE;
    g_state            = WDT_STATE_ENABLE_DELAY;

    Debug_Printf("[WDT] Enabled: %us delay, %ums timeout.\r\n",
                 (unsigned)delayS, (unsigned)timeoutMs);
}

void ComHpcWdt_Disable(void)
{
    if (g_state == WDT_STATE_FIRED)
    {
        prv_DeassertPltrst();
    }
    prv_DeassertWdOut();
    g_state = WDT_STATE_IDLE;
    Debug_Print("[WDT] Disabled.\r\n");
}

void ComHpcWdt_OnStrobeIsr(void)
{
    /* ISR context — just record the timestamp.
     * Stm_GetTimeMs() is safe to call from ISR (reads STM counter). */
    s_lastStrobeMs   = Stm_GetTimeMs();
    s_strobeReceived = TRUE;
}

void ComHpcWdt_Run(void)
{
    uint32 nowMs;

    switch (g_state)
    {
        case WDT_STATE_IDLE:
            /* Nothing to do */
            break;

        case WDT_STATE_ENABLE_DELAY:
            nowMs = Stm_GetTimeMs();
            if (nowMs >= s_enableDelayEndMs)
            {
                /* Enable delay expired — begin monitoring.
                 * Seed the last strobe time so the first timeout
                 * window starts from now, not from zero. */
                s_lastStrobeMs   = nowMs;
                s_strobeReceived = FALSE;
                g_state          = WDT_STATE_ACTIVE;
                Debug_Print("[WDT] Enable delay expired. Monitoring active.\r\n");
            }
            break;

        case WDT_STATE_ACTIVE:
            nowMs = Stm_GetTimeMs();

            if (s_strobeReceived)
            {
                /* Strobe arrived — reset is handled by the ISR updating
                 * s_lastStrobeMs.  Clear the flag. */
                s_strobeReceived = FALSE;
            }

            if ((nowMs - s_lastStrobeMs) >= s_timeoutMs)
            {
                /* Timeout — host stopped strobing.
                 * Mode 1: assert WD_OUT, drive PLTRST# low. */
                Debug_Printf("[WDT] TIMEOUT: no strobe for %ums. "
                             "Asserting WD_OUT + PLTRST#.\r\n",
                             (unsigned)s_timeoutMs);
                prv_AssertWdOut();
                prv_AssertPltrst();
                g_state = WDT_STATE_FIRED;
            }
            break;

        case WDT_STATE_FIRED:
            /* Stay in fired state until platform reset or explicit disable.
             * WD_OUT remains HIGH, PLTRST# remains LOW.
             * This is COM-HPC Mode 1 behavior. */
            break;

        default:
            g_state = WDT_STATE_IDLE;
            break;
    }
}

boolean ComHpcWdt_HasFired(void)
{
    return (boolean)(g_state == WDT_STATE_FIRED);
}