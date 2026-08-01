/**
 * @file    PowerManager_Cfg.c
 * @brief   Power rail configuration tables — runtime initialisation.
 *
 * Sequencing revised per AMD document 58241 Rev 0.50 Section 16 and
 * COM-HPC platform requirements:
 *
 * EFUSE Stage (3 entries):
 *   [0] 12V_EFUSE enable      — Assert MAIN_12V_EFUSE_EN, verify
 *       MAIN_12V_EFUSE_PG. COM-HPC requirement: 12V output confirmed
 *       before enabling Group B VRMs.
 *   [1] VR_APU_3V3 post-EFUSE — Re-verify VR_APU_3V3_PG is still stable
 *       after EFUSE load connects.  assertEnable=FALSE.
 *
 * Group B Stage (4 entries):
 *   [0] VR_APU_3V3 pre-Group B — User requirement: verify VR_APU_3V3_PG
 *       before asserting PWR_GROUP_B_EN (feeds load switches).
 *       assertEnable=FALSE.
 *   [1] VDD_MISC — Assert PWR_GROUP_B_EN, check VDD_MISC_S5 (0.75V).
 *       AMD Table 26: VDD_MISC_S5 must ramp first within Group B.
 *   [2] VDD_1V2  — VDD_12_S5 (1.2V).  assertEnable=FALSE.
 *   [3] VDD_1V8  — VDD_18_S5 (1.8V).  assertEnable=FALSE.
 *       (VDDIO_33_S5 load switch enabled by Group B enable; its PG is
 *       monitored via VR_APU_3V3_PG which is already verified in [0].)
 *
 * Group C Stage (6 entries): VDD_MEM / VDDIO_MEM / VDD_MEMQ Ch A & B.
 *   AMD §16.1.2: Group B fully stable before Group C > 10%.
 *   RSMRST_L is deasserted in PowerManager.c after Group B PGs are
 *   confirmed (AMD T1 min 10ms).
 *
 * Group D Stage (3 entries): VDDCR via MP2825A + MP86979.
 *   AMD §16.1.2: Group C fully stable before Group D > 10%.
 *   MP2825A takes VR_APU_3V3 as bias — already confirmed in Stage 0.
 *
 * RESET_L / PWR_GOOD ordering (in PowerManager.c):
 *   After Group D stable:
 *     1. Wait PM_PWRGD_DEGLITCH_MS (5ms) — AMD §16.1.1: ≥1ms before PWRGD
 *     2. Assert APU_PWR_GOOD
 *     3. Wait PM_RESET_HOLD_AFTER_PWRGD_MS (30ms) — AMD §16.1.5 T7: ≥28.5ms
 *     4. Deassert COLD_RST (RESET_L goes HIGH)
 */

#include "PowerManager_Cfg.h"

PwrRail_Cfg_t PM_RAILS_EFUSE[PM_RAIL_EFUSE_COUNT];
PwrRail_Cfg_t PM_RAILS_GRP_B[PM_RAIL_GRP_B_COUNT];
PwrRail_Cfg_t PM_RAILS_GRP_C[PM_RAIL_GRP_C_COUNT];
PwrRail_Cfg_t PM_RAILS_GRP_D[PM_RAIL_GRP_D_COUNT];
PwrRail_Cfg_t PM_RAILS_ALL_MON[PM_RAIL_ALL_MON_COUNT];
PwrRail_Cfg_t PM_RAILS_VR3V3[PM_RAIL_VR3V3_COUNT];

void PowerManager_CfgInit(void)
{

    /* ====================================================================
    * STAGE 0 — VR_APU_3V3 (1 entry)
    * NB706A powered from standby rail (+3V5B), independent of 12V EFUSE.
    * Must be stable before any downstream rail is enabled.
    * ==================================================================== */

    PM_RAILS_VR3V3[0].name         = "VR_APU_3V3";
    PM_RAILS_VR3V3[0].enablePin    = PIN_MAIN_12V_EFUSE_EN; /* unused */
    PM_RAILS_VR3V3[0].assertEnable = FALSE;
    PM_RAILS_VR3V3[0].pgoodPin     = PIN_VIN_PWR_OK;
    PM_RAILS_VR3V3[0].rampDelayMs  = 0u;
    PM_RAILS_VR3V3[0].pgTimeoutMs  = 1000u;


    /* [0] Pre-EFUSE: verify VR_APU_3V3 (Group A) is present.
     *     AMD §16.1.2: Group A must be stable before Group B > 10%.
     *     VR_APU_3V3 also feeds MP2825A bias (Group D) and load switches.
     *     No enable asserted — read only. 1-second timeout to allow
     *     carrier 3V3 to stabilise after carrier power-on. */
    PM_RAILS_EFUSE[0].name         = "12V_EFUSE";
    PM_RAILS_EFUSE[0].enablePin    = PIN_MAIN_12V_EFUSE_EN;
    PM_RAILS_EFUSE[0].assertEnable = TRUE;
    PM_RAILS_EFUSE[0].pgoodPin     = PIN_MAIN_12V_EFUSE_PG;
    PM_RAILS_EFUSE[0].rampDelayMs  = 5u;
    PM_RAILS_EFUSE[0].pgTimeoutMs  = 100u;

    /* [1] Re-verify VR_APU_3V3 stable under 12V load. */
    PM_RAILS_EFUSE[1].name         = "VR_APU_3V3_POST_EFUSE";
    PM_RAILS_EFUSE[1].enablePin    = PIN_MAIN_12V_EFUSE_EN; /* unused */
    PM_RAILS_EFUSE[1].assertEnable = FALSE;
    PM_RAILS_EFUSE[1].pgoodPin     = PIN_VR_APU_3V3_PG;
    PM_RAILS_EFUSE[1].rampDelayMs  = 2u;
    PM_RAILS_EFUSE[1].pgTimeoutMs  = 50u;

/*
    PM_RAILS_EFUSE[2].name         = "VR_APU_3V3_POST_EFUSE";
    PM_RAILS_EFUSE[2].enablePin    = PIN_MAIN_12V_EFUSE_EN; // unused
    PM_RAILS_EFUSE[2].assertEnable = FALSE;
    PM_RAILS_EFUSE[2].pgoodPin     = PIN_VR_APU_3V3_PG;
    PM_RAILS_EFUSE[2].rampDelayMs  = 2u;
    PM_RAILS_EFUSE[2].pgTimeoutMs  = 50u;
*/


    /* ====================================================================
     * STAGE 1 — Group B / S5 rails (4 entries)
     * NB706/NB693A powered by 12V_MAIN.  Load switches use VR_APU_3V3.
     * AMD Table 26 Group B internal order: VDD_MISC_S5 -> VDD_18_S5 -> VDDIO_33_S5
     * ==================================================================== */

    /* [0] Pre-Group B: verify VR_APU_3V3_PG immediately before asserting
     *     PWR_GROUP_B_EN.  User requirement.  assertEnable=FALSE. */
    PM_RAILS_GRP_B[0].name         = "VR_APU_3V3_PRE_GRP_B";
    PM_RAILS_GRP_B[0].enablePin    = PIN_PWR_GROUP_B_EN; /* unused */
    PM_RAILS_GRP_B[0].assertEnable = FALSE;
    PM_RAILS_GRP_B[0].pgoodPin     = PIN_VR_APU_3V3_PG;
    PM_RAILS_GRP_B[0].rampDelayMs  = 0u;
    PM_RAILS_GRP_B[0].pgTimeoutMs  = 50u;

    /* [1] Assert PWR_GROUP_B_EN; check VDD_MISC_S5 (0.75V, NB706).
     *     AMD Table 26: VDD_MISC_S5 ramps first within Group B. */
    PM_RAILS_GRP_B[1].name         = "VDD_MISC_S5";
    PM_RAILS_GRP_B[1].enablePin    = PIN_PWR_GROUP_B_EN;
    PM_RAILS_GRP_B[1].assertEnable = TRUE;
    PM_RAILS_GRP_B[1].pgoodPin     = PIN_VDD_MISC_PG;
    PM_RAILS_GRP_B[1].rampDelayMs  = 5u;
    PM_RAILS_GRP_B[1].pgTimeoutMs  = 50u;

    /* [2] VDD_12_S5 (1.2V, NB706).  Same enable as [1]. */
    PM_RAILS_GRP_B[2].name         = "VDD_12_S5";
    PM_RAILS_GRP_B[2].enablePin    = PIN_PWR_GROUP_B_EN;
    PM_RAILS_GRP_B[2].assertEnable = FALSE;
    PM_RAILS_GRP_B[2].pgoodPin     = PIN_VDD_1V2_PG;
    PM_RAILS_GRP_B[2].rampDelayMs  = 5u;
    PM_RAILS_GRP_B[2].pgTimeoutMs  = 50u;

    /* [3] VDD_18_S5 (1.8V, NB693A or NB706).  Same enable as [1].
     *     AMD Table 26: VDD_18_S5/VDDIO_AUDIO ramp after VDD_MISC_S5. */
    PM_RAILS_GRP_B[3].name         = "VDD_18_S5";
    PM_RAILS_GRP_B[3].enablePin    = PIN_PWR_GROUP_B_EN;
    PM_RAILS_GRP_B[3].assertEnable = FALSE;
    PM_RAILS_GRP_B[3].pgoodPin     = PIN_VDD_1V8_PG;
    PM_RAILS_GRP_B[3].rampDelayMs  = 5u;
    PM_RAILS_GRP_B[3].pgTimeoutMs  = 50u;

    /* Note: VDDIO_33_S5 (3.3V load switch from VR_APU_3V3) does not have
     * a dedicated PG GPIO.  Its upstream source VR_APU_3V3_PG was verified
     * in EFUSE[0] and GRP_B[0].  AMD Table 26 sequencing (33_S5 last) is
     * guaranteed by the load switch hardware after GROUP_B_EN. */

    /* ====================================================================
     * STAGE 2 — Group C / S3 + S0 memory rails (6 entries)
     * NB695C (VDD_MEMQ, VDDIO_MEM) + NB792 (VDD_MEM) + load switches.
     * AMD §16.1.2: Group B fully stable before Group C > 10%.
     * RSMRST_L is deasserted in PowerManager.c before this stage starts
     * (AMD Table 28 T1: 10ms after S5 rails stable).
     * ==================================================================== */
    PM_RAILS_GRP_C[0].name         = "VDD_MEM_ChA";
    PM_RAILS_GRP_C[0].enablePin    = PIN_PWR_GROUP_C_EN;
    PM_RAILS_GRP_C[0].assertEnable = TRUE;
    PM_RAILS_GRP_C[0].pgoodPin     = PIN_VDD_MEM_CHA_PG;
    PM_RAILS_GRP_C[0].rampDelayMs  = 5u;
    PM_RAILS_GRP_C[0].pgTimeoutMs  = 50u;

    PM_RAILS_GRP_C[1].name         = "VDD_MEM_ChB";
    PM_RAILS_GRP_C[1].enablePin    = PIN_PWR_GROUP_C_EN;
    PM_RAILS_GRP_C[1].assertEnable = FALSE;
    PM_RAILS_GRP_C[1].pgoodPin     = PIN_VDD_MEM_CHB_PG;
    PM_RAILS_GRP_C[1].rampDelayMs  = 5u;
    PM_RAILS_GRP_C[1].pgTimeoutMs  = 50u;

    PM_RAILS_GRP_C[2].name         = "VDDIO_MEM_ChA";
    PM_RAILS_GRP_C[2].enablePin    = PIN_PWR_GROUP_C_EN;
    PM_RAILS_GRP_C[2].assertEnable = FALSE;
    PM_RAILS_GRP_C[2].pgoodPin     = PIN_VDDIO_MEM_CHA_PG;
    PM_RAILS_GRP_C[2].rampDelayMs  = 5u;
    PM_RAILS_GRP_C[2].pgTimeoutMs  = 50u;

    PM_RAILS_GRP_C[3].name         = "VDDIO_MEM_ChB";
    PM_RAILS_GRP_C[3].enablePin    = PIN_PWR_GROUP_C_EN;
    PM_RAILS_GRP_C[3].assertEnable = FALSE;
    PM_RAILS_GRP_C[3].pgoodPin     = PIN_VDDIO_MEM_CHB_PG;
    PM_RAILS_GRP_C[3].rampDelayMs  = 5u;
    PM_RAILS_GRP_C[3].pgTimeoutMs  = 50u;

    PM_RAILS_GRP_C[4].name         = "VDD_MEMQ_ChA";
    PM_RAILS_GRP_C[4].enablePin    = PIN_PWR_GROUP_C_EN;
    PM_RAILS_GRP_C[4].assertEnable = FALSE;
    PM_RAILS_GRP_C[4].pgoodPin     = PIN_VDD_MEMQ_CHA_PG;
    PM_RAILS_GRP_C[4].rampDelayMs  = 5u;
    PM_RAILS_GRP_C[4].pgTimeoutMs  = 50u;

    PM_RAILS_GRP_C[5].name         = "VDD_MEMQ_ChB";
    PM_RAILS_GRP_C[5].enablePin    = PIN_PWR_GROUP_C_EN;
    PM_RAILS_GRP_C[5].assertEnable = FALSE;
    PM_RAILS_GRP_C[5].pgoodPin     = PIN_VDD_MEMQ_CHB_PG;
    PM_RAILS_GRP_C[5].rampDelayMs  = 5u;
    PM_RAILS_GRP_C[5].pgTimeoutMs  = 50u;

    /* ====================================================================
     * STAGE 3 — Group D / S0 VDDCR rails (3 entries)
     * MP2825A (x1-x2) drives MP86979 phases for VDDCR/CCD/SOC.
     * NB792 (2b VID) drives VDDCR_SR.
     * MP2825A bias rail is VR_APU_3V3 — already confirmed in EFUSE[0].
     * AMD §16.1.2: Group C fully stable before Group D > 10%.
     * AMD §16.1.1: Group D stable ≥1ms before PWR_GOOD assertion.
     * ==================================================================== */
    PM_RAILS_GRP_D[0].name         = "MP2825A_1";
    PM_RAILS_GRP_D[0].enablePin    = PIN_PWR_GROUP_D_EN;
    PM_RAILS_GRP_D[0].assertEnable = TRUE;
    PM_RAILS_GRP_D[0].pgoodPin     = PIN_MP2825A_1_PG;
    PM_RAILS_GRP_D[0].rampDelayMs  = 10u;
    PM_RAILS_GRP_D[0].pgTimeoutMs  = 100u;

    PM_RAILS_GRP_D[1].name         = "MP2825A_2";
    PM_RAILS_GRP_D[1].enablePin    = PIN_PWR_GROUP_D_EN;
    PM_RAILS_GRP_D[1].assertEnable = FALSE;
    PM_RAILS_GRP_D[1].pgoodPin     = PIN_MP2825A_2_PG;
    PM_RAILS_GRP_D[1].rampDelayMs  = 10u;
    PM_RAILS_GRP_D[1].pgTimeoutMs  = 100u;

    PM_RAILS_GRP_D[2].name         = "VDDCR";
    PM_RAILS_GRP_D[2].enablePin    = PIN_PWR_GROUP_D_EN;
    PM_RAILS_GRP_D[2].assertEnable = FALSE;
    PM_RAILS_GRP_D[2].pgoodPin     = PIN_VDDCR_PG;
    PM_RAILS_GRP_D[2].rampDelayMs  = 10u;
    PM_RAILS_GRP_D[2].pgTimeoutMs  = 100u;

    uint8 idx = 0u;
    uint8 j;
    for (j = 0u; j < PM_RAIL_GRP_B_COUNT; j++)
    {
        PM_RAILS_ALL_MON[idx] = PM_RAILS_GRP_B[j];
        idx++;
    }
    for (j = 0u; j < PM_RAIL_GRP_C_COUNT; j++)
    {
        PM_RAILS_ALL_MON[idx] = PM_RAILS_GRP_C[j];
        idx++;
    }
    for (j = 0u; j < PM_RAIL_GRP_D_COUNT; j++)
    {
        PM_RAILS_ALL_MON[idx] = PM_RAILS_GRP_D[j];
        idx++;
    }
}
