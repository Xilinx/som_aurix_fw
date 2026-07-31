/**
 * @file    PowerManager.c
 * @brief   COM-HPC / Strix Halo power sequencing state machine.
 *
 * Rail groups are enabled in order: ALW -> S5 -> S3 -> S0.
 * Power-down is performed in reverse order.
 * Any PG loss while running transitions directly to PM_STATE_FAULT which
 * disables all rails and deasserts PWRGD.
 */

#include "PowerManager.h"
#include "PowerManager_Cfg.h"
#include "AppPin.h"
#include "PwrGood_Mon.h"
#include "Platform_PinCfg.h"
#include "Platform_Cfg.h"
#include "Stm_Timer.h"
#include "Uart_Debug.h"
#include "IfxPort.h"
#include "ComHpcWdt.h"

/* ---- Private state ------------------------------------------------------- */

static PM_State_t s_state              = PM_STATE_OFF;
static boolean    s_powerOnReq         = FALSE;
static boolean    s_powerOffReq        = FALSE;
static boolean    s_shutdownToOff      = FALSE;
static volatile boolean s_thermtripIsrFlag = FALSE;
static uint8      s_thermtripDebounce  = 0u;  /* consecutive LOW-read counter */
static uint8           s_retryCount    = 0u;
static PM_ResetCause_t s_resetCause    = PM_RESET_CAUSE_NONE;
static PM_ResetCause_t s_pendingCause  = PM_RESET_CAUSE_NONE;

static void prv_OnPgFault(const PwrRail_Cfg_t *rail, uint8 railIdx);

/* ---- FuSa status encoding ------------------------------------------------
 *  Bit1 = FUSA_STATUS1 (P2.9),  Bit0 = FUSA_STATUS0 (P2.8)
 *
 *  00  FUSA_PWR_OFF   — MAIN_12V_EFUSE_EN not asserted
 *  01  FUSA_PWR_GOOD  — system fully powered, no fault
 *  10  FUSA_FAULT     — PG timeout or PG loss detected
 *  11  FUSA_RESET     — SoC platform is in reset (all ramp / power-down states)
 * -------------------------------------------------------------------------- */
typedef enum
{
    FUSA_PWR_OFF  = 0u,
    FUSA_PWR_GOOD = 1u,
    FUSA_FAULT    = 2u,
    FUSA_RESET    = 3u
} FusaStatus_t;

static void prv_SetFusaStatus(FusaStatus_t status)
{
    boolean bit0 = (boolean)(((uint8)status & 0x01u) != 0u);
    boolean bit1 = (boolean)(((uint8)status & 0x02u) != 0u);

    if (bit0)
        IfxPort_setPinHigh(AppPin_GetPort(PIN_FUSA_STATUS0.portIdx), PIN_FUSA_STATUS0.pinIdx);
    else
        IfxPort_setPinLow(AppPin_GetPort(PIN_FUSA_STATUS0.portIdx),  PIN_FUSA_STATUS0.pinIdx);

    if (bit1)
        IfxPort_setPinHigh(AppPin_GetPort(PIN_FUSA_STATUS1.portIdx), PIN_FUSA_STATUS1.pinIdx);
    else
        IfxPort_setPinLow(AppPin_GetPort(PIN_FUSA_STATUS1.portIdx),  PIN_FUSA_STATUS1.pinIdx);
}

static void prv_UpdateFusaStatus(PM_State_t state)
{
    switch (state)
    {
        case PM_STATE_OFF:
            /* 00 — MAIN_12V_EFUSE_EN not enabled */
            prv_SetFusaStatus(FUSA_PWR_OFF);
            break;
        case PM_STATE_POWER_UP:              /* <-- ADD */
            prv_SetFusaStatus(FUSA_RESET);   /* <-- ADD */
            break;  
        case PM_STATE_ON:
            /* 01 — all rails stable, system in good power state */
            prv_SetFusaStatus(FUSA_PWR_GOOD);
            break;

        case PM_STATE_FAULT:
            /* 10 — PG timeout or PG loss; supply not in expected state */
            prv_SetFusaStatus(FUSA_FAULT);
            break;

        case PM_STATE_S5:
            /* 11 — THERMTRIP suspend: Group B + EFUSE on, SoC in reset */
            prv_SetFusaStatus(FUSA_RESET);
            break;

        default:
            /* 11 — all ramp (RAMP_ALW/S5/S3/S0) and power-down (DN_*)
             *      states: COLD_RST is asserted, SoC platform in reset */
            prv_SetFusaStatus(FUSA_RESET);
            break;
    }
}

void PowerManager_OnThermtripIsr(void)
{
    s_thermtripIsrFlag = TRUE;  /* ISR-safe — just set the flag */
}

// Add this public function for voltage faults:
void PowerManager_OnVoltageFault(const VoltMon_ChCfg_t *ch,
                                  uint16 measuredMv,
                                  VoltMon_Severity_t severity)
{
    /* For now, treat any FAULT-level voltage event as a PG loss.
     * This runs from main-loop context (VoltMon_Scan), not ISR. */
    if (severity >= VOLTMON_FAULT)
    {
        Debug_Printf("[PM] Voltage fault on %s: %umV\r\n",
                     ch->name, (unsigned)measuredMv);
        s_pendingCause = PM_RESET_CAUSE_VOLTAGE;
        prv_OnPgFault(NULL_PTR, 0u);
    }
}

/* ---- Private helpers ----------------------------------------------------- */

static void prv_EnableRail(const PwrRail_Cfg_t *rail)
{
    IfxPort_setPinHigh(AppPin_GetPort(rail->enablePin.portIdx), rail->enablePin.pinIdx);
}

static void prv_DisableRail(const PwrRail_Cfg_t *rail)
{
    IfxPort_setPinLow(AppPin_GetPort(rail->enablePin.portIdx), rail->enablePin.pinIdx);
}

static void prv_DisableAllRails(void)
{
    /* Declare loop variable at top of block (C89). */
    sint8 i;

    /* Disable group enables in reverse order: D -> C -> B -> EFUSE. */
    for (i = (sint8)PM_RAIL_GRP_D_COUNT - 1; i >= 0; i--)
    {
        if (PM_RAILS_GRP_D[i].assertEnable)
        {
            prv_DisableRail(&PM_RAILS_GRP_D[i]);
            Stm_DelayMs(PM_INTER_RAIL_DELAY_MS);
        }
    }
    for (i = (sint8)PM_RAIL_GRP_C_COUNT - 1; i >= 0; i--)
    {
        if (PM_RAILS_GRP_C[i].assertEnable)
        {
            prv_DisableRail(&PM_RAILS_GRP_C[i]);
            Stm_DelayMs(PM_INTER_RAIL_DELAY_MS);
        }
    }
    for (i = (sint8)PM_RAIL_GRP_B_COUNT - 1; i >= 0; i--)
    {
        if (PM_RAILS_GRP_B[i].assertEnable)
        {
            prv_DisableRail(&PM_RAILS_GRP_B[i]);
            Stm_DelayMs(PM_INTER_RAIL_DELAY_MS);
        }
    }
    for (i = (sint8)PM_RAIL_EFUSE_COUNT - 1; i >= 0; i--)
    {
        if (PM_RAILS_EFUSE[i].assertEnable)
        {
            prv_DisableRail(&PM_RAILS_EFUSE[i]);
            Stm_DelayMs(PM_INTER_RAIL_DELAY_MS);
        }
    }
}

static void prv_DeassertPwrgd(void)
{
    IfxPort_setPinLow(AppPin_GetPort(PIN_COMHPC_PWRGD.portIdx), PIN_COMHPC_PWRGD.pinIdx);
}

static void prv_AssertPwrgd(void)
{
    Stm_DelayMs(PM_PWRGD_DEGLITCH_MS);
    IfxPort_setPinHigh(AppPin_GetPort(PIN_COMHPC_PWRGD.portIdx), PIN_COMHPC_PWRGD.pinIdx);
}

/* ---- UART MUX helpers ------------------------------------------------
 * PIN_UART_MUX_SEL (P14.6):
 *   HIGH (1) = AURIX owns ASCLIN0 on P14.0/P14.1
 *   LOW  (0) = x86 SoC owns the shared UART connector
 *
 * AURIX holds the UART from power-on until SYS_RESET_L is released,
 * ensuring all boot-time diagnostic output is visible.  The MUX hands
 * off to the SoC immediately before COLD_RST is deasserted and is
 * reclaimed immediately after COLD_RST is (re-)asserted.
 * --------------------------------------------------------------------- */
static void prv_UartClaimByAurix(void)
{
    IfxPort_setPinHigh(AppPin_GetPort(PIN_UART_MUX_SEL.portIdx),
                       PIN_UART_MUX_SEL.pinIdx);
}

static void prv_UartReleaseToSoc(void)
{
    /* Print last AURIX diagnostic before relinquishing the UART path. */
    Debug_Print("[PM] UART MUX -> x86 SoC (UART_MUX_SEL=0)\r\n");
    IfxPort_setPinLow(AppPin_GetPort(PIN_UART_MUX_SEL.portIdx),
                      PIN_UART_MUX_SEL.pinIdx);
}

static void prv_AssertApuReset(void)
{
    IfxPort_setPinLow(AppPin_GetPort(PIN_APU_RESET_OUT_L.portIdx),
                      PIN_APU_RESET_OUT_L.pinIdx);
    /* Reclaim UART immediately after asserting reset — SoC is now in
     * reset so the shared line is free for AURIX diagnostic use. */
    prv_UartClaimByAurix();
    Debug_Print("[PM] COLD_RST asserted. UART MUX -> AURIX (UART_MUX_SEL=1)\r\n");
}

static void prv_DeassertApuReset(void)
{
    /* Hand UART to x86 SoC before releasing reset so it owns the line
     * from its first boot cycle.  This is the last AURIX UART message. */
    prv_UartReleaseToSoc();
    IfxPort_setPinHigh(AppPin_GetPort(PIN_APU_RESET_OUT_L.portIdx),
                       PIN_APU_RESET_OUT_L.pinIdx);
}

/* ---- BIOS ROM validation stub -------------------------------------------
 * Reads BSEL[2:0] straps, takes control of SPI ROM MUX, validates ROM
 * contents via QSPI0, releases MUX.  Returns TRUE if ROM is valid.
 * AMD-defined validation method is TBD — currently a pass-through stub.
 * --------------------------------------------------------------------- */
static boolean prv_BiosRomValidate(void)
{
    uint8 bsel;

    bsel  = (IfxPort_getPinState(AppPin_GetPort(PIN_BSEL_2.portIdx),
                                 PIN_BSEL_2.pinIdx) != 0u) ? 4u : 0u;
    bsel |= (IfxPort_getPinState(AppPin_GetPort(PIN_BSEL_1.portIdx),
                                 PIN_BSEL_1.pinIdx) != 0u) ? 2u : 0u;
    bsel |= (IfxPort_getPinState(AppPin_GetPort(PIN_BSEL_0.portIdx),
                                 PIN_BSEL_0.pinIdx) != 0u) ? 1u : 0u;

    Debug_Printf("[PM] BSEL[2:0] = %u\r\n", (unsigned)bsel);

    /* Assert SPI MUX select — AURIX owns BIOS ROM flash. */
    IfxPort_setPinHigh(AppPin_GetPort(PIN_APU_ROM_SPI_SEL.portIdx),
                       PIN_APU_ROM_SPI_SEL.pinIdx);

    /* TODO: QSPI0 read of ROM contents + AMD-defined integrity check.
     *       Stub returns TRUE until validation method is defined. */

    /* Release SPI MUX — APU owns BIOS ROM flash. */
    IfxPort_setPinLow(AppPin_GetPort(PIN_APU_ROM_SPI_SEL.portIdx),
                      PIN_APU_ROM_SPI_SEL.pinIdx);

    return TRUE;
}

static void prv_SetState(PM_State_t newState)
{
    Debug_Printf("[PM] %d -> %d\r\n", (int)s_state, (int)newState);
    s_state = newState;
    prv_UpdateFusaStatus(newState);
}

static void prv_AssertRsmrst(void)
{
    IfxPort_setPinLow(AppPin_GetPort(PIN_MMC_RSMRST_L.portIdx),
                      PIN_MMC_RSMRST_L.pinIdx);
}

static void prv_DeassertRsmrst(void)
{
    IfxPort_setPinHigh(AppPin_GetPort(PIN_MMC_RSMRST_L.portIdx),
                       PIN_MMC_RSMRST_L.pinIdx);
}

/* Forward declaration — prv_DisableGroup is defined after prv_GoToS5. */
static void prv_DisableGroup(const PwrRail_Cfg_t *rails, uint8 count);

/**
 * @brief Transition to PM_STATE_S5 in response to THERMTRIP assertion.
 *
 * Keeps Group B (S5 rails) and the 12V EFUSE powered so the platform
 * can recover when the thermal condition clears.  Group C (memory) and
 * Group D (VDDCR) are powered down in reverse order.  Both APU resets
 * (COLD_RST and RSMRST_L) remain asserted until wake from S5.
 */
static void prv_GoToS5(void)
{
    Debug_Print("[PM] THERMTRIP asserted — suspending to S5 "
                "(Group B + EFUSE remain on)\r\n");

    /* Secure APU and deassert PWRGD before touching rails. */
    prv_AssertApuReset();
    prv_AssertRsmrst();
    prv_DeassertPwrgd();
    PwrGood_MonDisarm();
    VoltMon_Disable();

    /* Disable Group D (VDDCR core) then Group C (memory).
     * Group B (S5 rails) and EFUSE stay powered. */
    prv_DisableGroup(PM_RAILS_GRP_D, PM_RAIL_GRP_D_COUNT);
    prv_DisableGroup(PM_RAILS_GRP_C, PM_RAIL_GRP_C_COUNT);

    s_thermtripDebounce = 0u;
    prv_SetState(PM_STATE_S5);
}

/* PG fault callback — called by PwrGood_Mon on confirmed PG loss. */
static void prv_OnPgFault(const PwrRail_Cfg_t *rail, uint8 railIdx)
{
    (void)railIdx;

    /* Record what caused the fault */
    s_resetCause = s_pendingCause;
    if (s_resetCause == PM_RESET_CAUSE_NONE)
    {
        /* Default to PG loss if no specific cause was set */
        s_resetCause = PM_RESET_CAUSE_PG_LOSS;
    }

    if (rail != NULL_PTR)
    {
        Debug_Printf("[PM] FAULT: %s (cause=%u, retry=%u/%u)\r\n",
                     rail->name, (unsigned)s_resetCause,
                     (unsigned)s_retryCount, (unsigned)PM_MAX_RETRIES);
    }
    else
    {
        Debug_Printf("[PM] FAULT: cause=%u, retry=%u/%u\r\n",
                     (unsigned)s_resetCause,
                     (unsigned)s_retryCount, (unsigned)PM_MAX_RETRIES);
    }

    /* Secure the platform */
    prv_AssertApuReset();
    prv_AssertRsmrst();
    prv_DeassertPwrgd();
    PwrGood_MonDisarm();
    prv_DisableAllRails();

    if (s_retryCount < PM_MAX_RETRIES)
    {
        s_retryCount++;
        Debug_Printf("[PM] Retry %u/%u in %ums...\r\n",
                     (unsigned)s_retryCount, (unsigned)PM_MAX_RETRIES,
                     (unsigned)PM_RETRY_DELAY_MS);
        prv_SetState(PM_STATE_FAULT);
        /* Retry is handled in PM_STATE_FAULT case below */
    }
    else
    {
        Debug_Print("[PM] LATCH-OFF: max retries exceeded. "
                    "Power cycle required.\r\n");
        s_retryCount = 0u;
        prv_SetState(PM_STATE_FAULT);
    }

    /* Clear pending cause for next fault */
    s_pendingCause = PM_RESET_CAUSE_NONE;
}

/* Enable a rail group: assert group enable on first entry (assertEnable==TRUE),
 * then check each rail's individual PG. Returns FALSE on any PG timeout. */
static boolean prv_RampGroup(const PwrRail_Cfg_t *rails, uint8 count)
{
    /* Declare all locals at top of block (C89). */
    uint8 i;
    uint8 failIdx;

    for (i = 0u; i < count; i++)
    {
        if (rails[i].assertEnable)
        {
            Debug_Printf("[PM] EN: %s (group enable)\r\n", rails[i].name);
            prv_EnableRail(&rails[i]);
            Stm_DelayMs(PM_INTER_RAIL_DELAY_MS);
        }

        failIdx = 0u;
        if (!PwrGood_WaitAllPg(&rails[i], 1u,
                                rails[i].rampDelayMs,
                                rails[i].pgTimeoutMs,
                                &failIdx))
        {
            Debug_Printf("[PM] TIMEOUT: PG not asserted for %s\r\n", rails[i].name);
            return FALSE;
        }
    }
    return TRUE;
}

/* Disable a group by deasserting its group enable (first assertEnable entry). */
static void prv_DisableGroup(const PwrRail_Cfg_t *rails, uint8 count)
{
    sint8 i;
    for (i = (sint8)count - 1; i >= 0; i--)
    {
        if (rails[i].assertEnable)
        {
            Debug_Printf("[PM] DIS: %s (group enable)\r\n", rails[i].name);
            prv_DisableRail(&rails[i]);
            Stm_DelayMs(PM_INTER_RAIL_DELAY_MS);
        }
    }
}

/* SLP_S3 / SLP_S5 are active HIGH per GP_AURIX_Subsystem_PinDefn.xlsx. */
static boolean prv_SlpS3Active(void)
{
    return (boolean)IfxPort_getPinState(
        AppPin_GetPort(PIN_SLP_S3_ACTIVE.portIdx), PIN_SLP_S3_ACTIVE.pinIdx);
}

static boolean prv_SlpS5Active(void)
{
    return (boolean)IfxPort_getPinState(
        AppPin_GetPort(PIN_SLP_S5_ACTIVE.portIdx), PIN_SLP_S5_ACTIVE.pinIdx);
}

static boolean prv_PwrBtnPressed(void)
{
    return (IfxPort_getPinState(
        AppPin_GetPort(PIN_PWRBTN_L.portIdx), PIN_PWRBTN_L.pinIdx) == 0u);
}

static boolean prv_ThermTripActive(void)
{
    return (IfxPort_getPinState(
        AppPin_GetPort(PIN_THERMTRIP_L.portIdx), PIN_THERMTRIP_L.pinIdx) == 0u);
}

static boolean prv_VinPwrOk(void)
{
    return (boolean)IfxPort_getPinState(
        AppPin_GetPort(PIN_VIN_PWR_OK.portIdx), PIN_VIN_PWR_OK.pinIdx);
}

/* ---- Public API ---------------------------------------------------------- */

void PowerManager_Init(void)
{
    /* Populate rail tables before any GPIO access (Tasking E306 workaround). */
    PowerManager_CfgInit();

    prv_DeassertPwrgd();
    prv_AssertApuReset();
    prv_AssertRsmrst();     /* hold RSMRST_L until S5 rails stable + 10ms */
    PwrGood_MonDisarm();
    s_state             = PM_STATE_OFF;
    s_powerOnReq        = FALSE;
    s_powerOffReq       = FALSE;
    s_shutdownToOff     = FALSE;
    s_thermtripDebounce = 0u;
    s_retryCount        = 0u;
    s_resetCause        = PM_RESET_CAUSE_NONE;
    s_pendingCause      = PM_RESET_CAUSE_NONE;
    prv_SetFusaStatus(FUSA_PWR_OFF);   /* 00 — EFUSE not yet enabled */
    Debug_Print("[PM] Initialised. State: OFF\r\n");
}

PM_State_t PowerManager_GetState(void)
{
    return s_state;
}

void PowerManager_RequestPowerOn(void)
{
    s_powerOnReq = TRUE;
}

void PowerManager_RequestPowerOff(void)
{
    s_powerOffReq = TRUE;
}

void PowerManager_Run(void)
{
    if (s_thermtripIsrFlag)
    {
        s_thermtripIsrFlag = FALSE;
        if ((s_state != PM_STATE_OFF) &&
            (s_state != PM_STATE_S5)  &&
            (s_state != PM_STATE_FAULT))
        {
            Debug_Print("[PM] THERMTRIP# ISR triggered\r\n");
            s_resetCause = PM_RESET_CAUSE_THERMAL;
            prv_GoToS5();
            return;
        }
    }
    /* ------------------------------------------------------------------
     * THERMTRIP# monitoring — active low, debounced.
     * PM_PG_DEBOUNCE_POLLS consecutive LOW reads required before acting
     * to reject glitches (covers both falling-edge and static-low cases).
     * Ignored when already in OFF, S5, or FAULT states.
     * ------------------------------------------------------------------ */
    if (prv_ThermTripActive())
    {
        if ((s_state != PM_STATE_OFF)   &&
            (s_state != PM_STATE_S5)    &&
            (s_state != PM_STATE_FAULT))
        {
            s_thermtripDebounce++;
            if (s_thermtripDebounce >= PM_PG_DEBOUNCE_POLLS)
            {
                Debug_Printf("[PM] THERMTRIP# asserted (debounce %u polls)\r\n",
                             (unsigned)s_thermtripDebounce);
                prv_GoToS5();
                return;
            }
        }
    }
    else
    {
        s_thermtripDebounce = 0u;   /* clear debounce counter on deassert */
    }

    /* Run the PG monitor on every call (only active when armed). */
    PwrGood_MonRun();

    /* Check for PWR_BTN press (edge detection for ON request). */
    if (prv_PwrBtnPressed() && (s_state == PM_STATE_OFF))
    {
        s_powerOnReq = TRUE;
    }

    switch (s_state)
    {
        /* ------------------------------------------------------------------ */
        case PM_STATE_OFF:
            if (s_powerOnReq && prv_VinPwrOk())
            {
                s_powerOnReq = FALSE;
                VoltMon_Disable(); /* suppresses faults in sequencing */
                prv_SetState(PM_STATE_POWER_UP);
            }
            else if (s_powerOnReq && !prv_VinPwrOk())
            {
                Debug_Print("[PM] Power-on request blocked: VIN_PWR_OK not asserted\r\n");
                s_powerOnReq = FALSE;
            }
            break;
        case PM_STATE_POWER_UP:
            /* Initial power-up sequencing entry point.
            * Disable voltage monitoring for the duration of rail bring-up.
            * VoltMon will be re-enabled at the end of RAMP_S0 once all
            * rails are confirmed stable and PWRGD is asserted. */
            Debug_Print("[PM] Power-up sequence started. "
                        "Voltage monitoring suspended.\r\n");
            VoltMon_Disable();
            prv_SetState(PM_STATE_RAMP_VR3V3);
            break;
        case PM_STATE_RAMP_VR3V3:
            if (!prv_RampGroup(PM_RAILS_VR3V3, PM_RAIL_VR3V3_COUNT))
            {
                s_pendingCause = PM_RESET_CAUSE_PG_TIMEOUT;
                prv_OnPgFault(&PM_RAILS_VR3V3[0], 0u);
                break;
            }
            Debug_Print("[PM] VR_APU_3V3 stable (standby rail).\r\n");
            prv_SetState(PM_STATE_RAMP_ALW);
            break;
        /* ------------------------------------------------------------------ */
        /* Stage 0: 12V EFUSE */
        case PM_STATE_RAMP_ALW:
            if (!prv_RampGroup(PM_RAILS_EFUSE, PM_RAIL_EFUSE_COUNT))
            {
                s_pendingCause = PM_RESET_CAUSE_PG_TIMEOUT;
                prv_OnPgFault(&PM_RAILS_EFUSE[0], 0u);
                break;
            }
            prv_SetState(PM_STATE_RAMP_S5);
            break;

        /* ------------------------------------------------------------------ */
        /* Stage 1: Group B — S5 rails (MISC, 1V2, 1V8 + 3V3 pre-check) */
        case PM_STATE_RAMP_S5:
            if (!prv_RampGroup(PM_RAILS_GRP_B, PM_RAIL_GRP_B_COUNT))
            {
                s_pendingCause = PM_RESET_CAUSE_PG_TIMEOUT;
                prv_OnPgFault(&PM_RAILS_GRP_B[0], 0u);
                break;
            }
            /* AMD 58241 §16.1.5 Table 28 T1: S5 power rails stable ->
             * RSMRST_L rising, minimum 10ms.  Deassert RSMRST_L (drive HIGH)
             * after PM_RSMRST_DELAY_AFTER_S5_MS. */
            Stm_DelayMs(PM_RSMRST_DELAY_AFTER_S5_MS);
            IfxPort_setPinHigh(AppPin_GetPort(PIN_MMC_RSMRST_L.portIdx),
                               PIN_MMC_RSMRST_L.pinIdx);
            Debug_Print("[PM] RSMRST_L deasserted (S5 rails stable + 10ms).\r\n");
            prv_SetState(PM_STATE_RAMP_S3);
            break;

        /* ------------------------------------------------------------------ */
        /* Stage 2: Group C — memory rails */
        case PM_STATE_RAMP_S3:
            if (!prv_RampGroup(PM_RAILS_GRP_C, PM_RAIL_GRP_C_COUNT))
            {
                s_pendingCause = PM_RESET_CAUSE_PG_TIMEOUT;
                prv_OnPgFault(&PM_RAILS_GRP_C[0], 0u);
                break;
            }
            prv_SetState(PM_STATE_RAMP_S0);
            break;

        /* ------------------------------------------------------------------ */
        /* Stage 3: Group D — VDDCR core */
        case PM_STATE_RAMP_S0:
            if (!prv_RampGroup(PM_RAILS_GRP_D, PM_RAIL_GRP_D_COUNT))
            {
                s_pendingCause = PM_RESET_CAUSE_PG_TIMEOUT;
                prv_OnPgFault(&PM_RAILS_GRP_D[0], 0u);
                break;
            }

            /* AMD 58241 §16.1.1: all rails stable ≥1ms before PWR_GOOD.
             * PM_PWRGD_DEGLITCH_MS = 5ms satisfies this requirement. */
            prv_AssertPwrgd();

            /* AMD 58241 §16.1.5 Table 30 T7: RESET_L must remain asserted
             * a minimum of 28.5ms AFTER PWR_GOOD assertion.
             * Wait PM_RESET_HOLD_AFTER_PWRGD_MS (30ms) then release COLD_RST. */
            Stm_DelayMs(PM_RESET_HOLD_AFTER_PWRGD_MS);
            if (!prv_BiosRomValidate())
            {
                Debug_Print("[PM] BIOS ROM validation FAILED — blocking boot\r\n");
                s_pendingCause = PM_RESET_CAUSE_BIOS_FAIL;
                prv_OnPgFault(NULL_PTR, 0u);
                break;
            }
            prv_DeassertApuReset();

            /* Pulse APU_PWRBTN LOW to trigger SoC boot (>16ms per ACPI spec). */
            IfxPort_setPinLow(AppPin_GetPort(PIN_APU_PWRBTN.portIdx),
                            PIN_APU_PWRBTN.pinIdx);
            Stm_DelayMs(200u);
            IfxPort_setPinHigh(AppPin_GetPort(PIN_APU_PWRBTN.portIdx),
                            PIN_APU_PWRBTN.pinIdx);
            Debug_Print("[PM] APU_PWRBTN pulsed LOW 200ms.\r\n");

            /* Arm continuous PG monitoring on Group B rails (always-on set).
             * Expand to GRP_C / GRP_D by adding chained monitor calls or
             * a unified flat table when the monitor supports multiple segments. */
            PwrGood_MonArm(PM_RAILS_GRP_B, PM_RAIL_GRP_B_COUNT, prv_OnPgFault);
            ComHpcWdt_Enable(COMHPC_WDT_DEFAULT_ENABLE_DELAY_S,
                             COMHPC_WDT_DEFAULT_TIMEOUT_MS);
            s_retryCount = 0u;
            VoltMon_Enable(); 
            prv_SetState(PM_STATE_ON);
            Debug_Print("[PM] System ON. APU_PWR_GOOD asserted, COLD_RST released.\r\n");
            break;

        /* ------------------------------------------------------------------ */
        case PM_STATE_ON:
            /* Check APU-initiated sleep state transitions. */
            if (s_powerOffReq || prv_SlpS5Active())
            {
                s_powerOffReq = FALSE;
                s_shutdownToOff = TRUE; //full shutdown to off
                prv_AssertApuReset();
                prv_AssertRsmrst();
                prv_DeassertPwrgd();
                PwrGood_MonDisarm();
                VoltMon_Disable();
                ComHpcWdt_Disable();
                prv_SetState(PM_STATE_DN_S0_S3);
            }
            else if (prv_SlpS3Active())
            {
                s_shutdownToOff = FALSE;
                prv_AssertApuReset();
                prv_AssertRsmrst();
                prv_DeassertPwrgd();
                PwrGood_MonDisarm();
                VoltMon_Disable();
                ComHpcWdt_Disable();
                prv_SetState(PM_STATE_DN_S0_S3);
            }
            if (IfxPort_getPinState(AppPin_GetPort(PIN_CB_RSTBTN_L.portIdx),
                                     PIN_CB_RSTBTN_L.pinIdx) == 0u)
            {
                s_resetCause = PM_RESET_CAUSE_HOST_REQUEST;
                prv_SetState(PM_STATE_WARM_RESET);
            }
            break;

        /* ------------------------------------------------------------------ */
        case PM_STATE_S5:
            /*
             * Suspended at S5 due to THERMTRIP# assertion.
             * Group B (S5 rails) and EFUSE remain powered.
             * Group C and D are off.  Both APU resets are held asserted.
             * FUSA_STATUS = 11 (SoC in reset).
             *
             * Wake conditions (both must be true):
             *   1. THERMTRIP# is no longer asserted (thermal event cleared).
             *   2. A power-on event has been requested (PWR_BTN or API call).
             *
             * Wake sequence:
             *   - S5 rails already stable, so skip EFUSE and Group B stages.
             *   - Deassert RSMRST_L immediately (AMD T1 already satisfied
             *     by the time spent in S5 — far exceeds 10ms minimum).
             *   - Jump directly to PM_STATE_RAMP_S3 to re-enable Group C/D.
             */
            if (!prv_ThermTripActive())
            {
                if (s_powerOnReq || prv_PwrBtnPressed())
                {
                    s_powerOnReq = FALSE;
                    Debug_Print("[PM] Wake from S5: THERMTRIP# cleared, "
                                "re-enabling Group C/D.\r\n");
                    prv_DeassertRsmrst();
                    prv_SetState(PM_STATE_RAMP_S3);
                }
            }
            else
            {
                /* THERMTRIP still active — log periodically if needed. */
                s_thermtripDebounce = 0u;   /* keep counter reset while in S5 */
            }
            break;

        /* ------------------------------------------------------------------ */
        case PM_STATE_DN_S0_S3:
            /* Disable Group D (VDDCR core) */
            prv_DisableGroup(PM_RAILS_GRP_D, PM_RAIL_GRP_D_COUNT);
            prv_SetState(PM_STATE_DN_S3_S5);
            break;

        /* ------------------------------------------------------------------ */
        case PM_STATE_DN_S3_S5:
            if (!s_shutdownToOff && !prv_SlpS3Active())
            {
                /* Resume to S0 — re-enable memory + core rails. */
                prv_DeassertRsmrst();
                prv_SetState(PM_STATE_RAMP_S3);
            }
            else
            {
                /* Disable Group C (memory rails) */
                prv_DisableGroup(PM_RAILS_GRP_C, PM_RAIL_GRP_C_COUNT);
                prv_SetState(PM_STATE_DN_S5_OFF);
            }
            break;

        /* ------------------------------------------------------------------ */
        case PM_STATE_DN_S5_OFF:
            /* Disable Group B then EFUSE */
            prv_DisableGroup(PM_RAILS_GRP_B, PM_RAIL_GRP_B_COUNT);
            prv_DisableGroup(PM_RAILS_EFUSE, PM_RAIL_EFUSE_COUNT);
            prv_SetState(PM_STATE_OFF);
            Debug_Print("[PM] System OFF.\r\n");
            break;

        /* ------------------------------------------------------------------ */
        case PM_STATE_FAULT:
            /* Stay in fault until a manual reset. Future: add recovery logic. */
            if (s_retryCount > 0u && s_retryCount <= PM_MAX_RETRIES)
            {
                /* Retry: delay then attempt full re-sequence */
                Stm_DelayMs(PM_RETRY_DELAY_MS);
                Debug_Printf("[PM] Retrying power-on (attempt %u)...\r\n",
                             (unsigned)s_retryCount);
                prv_SetState(PM_STATE_POWER_UP);
            }
            break;
        case PM_STATE_WARM_RESET:
            /* Warm reset: KBRST_L asserted without dropping MAIN rails.
            * Re-validate BIOS ROM, then release KBRST_L. */
            Debug_Print("[PM] Warm reset: asserting KBRST_L...\r\n");
            IfxPort_setPinLow(AppPin_GetPort(PIN_WARM_RST.portIdx),
                            PIN_WARM_RST.pinIdx);

            /* UART MUX to AURIX during reset for debug visibility */
            prv_UartClaimByAurix();

            Stm_DelayMs(10u);   /* KBRST_L minimum assertion time */

            if (!prv_BiosRomValidate())
            {
                Debug_Print("[PM] BIOS ROM validation FAILED during warm reset\r\n");
                s_pendingCause = PM_RESET_CAUSE_BIOS_FAIL;
                prv_OnPgFault(NULL_PTR, 0u);
                break;
            }

            /* Release KBRST_L, hand UART back to SoC */
            prv_UartReleaseToSoc();
            IfxPort_setPinHigh(AppPin_GetPort(PIN_WARM_RST.portIdx),
                            PIN_WARM_RST.pinIdx);

            Debug_Print("[PM] Warm reset complete.\r\n");
            prv_SetState(PM_STATE_ON);
            break;
        default:
            break;
    }
}

PM_ResetCause_t PowerManager_GetResetCause(void)
{
    return s_resetCause;
}

uint8 PowerManager_GetRetryCount(void)
{
    return s_retryCount;
}