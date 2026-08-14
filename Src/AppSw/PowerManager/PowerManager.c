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
#include "IfxCpu.h"

/* ---- Private state ------------------------------------------------------- */

static PM_State_t s_state              = PM_STATE_OFF;
static boolean    s_powerOnReq         = FALSE;
static boolean    s_powerOffReq        = FALSE;
static boolean    s_shutdownToOff      = FALSE;
static volatile boolean s_thermtripIsrFlag = FALSE;
static boolean          s_rsmrstAsserted = FALSE;
static boolean s_waitForRstRelease      = FALSE;
static uint8      s_thermtripDebounce  = 0u;  /* consecutive LOW-read counter */
static uint8      s_rstBtnDebounce     = 0u;
static uint8           s_retryCount    = 0u;
static PM_ResetCause_t s_resetCause    = PM_RESET_CAUSE_NONE;
static PM_ResetCause_t s_pendingCause  = PM_RESET_CAUSE_NONE;
static uint8 s_pwrBtnDebounce = 0u;

static uint32   s_rsmrstDeassertTimeMs = 0u;
static boolean  s_coldBoot             = FALSE;
static uint32  s_pwrBtnPressStartMs = 0u;
static boolean s_pwrBtnWasPressed   = FALSE;
static boolean s_waitForBtnRelease     = FALSE;
static uint32 s_coldRstDwellStartMs = 0u;
static uint32  s_forcedOffMs        = 0u;
static uint32 s_s0i3EntryMs         = 0u;
static boolean s_coldRstDwellActive = FALSE;
static boolean s_suppressResetDetect = FALSE;
static boolean s_slpS3WasActive      = FALSE;  /* last-read SLP_S3_ACTIVE level,
                                                 * for S0i3 wake-edge detect */
static boolean s_retryDelayActive   = FALSE;
static uint32  s_retryDelayStartMs  = 0u;
static uint32 s_coldRstDetectMs = 0u;
static uint8 s_pwrokLossDebounce = 0u;

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

#if (FUSA_FEATURE_ENABLE == 1u)
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
#endif


static boolean prv_PwrokValid(void)
{
    return (boolean)IfxPort_getPinState(
        AppPin_GetPort(PIN_APU_PWROK.portIdx), PIN_APU_PWROK.pinIdx);
}


static boolean prv_VerifyUpstreamPg(PM_State_t stage)
{
    uint8 failIdx = 0u;
    typedef struct {
        const PwrRail_Cfg_t *rails;
        uint8                count;
        PM_State_t           minStage;   /* check when stage >= this */
        const char          *name;
    } PgCheckEntry_t;

    static const PgCheckEntry_t checks[] = {
        { PM_RAILS_EFUSE,  PM_RAIL_EFUSE_COUNT,  PM_STATE_RAMP_ALW,   "EFUSE"   },
        { PM_RAILS_VR3V3,  PM_RAIL_VR3V3_COUNT,  PM_STATE_RAMP_VR3V3, "VR3V3"   },
        { PM_RAILS_GRP_B,  PM_RAIL_GRP_B_COUNT,  PM_STATE_RAMP_S5,    "Group B" },
        { PM_RAILS_GRP_C,  PM_RAIL_GRP_C_COUNT,  PM_STATE_RAMP_S3,    "Group C" },
        { PM_RAILS_GRP_D,  PM_RAIL_GRP_D_COUNT,  PM_STATE_RAMP_S0,    "Group D" },
    };

    uint8 numChecks = (uint8)(sizeof(checks) / sizeof(checks[0]));
    uint8 i;

    for (i = 0u; i < numChecks; i++) {
        if (stage < checks[i].minStage) {
            break;   /* only check groups that should already be up */
        }
        failIdx = 0u;
        if (!PwrGood_WaitAllPg(checks[i].rails, checks[i].count,
                               0u,               /* no ramp delay — already up */
                               PM_PG_TIMEOUT_MS,
                               &failIdx)) {
            Debug_Printf("[PM] PG lost: %s rail %u during %d\r\n",
                         checks[i].name, (unsigned)failIdx, (int)stage);
            s_pendingCause = PM_RESET_CAUSE_PG_LOSS;
            prv_OnPgFault(&checks[i].rails[failIdx], failIdx);
            return FALSE;
        }
    }
    return TRUE;
}


static void prv_UpdateFusaStatus(PM_State_t state)
{
#if (FUSA_FEATURE_ENABLE == 1u)
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
#else
    (void) state;
#endif
}

void PowerManager_OnThermtripIsr(void)
{
    if ((s_state == PM_STATE_ON) && prv_PwrokValid())
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
        if ((s_state == PM_STATE_OFF) || (s_state == PM_STATE_FAULT))
            return;   /* already shut down, ignore */
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

static void prv_DeassertKbrst(void)
{
    IfxPort_setPinLow(AppPin_GetPort(PIN_WARM_RST.portIdx),
                       PIN_WARM_RST.pinIdx);
}

static void prv_AssertKbrst(void)
{
    IfxPort_setPinHigh(AppPin_GetPort(PIN_WARM_RST.portIdx),
                      PIN_WARM_RST.pinIdx);
}

/* ---- SoC reset signal read (AURIX input from SoC) -------------------- */
static boolean prv_ReadSocResetL(void)
{
    return (boolean)IfxPort_getPinState(
        AppPin_GetPort(PIN_APU_RESET_IN_L.portIdx),
        PIN_APU_RESET_IN_L.pinIdx);
}

/* ---- COM-HPC PLTRST# output ----------------------------------------- */
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

/* ---- COM-HPC RSMRST_OUT# output ------------------------------------- */
static void prv_AssertRsmrstOut(void)
{
    IfxPort_setPinLow(AppPin_GetPort(PIN_RSMRST_OUT_L.portIdx),
                      PIN_RSMRST_OUT_L.pinIdx);
}

static void prv_DeassertRsmrstOut(void)
{
    IfxPort_setPinHigh(AppPin_GetPort(PIN_RSMRST_OUT_L.portIdx),
                       PIN_RSMRST_OUT_L.pinIdx);
}

/* ---- Read back AURIX's own RSMRST_L output state --------------------- */
static boolean prv_ReadRsmrstState(void)
{
    return (boolean)IfxPort_getPinState(
        AppPin_GetPort(PIN_MMC_RSMRST_L.portIdx),
        PIN_MMC_RSMRST_L.pinIdx);
}

/* ---- Signal mirrors (COM-HPC spec) -----------------------------------
 * PLTRST#:      SoC RESET_L (PIN_APU_RESET_IN_L) → COM-HPC PLTRST# (PIN_PLTRST_L)
 * RSMRST_OUT#:  AURIX RSMRST_L (PIN_MMC_RSMRST_L read-back) → COM-HPC RSMRST_OUT# (PIN_RSMRST_OUT_L)
 * ---------------------------------------------------------------------- */
static void prv_MirrorResetSignals(void)
{
    /* RESET_L is undetermined before Group B is up or after a fault
     * drops it again — don't pass it through as PLTRST# in that window. */
    if ((s_state < PM_STATE_RAMP_S3) || (s_state == PM_STATE_FAULT))
    {
        prv_AssertPltrst();
        return;
    }

    /* Mirror SoC RESET_L → COM-HPC PLTRST#
     * Per COM-HPC spec: PLTRST# shall not be released while RSTBTN# is low */
    if (prv_ReadSocResetL() &&
        (IfxPort_getPinState(AppPin_GetPort(PIN_CB_RSTBTN_L.portIdx),
                             PIN_CB_RSTBTN_L.pinIdx) != 0u))
        prv_DeassertPltrst();
    else
        prv_AssertPltrst();

    /* Mirror RSMRST_L → COM-HPC RSMRST_OUT# */
    if (prv_ReadRsmrstState())
        prv_DeassertRsmrstOut();
    else
        prv_AssertRsmrstOut();
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

    for (i = (sint8)PM_RAIL_VR3V3_COUNT - 1; i >= 0; i--)
    {
        if (PM_RAILS_VR3V3[i].assertEnable)
        {
            prv_DisableRail(&PM_RAILS_VR3V3[i]);
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
    IfxPort_setPinHigh(AppPin_GetPort(PIN_APU_RESET_OUT_L.portIdx),
                      PIN_APU_RESET_OUT_L.pinIdx);
    /* Reclaim UART immediately after asserting reset — SoC is now in
     * reset so the shared line is free for AURIX diagnostic use. */
    Debug_Print("[PM] COLD_RST asserted. UART MUX -> AURIX (UART_MUX_SEL=1)\r\n");
}

static void prv_DeassertApuReset(void)
{
    /* Hand UART to x86 SoC before releasing reset so it owns the line
     * from its first boot cycle.  This is the last AURIX UART message. */
    IfxPort_setPinLow(AppPin_GetPort(PIN_APU_RESET_OUT_L.portIdx),
                    PIN_APU_RESET_OUT_L.pinIdx);
    Debug_Print("[PM] SYS_RESET_L released\r\n");
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
//  IfxPort_setPinHigh(AppPin_GetPort(PIN_APU_ROM_SPI_SEL.portIdx),
//                     PIN_APU_ROM_SPI_SEL.pinIdx);
    IfxPort_setPinLow(AppPin_GetPort(PIN_APU_ROM_SPI_SEL.portIdx),
                       PIN_APU_ROM_SPI_SEL.pinIdx);

    /* TODO: QSPI0 read of ROM contents + AMD-defined integrity check.
     *       Stub returns TRUE until validation method is defined. */

    /* Release SPI MUX — APU owns BIOS ROM flash. */
//  IfxPort_setPinLow(AppPin_GetPort(PIN_APU_ROM_SPI_SEL.portIdx),
//                    PIN_APU_ROM_SPI_SEL.pinIdx);
    IfxPort_setPinHigh(AppPin_GetPort(PIN_APU_ROM_SPI_SEL.portIdx),
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
    s_rsmrstAsserted = TRUE;

}

static void prv_DeassertRsmrst(void)
{
    IfxPort_setPinHigh(AppPin_GetPort(PIN_MMC_RSMRST_L.portIdx),
                       PIN_MMC_RSMRST_L.pinIdx);
    s_rsmrstAsserted = FALSE;
}

/* Forward declaration — prv_DisableGroup is defined after prv_GoToS5. */
static void prv_DisableGroup(const PwrRail_Cfg_t *rails, uint8 count);

/**
 * @brief Transition to PM_STATE_S5 in response to THERMTRIP assertion.
 *
 * Keeps Group B (S5 rails) and the 12V EFUSE powered so the platform
 * can recover when the thermal condition clears.  Group C (memory) and
 * Group D (VDDCR) are powered down in reverse order.  Neither COLD_RST
 * (SYS_RESET_L) nor RSMRST_L is asserted here — Group B stays powered
 * throughout this S0->S5 transition, so neither signal is toggled;
 * they're only released during the S5-and-above power-on ramp.
 */
static void prv_GoToS5(void)
{
    Debug_Print("[PM] THERMTRIP asserted — suspending to S5 "
                "(Group B + EFUSE remain on)\r\n");

    /* Secure APU and deassert PWRGD before touching rails. */
    prv_UartClaimByAurix();
    prv_AssertKbrst();
    prv_DeassertPwrgd();
    //prv_AssertPltrst();
    PwrGood_MonDisarm();
    VoltMon_Disable();
#if (FUSA_FEATURE_ENABLE == 1u)
    ComHpcWdt_Disable();
#endif
    /* Disable Group D (VDDCR core) then Group C (memory).
     * Group B (S5 rails) and EFUSE stay powered. */
    prv_DisableGroup(PM_RAILS_GRP_D, PM_RAIL_GRP_D_COUNT);
    Stm_DelayMs(PM_GRP_D_OFF_DWELL_MS);
    prv_DisableGroup(PM_RAILS_GRP_C, PM_RAIL_GRP_C_COUNT);
    Stm_DelayMs(PM_GRP_C_OFF_DWELL_MS);

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
    prv_UartClaimByAurix();
    prv_AssertKbrst();
    prv_AssertRsmrst();
    prv_DeassertPwrgd();
    //prv_AssertPltrst();
    PwrGood_MonDisarm();
    VoltMon_Disable();
    ComHpcWdt_Disable();     /* WDT could also fire during rail-down */
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
    return (boolean)IfxPort_getPinState(
        AppPin_GetPort(PIN_THERMTRIP_L.portIdx), PIN_THERMTRIP_L.pinIdx);
}

static boolean prv_VinPwrOk(void)
{
    return (boolean)IfxPort_getPinState(
        AppPin_GetPort(PIN_VIN_PWR_OK.portIdx), PIN_VIN_PWR_OK.pinIdx);
}


static void prv_WaitSinceRsmrst(uint16 minMs)
{
    uint32 elapsed = Stm_GetTimeMs() - s_rsmrstDeassertTimeMs;
    if (elapsed < (uint32)minMs)
    {
        Stm_DelayMs((uint16)((uint32)minMs - elapsed));
    }
}

static boolean prv_WaitSlpDeassert(uint16 timeoutMs)
{
    uint16 ms = 0u;
    while ((prv_SlpS5Active() || prv_SlpS3Active()) && (ms < timeoutMs))
    {
        Stm_DelayMs(1u);
        ms++;
    }
    if (prv_SlpS5Active() || prv_SlpS3Active())
    {
        Debug_Printf("[PM] SLP timeout after %ums (S5=%u S3=%u)\r\n",
                     (unsigned)ms,
                     (unsigned)prv_SlpS5Active(),
                     (unsigned)prv_SlpS3Active());
        return FALSE;
    }
    Debug_Printf("[PM] SLP deasserted after %ums\r\n", (unsigned)ms);
    return TRUE;
}

/* Cold boot: PWR_BTN assertion, min 16ms per Table 30 T2 (+margin). */
static void prv_PulsePwrBtnCold(void)
{
    IfxPort_setPinLow(AppPin_GetPort(PIN_APU_PWRBTN.portIdx),
                      PIN_APU_PWRBTN.pinIdx);
    Stm_DelayMs(18u);
    IfxPort_setPinHigh(AppPin_GetPort(PIN_APU_PWRBTN.portIdx),
                       PIN_APU_PWRBTN.pinIdx);
    Debug_Print("[PM] APU_PWRBTN pulsed 18ms (T3 cold)\r\n");
}

/* S0i3 resume (S0 -> S3 -> S0): 16ms minimum per AMD T2 Table 30. */
static void prv_PulsePwrBtnWarm(void)
{
    IfxPort_setPinLow(AppPin_GetPort(PIN_APU_PWRBTN.portIdx),
                      PIN_APU_PWRBTN.pinIdx);
    Stm_DelayMs(18u);
    IfxPort_setPinHigh(AppPin_GetPort(PIN_APU_PWRBTN.portIdx),
                       PIN_APU_PWRBTN.pinIdx);
    Debug_Print("[PM] APU_PWRBTN pulsed 18ms (T2 warm)\r\n");
}


static void prv_EmergencyShutdown(PM_ResetCause_t cause)
{
    s_resetCause = cause;
    s_coldBoot = FALSE;
    prv_AssertApuReset();
    prv_UartClaimByAurix();
    prv_AssertKbrst();
    prv_AssertRsmrst();
    prv_DeassertPwrgd();
    //prv_AssertPltrst();
    PwrGood_MonDisarm();
    VoltMon_Disable();
    ComHpcWdt_Disable();
    prv_DisableAllRails();
    prv_SetState(PM_STATE_OFF);
}


/* ---- Public API ---------------------------------------------------------- */

void PowerManager_Init(void)
{
    /* Populate rail tables before any GPIO access (Tasking E306 workaround). */
    PowerManager_CfgInit();
    prv_DeassertPwrgd();
    prv_AssertApuReset();
    prv_AssertKbrst();
    prv_AssertRsmrst();     /* hold RSMRST_L until S5 rails stable + 10ms */
    //prv_AssertPltrst();
    PwrGood_MonDisarm();
    s_state             = PM_STATE_OFF;
    s_powerOnReq        = FALSE;
    s_powerOffReq       = FALSE;
    s_shutdownToOff     = FALSE;
    s_waitForBtnRelease = FALSE;
    s_waitForRstRelease = FALSE;
    s_rstBtnDebounce    = 0u;
    s_thermtripDebounce = 0u;
    s_retryCount        = 0u;
    s_pwrBtnWasPressed   = FALSE;
    s_pwrBtnPressStartMs   = 0u;
    s_pwrBtnDebounce = 0u;
    s_forcedOffMs = 0u;
    s_s0i3EntryMs = 0u;
    s_coldBoot = FALSE;
    s_suppressResetDetect = FALSE;
    s_slpS3WasActive = FALSE;
    s_retryDelayActive = FALSE;
    s_retryDelayStartMs = 0u;
    s_coldRstDetectMs = 0u;
    s_pwrokLossDebounce = 0u;
    s_resetCause        = PM_RESET_CAUSE_NONE;
    s_pendingCause      = PM_RESET_CAUSE_NONE;
#if (FUSA_FEATURE_ENABLE == 1u)
    prv_SetFusaStatus(FUSA_PWR_OFF);
#endif
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
    boolean thermtripLocal;
    prv_MirrorResetSignals();
    /* --- Atomic capture of ISR flag ---
     * Disable interrupts so that no ISR can set the flag between
     * our read and our clear.  The critical section is two
     * instructions (~2 cycles) — negligible interrupt latency. */
    IfxCpu_disableInterrupts();
    thermtripLocal = s_thermtripIsrFlag;
    s_thermtripIsrFlag = FALSE;
    IfxCpu_enableInterrupts();
    if (thermtripLocal)
    {
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
//      if ((s_state != PM_STATE_OFF)   &&
//          (s_state != PM_STATE_S5)    &&
//          (s_state != PM_STATE_FAULT))
        if (s_state == PM_STATE_ON  && prv_PwrokValid())
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

    if (!prv_VinPwrOk() &&
        (s_state != PM_STATE_OFF) &&
        (s_state != PM_STATE_FAULT))
    {
        Debug_Print("[PM] VIN_PWR_OK lost — emergency shutdown\r\n");
        prv_EmergencyShutdown(PM_RESET_CAUSE_PG_LOSS);
        return;
    }

    /* Run the PG monitor on every call (only active when armed). */
    PwrGood_MonRun();

    /* Check for PWR_BTN press (edge detection for ON request). */
    if (prv_PwrBtnPressed())
    {
        if (!s_pwrBtnWasPressed && !s_waitForBtnRelease)
        {
            s_pwrBtnDebounce++;
            if (s_pwrBtnDebounce >= PM_PWRBTN_DEBOUNCE_POLLS)
            {
                s_pwrBtnDebounce     = 0u;
                s_pwrBtnWasPressed   = TRUE;
                s_pwrBtnPressStartMs = Stm_GetTimeMs();
            }
        }
        else if ((s_state != PM_STATE_OFF) &&
                (s_state != PM_STATE_FAULT) &&
                (Stm_GetTimeMs() - s_pwrBtnPressStartMs >= PM_PWRBTN_HOLD_MS))
        {
            Debug_Print("[PM] PWRBTN# held >=4s — forced shutdown\r\n");
            s_pwrBtnWasPressed = FALSE;
            s_waitForBtnRelease = TRUE;
            s_forcedOffMs = Stm_GetTimeMs();
            prv_EmergencyShutdown(PM_RESET_CAUSE_HOST_REQUEST);
            return;
        }
    } else {
        s_pwrBtnDebounce = 0u;   /* reset debounce on release */
        if (s_pwrBtnWasPressed &&
            (Stm_GetTimeMs() - s_pwrBtnPressStartMs < PM_PWRBTN_HOLD_MS))
        {
            if (s_state == PM_STATE_ON)
            {
                Debug_Print("[PM] PWRBTN short press — forwarding to APU\r\n");
                prv_PulsePwrBtnCold();
            }
            else
            {
                s_powerOnReq = TRUE;
            }
        }
        s_pwrBtnWasPressed = FALSE;
        s_waitForBtnRelease = FALSE;
    }

    switch (s_state)
    {
        /* ------------------------------------------------------------------ */
        case PM_STATE_OFF:
            if (s_powerOnReq && prv_VinPwrOk())
            {
                /* After forced shutdown, ignore power-on requests for
                 * 2 seconds to reject button bounce on release. */
                if ((s_forcedOffMs != 0u) &&
                    (Stm_GetTimeMs() - s_forcedOffMs < PM_FORCED_OFF_COOLDOWN_MS))
                {
                    s_powerOnReq = FALSE;
                    break;
                }
                s_forcedOffMs = 0u;
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
            Debug_Print("[PM] Power-up sequence started.\r\n");
            VoltMon_Disable();
            s_coldBoot = TRUE;
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
//          prv_SetState(PM_STATE_RAMP_S5);
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
//          prv_SetState(PM_STATE_RAMP_ALW);
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

            if (!prv_VerifyUpstreamPg(PM_STATE_RAMP_S5)) break;

            prv_DeassertApuReset();
            Stm_DelayMs(PM_RSMRST_DELAY_AFTER_S5_MS);
                        /* Hand UART to APU just before RSMRST_L release */
            prv_UartReleaseToSoc();
            if (s_rsmrstAsserted)
            {
                prv_DeassertRsmrst();
                s_rsmrstDeassertTimeMs = Stm_GetTimeMs();
                Debug_Print("[PM] RSMRST_L deasserted (S5 rails stable + 10ms).\r\n");
                prv_WaitSinceRsmrst(PM_RTCCLK_STABLE_MS);   /* T1a: >=16ms RSMRST to PWR_BTN */
            }

            prv_PulsePwrBtnCold();

            if (!prv_WaitSlpDeassert(PM_SLP_S3_TIMEOUT_MS))
            {
                /* RTCCLK may not have been stable yet (Table 28 T2: up to 400ms) —
                * one re-pulse before declaring fault */
                Debug_Print("[PM] SLP timeout — re-pulsing PWR_BTN (RTCCLK late?)\r\n");
                prv_PulsePwrBtnCold();
                if (!prv_WaitSlpDeassert(PM_SLP_S3_TIMEOUT_MS))
                {
                    s_pendingCause = PM_RESET_CAUSE_PG_TIMEOUT;
                    prv_OnPgFault(NULL_PTR, 0u);
                    break;
                }
            }

            prv_SetState(PM_STATE_RAMP_S3);
            break;

        /* ------------------------------------------------------------------ */
        /* Stage 2: Group C — memory rails */
        case PM_STATE_RAMP_S3:
            if (!s_coldBoot && prv_SlpS5Active())
            {
                Debug_Print("[PM] SLP_S5 asserted during S0i3 resume — aborting\r\n");
                s_pendingCause = PM_RESET_CAUSE_PG_TIMEOUT;
                prv_OnPgFault(NULL_PTR, 0u);
                break;
            }
            if (!prv_RampGroup(PM_RAILS_GRP_C, PM_RAIL_GRP_C_COUNT))
            {
                s_pendingCause = PM_RESET_CAUSE_PG_TIMEOUT;
                prv_OnPgFault(&PM_RAILS_GRP_C[0], 0u);
                break;
            }

            if (!prv_VerifyUpstreamPg(PM_STATE_RAMP_S3)) break;
            prv_SetState(PM_STATE_RAMP_S0);
            break;

        /* ------------------------------------------------------------------ */
        /* Stage 3: Group D — VDDCR core */
        case PM_STATE_RAMP_S0:
            if (!s_coldBoot && prv_SlpS5Active())
            {
                Debug_Print("[PM] SLP_S5 asserted during Group D ramp — aborting\r\n");
                s_pendingCause = PM_RESET_CAUSE_PG_TIMEOUT;
                prv_OnPgFault(NULL_PTR, 0u);
                break;
            }
            if (!prv_RampGroup(PM_RAILS_GRP_D, PM_RAIL_GRP_D_COUNT))
            {
                s_pendingCause = PM_RESET_CAUSE_PG_TIMEOUT;
                prv_OnPgFault(&PM_RAILS_GRP_D[0], 0u);
                break;
            }
            if (s_coldBoot)
            {
                if (!prv_BiosRomValidate())
                {
                    Debug_Print("[PM] BIOS ROM validation FAILED — blocking boot\r\n");
                    s_pendingCause = PM_RESET_CAUSE_BIOS_FAIL;
                    prv_OnPgFault(NULL_PTR, 0u);
                    break;
                }
            }
            /* was: prv_AssertPwrgd(); delay; prv_BiosRomValidate(); */
            prv_AssertPwrgd();
            Stm_DelayMs(PM_RESET_HOLD_AFTER_PWRGD_MS);

            if (!prv_VerifyUpstreamPg(PM_STATE_RAMP_S0)) break;
            prv_DeassertKbrst();  /* KBRST_L released    */

            /* Added  PWROK check with PIN_APU_PWROK */
            /* T6: wait for SoC PWROK assertion (21.4ms per AMD spec) */
            {
                uint16 pwrokWaitMs = 0u;
                while (!IfxPort_getPinState(AppPin_GetPort(PIN_APU_PWROK.portIdx),
                                            PIN_APU_PWROK.pinIdx) &&
                        (pwrokWaitMs < 100u))  /* T5 ceiling: 100ms */
                {
                    Stm_DelayMs(1u);
                    pwrokWaitMs++;
                }
                if (!IfxPort_getPinState(AppPin_GetPort(PIN_APU_PWROK.portIdx),
                                            PIN_APU_PWROK.pinIdx))
                {
                    Debug_Print("[PM] SoC PWROK not asserted — startup fault\r\n");
                    s_pendingCause = PM_RESET_CAUSE_PG_TIMEOUT;
                    prv_OnPgFault(NULL_PTR, 0u);
                    break;
                }

                Debug_Printf("[PM] SoC PWROK asserted after %ums\r\n",
                                (unsigned)pwrokWaitMs);
            }
            Stm_DelayMs(8u);   /* T7-T6 = 7.1ms per flowchart before enabling monitors */
            PwrGood_MonArm(PM_RAILS_ALL_MON, PM_RAIL_ALL_MON_COUNT, prv_OnPgFault);
#if (FUSA_FEATURE_ENABLE == 1u)
            ComHpcWdt_Enable(COMHPC_WDT_DEFAULT_ENABLE_DELAY_S,
                            COMHPC_WDT_DEFAULT_TIMEOUT_MS);
#endif
            s_retryCount = 0u;
            VoltMon_Enable();
            //prv_DeassertPltrst();
            prv_SetState(PM_STATE_ON);
            Debug_Print("[PM] System ON.\r\n");
            break;

        /* ------------------------------------------------------------------ */
        case PM_STATE_ON:
            /* Observe APU_RESET_L for visibility only — by design this must
             * not change S0/S3/S5 power state. Log once per assertion. */
            if (prv_ReadSocResetL() && !prv_PwrokValid())
            {
                s_pwrokLossDebounce++;
                if (s_pwrokLossDebounce >= PM_PG_DEBOUNCE_POLLS)
                {
                    Debug_Print("[PM] SoC PWROK lost in S0\r\n");
                    s_pendingCause = PM_RESET_CAUSE_PG_LOSS;
                    prv_OnPgFault(NULL_PTR, 0u);
                    break;
                }
            }
            else 
            {
                s_pwrokLossDebounce = 0u;
            }

            if (!prv_ReadSocResetL())
            {
                if (!s_suppressResetDetect)
                {
                    Debug_Print("[PM] APU_RESET_L asserted (observed only)\r\n");
                    s_coldRstDetectMs = Stm_GetTimeMs();
                    s_suppressResetDetect = TRUE;
                }
            }
            else
            {
                s_suppressResetDetect = FALSE;
            }

            if (s_powerOffReq || prv_SlpS5Active())
            {
                
                if (!s_powerOffReq &&
                    (s_coldRstDetectMs != 0u) &&
                    ((Stm_GetTimeMs() - s_coldRstDetectMs) <= 500u))
                {
                    s_resetCause = PM_RESET_CAUSE_COLD_RST;   /* CF9-style cold reset */
                }
                s_coldRstDetectMs = 0u;
                Debug_Print("[PM] SLP_S5 active — soft shutdown to S5\r\n");
                s_powerOffReq = FALSE;
                s_shutdownToOff = TRUE;
                s_waitForBtnRelease = TRUE;
                s_thermtripDebounce = 0u;
                /* Neither SYS_RESET_L nor RSMRST_L is asserted here —
                 * Group B stays powered throughout this S0->S5
                 * transition, so neither signal is toggled. */
                prv_UartClaimByAurix();
                prv_AssertKbrst();
                //prv_AssertRsmrst();
                prv_DeassertPwrgd();
                //prv_AssertPltrst();
                PwrGood_MonDisarm();
                VoltMon_Disable();
                ComHpcWdt_Disable();
                prv_SetState(PM_STATE_DN_S0_S3);
                break; 
            }
            else if (prv_SlpS3Active())
            {
                Debug_Print("[PM] SLP_S3 active — entering S0i3\r\n");
                s_shutdownToOff = FALSE;
                s_s0i3EntryMs = Stm_GetTimeMs();
                s_slpS3WasActive = TRUE;   /* arm wake-edge detect for DN_S3_S5 */
                prv_UartClaimByAurix();
                prv_AssertKbrst();
                // prv_AssertRsmrst();
                prv_DeassertPwrgd();
                //prv_AssertPltrst();
                PwrGood_MonDisarm();
                VoltMon_Disable();
                ComHpcWdt_Disable();
                prv_SetState(PM_STATE_DN_S0_S3);
                break;
            }
            if (IfxPort_getPinState(AppPin_GetPort(PIN_CB_RSTBTN_L.portIdx),
                                     PIN_CB_RSTBTN_L.pinIdx) == 0u)
            {
                if (!s_waitForRstRelease)
                {
                    s_rstBtnDebounce++;
                    if (s_rstBtnDebounce >= PM_RSTBTN_DEBOUNCE_POLLS)
                    {
                        s_rstBtnDebounce = 0u;
                        s_waitForRstRelease = TRUE;
                        Debug_Print("[PM] RSTBTN# pressed — pulsing COLD_RST\r\n");

                        /* COLD_RST pulse only — no rail or state changes.
                         * The APU performs its own full reset and reboot
                         * autonomously. PG-Loss monitoring stays armed
                         * throughout; any suppression needed while the APU
                         * is mid-reset is already driven by its own
                         * APU_RESET_L assertion (e.g. PLTRST# mirroring in
                         * prv_MirrorResetSignals()), not by this handler. */
                        s_suppressResetDetect = TRUE; //Prevents APU
                        prv_AssertApuReset();
                        Stm_DelayMs(PM_COLD_RST_PULSE_MS);
                        /* COM-HPC: module shall stay in reset while RSTBTN# is held low */
                        while (IfxPort_getPinState(AppPin_GetPort(PIN_CB_RSTBTN_L.portIdx),
                           PIN_CB_RSTBTN_L.pinIdx) == 0u)
                        {
                            Stm_DelayMs(1u);
                        }                        
                        s_coldRstDetectMs = Stm_GetTimeMs(); 
                        prv_DeassertApuReset();
                    }
                }
            }
            else
            {
                s_rstBtnDebounce = 0u;        /* clear on release */
                s_waitForRstRelease = FALSE;  /* re-arm for next press */
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
            if (s_resetCause == PM_RESET_CAUSE_COLD_RST)
            {
                if (!s_coldRstDwellActive)
                {
                    s_coldRstDwellActive = TRUE;
                    s_coldRstDwellStartMs = Stm_GetTimeMs();
                    Debug_Print("[PM] Cold reset: dwelling at S5 for 3s\r\n");
                    break;  /* just armed — don't process wake events yet */
                }
                else if ((Stm_GetTimeMs() - s_coldRstDwellStartMs) >= 3000u)
                {
                    s_coldRstDwellActive = FALSE;
                    Debug_Print("[PM] Cold reset: dwell complete, re-powering\r\n");
                    s_coldRstDwellStartMs = 0u;
                    s_resetCause = PM_RESET_CAUSE_NONE;
                    s_powerOnReq = TRUE;
                    /* falls through to existing wake logic below */
                }
                else
                {
                    break;  /* still dwelling, don't process wake events */
                }
            }
            //if ((s_powerOnReq || prv_PwrBtnPressed()) && !prv_ThermTripActive())
            //{
            if (s_waitForBtnRelease)
            {
                if (!prv_PwrBtnPressed())
                    s_waitForBtnRelease = FALSE;
                break;   /* don't process wake events until released */
            }


            if (s_powerOnReq || prv_PwrBtnPressed())
            {
                s_powerOnReq = FALSE;
                s_coldBoot = TRUE;
                s_waitForBtnRelease = TRUE;
                s_pwrBtnWasPressed   = FALSE;   /* consume the press — prevent hold-timer */
                s_pwrBtnPressStartMs = 0u;
                s_pwrBtnDebounce     = 0u;
                Debug_Print("[PM] Wake from S5\r\n");

                prv_DeassertApuReset();
                Stm_DelayMs(PM_RSMRST_DELAY_AFTER_S5_MS);
                prv_UartReleaseToSoc();

                /* RSMRST_L is not re-asserted anywhere on the way into
                 * S5, so it's already been deasserted (and T1a already
                 * satisfied) since the original S5 power-on ramp —
                 * nothing to redo here. */
                prv_PulsePwrBtnCold();
                if (!prv_WaitSlpDeassert(PM_SLP_S3_TIMEOUT_MS))
                {
                    s_pendingCause = PM_RESET_CAUSE_PG_TIMEOUT;
                    prv_OnPgFault(NULL_PTR, 0u);
                    break;
                }
                prv_SetState(PM_STATE_RAMP_S3);
            }
           // }
    //        else
    //        {
    //            /* Don't need to read THERMTRIP in this state */
    //           s_thermtripDebounce = 0u;   /* keep counter reset while in S5 */
    //        }
            break;

        /* ------------------------------------------------------------------ */
        case PM_STATE_DN_S0_S3:
            /* Disable Group D (VDDCR core) then Group C (memory) */
            prv_DisableGroup(PM_RAILS_GRP_D, PM_RAIL_GRP_D_COUNT);
            Stm_DelayMs(PM_GRP_D_OFF_DWELL_MS);
            if (s_shutdownToOff) {
                prv_DisableGroup(PM_RAILS_GRP_C, PM_RAIL_GRP_C_COUNT);
                Stm_DelayMs(PM_GRP_C_OFF_DWELL_MS);
            }
            /* S0i3: Group C stays powered on */
            prv_SetState(PM_STATE_DN_S3_S5);
            break;

        /* ------------------------------------------------------------------ */

        /*
        
        In this section, power button detected -> PWR_BTN -> wait T3 -> verify SLP signals deassert -> ramp C + D

        */
        case PM_STATE_DN_S3_S5:
        {
            /* SLP_S3_ACTIVE falling edge == physical SLP_S3_L rising edge
             * (this pin is a buffered/inverted active-HIGH view of the raw
             * active-low SLP_S3_L, see prv_SlpS3Active()). The APU can exit
             * S3 on its own (WoL, RTC, etc.) without any physical PWR_BTN
             * press, so that edge — not just the button — must trigger the
             * S0i3 wake sequence below. */
            boolean slpS3ActiveNow;
            boolean slpS3WakeEdge;

            slpS3ActiveNow = prv_SlpS3Active();

            /* AMD datasheet defines T1' as the minimum entry time to S0i3.
             * Hold off evaluating the wake edge until PM_T1_PRIME_MS has
             * elapsed since S0i3 entry (s_s0i3EntryMs). */
            if ((Stm_GetTimeMs() - s_s0i3EntryMs) >= PM_T1_PRIME_MS)
            {
                slpS3WakeEdge    = (s_slpS3WasActive && !slpS3ActiveNow);
                s_slpS3WasActive = slpS3ActiveNow;
            }
            else
            {
                slpS3WakeEdge = FALSE;
            }

            if (!s_shutdownToOff && (prv_PwrBtnPressed() || slpS3WakeEdge))
            {
                /* S0i3 wake: pulse PWR_BTN to SoC, then verify SLP deassert.
                 * On an autonomous wake (SLP_S3 edge, e.g. WoL/RTC), the
                 * chipset has already initiated its own wake by deasserting
                 * SLP_S3 — pulsing PWR_BTN here would be an unrequested
                 * input into a chipset that's already waking, so skip it. */
                s_coldBoot = FALSE;
                /* This handler consumes the press directly — clear the
                 * top-level edge detector's tracking so it doesn't later
                 * re-evaluate a stale s_pwrBtnPressStartMs against a
                 * different state and misfire a forced shutdown. */
                s_pwrBtnWasPressed   = FALSE;
                s_pwrBtnPressStartMs = 0u;
                s_pwrBtnDebounce     = 0u;
                //prv_DeassertRsmrst();
                prv_UartReleaseToSoc();
                prv_DeassertKbrst();
                if (slpS3WakeEdge)
                {
                    Debug_Print("[PM] SLP_S3_L rising edge — autonomous S0i3 wake, "
                                "skipping PWR_BTN pulse\r\n");
                }
                else
                {
                    prv_PulsePwrBtnWarm();
                }

                if (!prv_WaitSlpDeassert(PM_SLP_S3_TIMEOUT_MS))
                {
                    s_pendingCause = PM_RESET_CAUSE_PG_TIMEOUT;
                    prv_OnPgFault(NULL_PTR, 0u);
                    break;
                }
                s_s0i3EntryMs = 0u;
                prv_SetState(PM_STATE_RAMP_S3);
            }
            else if (s_shutdownToOff)
            {
                /* Soft power off: park at S5.
                 * Group D + C already disabled in DN_S0_S3.
                 * Group B + EFUSE remain powered for fast wake. */
                Debug_Print("[PM] Soft shutdown — parking at S5 "
                            "(Group B + EFUSE remain on)\r\n");
                s_thermtripDebounce = 0u;
                s_s0i3EntryMs = 0u;
                prv_SetState(PM_STATE_S5);
            }
            else if (!s_shutdownToOff)
            {
                /* ---- S0i3 safety checks while waiting for wake ---- */
                /* 1. Verify rails that should still be up (EFUSE + B + C) */
                if (!prv_VerifyUpstreamPg(PM_STATE_RAMP_S3))
                {
                    Debug_Print("[PM] PG lost during S0i3 suspend\r\n");
                    /* prv_VerifyUpstreamPg already called prv_OnPgFault */
                    s_s0i3EntryMs = 0u;
                    break;
                }
                /* 2. Timeout: if no wake event within limit, fault out */
                if ((s_s0i3EntryMs != 0u) &&
                    (Stm_GetTimeMs() - s_s0i3EntryMs >= PM_S0I3_TIMEOUT_MS))
                {
                    Debug_Print("[PM] S0i3 timeout — no wake event, shutting down\r\n");
                    s_s0i3EntryMs = 0u;
                    s_pendingCause = PM_RESET_CAUSE_PG_TIMEOUT;
                    prv_OnPgFault(NULL_PTR, 0u);
                    break;
                }
            }
            break;
        }

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
                /* Non-blocking retry delay — a blocking Stm_DelayMs() here
                 * would stall SysMonitor/VoltMon/UsbPdManager servicing
                 * (including THERMTRIP handling) for the full delay on
                 * every retry. */
                if (!s_retryDelayActive)
                {
                    s_retryDelayActive  = TRUE;
                    s_retryDelayStartMs = Stm_GetTimeMs();
                }
                else if ((Stm_GetTimeMs() - s_retryDelayStartMs) >= PM_RETRY_DELAY_MS)
                {
                    s_retryDelayActive = FALSE;
                    Debug_Printf("[PM] Retrying power-on (attempt %u)...\r\n",
                                (unsigned)s_retryCount);
                    prv_SetState(PM_STATE_POWER_UP);
                }
            }
            /* Latch-off: power cycle required. No retries remaining. */
            break;
        case PM_STATE_WARM_RESET:
            /* Warm reset: KBRST_L asserted without dropping MAIN rails.
            * Re-validate BIOS ROM, then release KBRST_L. */
            Debug_Print("[PM] Warm reset: asserting KBRST_L...\r\n");
            prv_AssertKbrst();
            //prv_AssertPltrst();

            /* UART MUX to AURIX during reset for debug visibility */
            prv_UartClaimByAurix();

            Stm_DelayMs(10u);   /* KBRST_L minimum assertion time */
            if (!prv_VerifyUpstreamPg(PM_STATE_RAMP_S0))
            {
                Debug_Print("[PM] PG lost during warm reset — fault\r\n");
                /* prv_VerifyUpstreamPg already called prv_OnPgFault */
                break;
            }

            if (!prv_BiosRomValidate())
            {
                Debug_Print("[PM] BIOS ROM validation FAILED — blocking boot\r\n");
                s_pendingCause = PM_RESET_CAUSE_BIOS_FAIL;
                prv_OnPgFault(NULL_PTR, 0u);
                break;
            }

            /* Release KBRST_L, hand UART back to SoC */
            prv_UartReleaseToSoc();
            prv_DeassertKbrst();
            PwrGood_MonArm(PM_RAILS_ALL_MON, PM_RAIL_ALL_MON_COUNT, prv_OnPgFault);
            VoltMon_Enable();
#if (FUSA_FEATURE_ENABLE == 1u)
            ComHpcWdt_Enable(COMHPC_WDT_DEFAULT_ENABLE_DELAY_S,
                 COMHPC_WDT_DEFAULT_TIMEOUT_MS);
#endif
            Debug_Print("[PM] Warm reset complete.\r\n");
            s_suppressResetDetect = TRUE;
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
