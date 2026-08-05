/**
 * @file    PowerManager_Cfg.h
 * @brief   Power rail table for the Strix Halo COM-HPC module.
 *
 * Source: GP_AURIX_Subsystem_PinDefn.xlsx
 *
 * The hardware uses GROUP enables (GROUP_B_EN, GROUP_C_EN, GROUP_D_EN) rather
 * than individual per-rail enables. Each group enable brings up multiple VRMs
 * simultaneously; the TC387 then monitors each individual rail's PG input.
 *
 * Sequencing order (each stage waits for all PGs before proceeding):
 *
 *   Stage 0 — 12V EFUSE (MAIN_12V_EFUSE_EN -> MAIN_12V_EFUSE_PG)
 *   Stage 1 — Group B   (PWR_GROUP_B_EN -> VR_APU_3V3, VDD_MISC, VDD_1V2, VDD_1V8)
 *   Stage 2 — Group C   (PWR_GROUP_C_EN -> VDD_MEM_ChA/B, VDDIO_MEM_ChA/B, VDD_MEMQ_ChA/B)
 *   Stage 3 — Group D   (PWR_GROUP_D_EN -> VDDCR via MP2825A)
 *
 * Note: Within each group, PG inputs are checked individually after the group
 * enable is asserted. The group enable is a single GPIO output; the individual
 * PG inputs report per-rail status.
 *
 * Adjust rampDelayMs and pgTimeoutMs to match VRM datasheet startup times.
 */

#ifndef POWER_MANAGER_CFG_H
#define POWER_MANAGER_CFG_H

#include "Platform_PinCfg.h"
#include "Platform_Cfg.h"
#include "AppPin.h"

/* --------------------------------------------------------------------------
 * PwrRail_Cfg_t
 * Uses AppPin_t (uint8 portIdx, uint8 pinIdx) instead of IfxPort_Pin to
 * avoid Tasking E272/E306/E333 caused by IfxPort_Pin's register pointer.
 * -------------------------------------------------------------------------- */
typedef struct
{
    const char *name;
    AppPin_t    enablePin;      /* GPIO output — VRM / group enable */
    boolean     assertEnable;   /* TRUE = assert enablePin for this entry */
    AppPin_t    pgoodPin;       /* GPIO input  — per-rail power good */
    uint32      rampDelayMs;    /* wait after enable before PG check */
    uint32      pgTimeoutMs;    /* max time for PG to assert */
} PwrRail_Cfg_t;

#define PM_RAIL_VR3V3_COUNT     1u
#define PM_PG_TIMEOUT_MS    5u
#define PM_PWRBTN_HOLD_MS   4000u   /* 4s ACPI force-off convention */

extern PwrRail_Cfg_t PM_RAILS_VR3V3[PM_RAIL_VR3V3_COUNT];


/*
 * Rail tables — defined and populated in PowerManager_Cfg.c.
 * NOT declared static const: Tasking ctc E306 rejects IfxPort_Pin members
 * (hardware register pointers) in any file-scope aggregate initialiser.
 * Tables are filled at runtime by PowerManager_CfgInit().
 *
 */
#define PM_RAIL_EFUSE_COUNT     2u
extern PwrRail_Cfg_t PM_RAILS_EFUSE[PM_RAIL_EFUSE_COUNT];

/* --------------------------------------------------------------------------
 * Stage 1 — Group B  (S5 rails: MISC, 1V2, 1V8 outputs from NB706 etc.)
 * Entry [0] = VR_APU_3V3 pre-Group B check (assertEnable=FALSE) per user
 * requirement: verify 3V3 stable before asserting PWR_GROUP_B_EN.
 * Entry [1] = GROUP_B_EN assert + VDD_MISC_S5 PG (first NB706 output).
 * -------------------------------------------------------------------------- */
#define PM_RAIL_GRP_B_COUNT     4u
extern PwrRail_Cfg_t PM_RAILS_GRP_B[PM_RAIL_GRP_B_COUNT];

/* --------------------------------------------------------------------------
 * Stage 2 — Group C  (memory rails: VDD_MEM, VDDIO_MEM, VDD_MEMQ)
 * -------------------------------------------------------------------------- */
#define PM_RAIL_GRP_C_COUNT     6u
extern PwrRail_Cfg_t PM_RAILS_GRP_C[PM_RAIL_GRP_C_COUNT];

/* --------------------------------------------------------------------------
 * Stage 3 — Group D  (core voltage: VDDCR via MP2825A)
 * -------------------------------------------------------------------------- */
#define PM_RAIL_GRP_D_COUNT     3u
extern PwrRail_Cfg_t PM_RAILS_GRP_D[PM_RAIL_GRP_D_COUNT];

/* Convenience count for total managed rail entries across all stages. */
#define PM_TOTAL_RAIL_COUNT     (PM_RAIL_EFUSE_COUNT  + \
                                 PM_RAIL_GRP_B_COUNT  + \
                                 PM_RAIL_GRP_C_COUNT  + \
                                 PM_RAIL_GRP_D_COUNT)

#define PM_RAIL_ALL_MON_COUNT   (PM_RAIL_GRP_B_COUNT + PM_RAIL_GRP_C_COUNT + PM_RAIL_GRP_D_COUNT)

extern PwrRail_Cfg_t PM_RAILS_ALL_MON[PM_RAIL_ALL_MON_COUNT];

/**
 * @brief Populate all rail tables at runtime.
 *        Must be called once at the start of PowerManager_Init().
 */
void PowerManager_CfgInit(void);

#endif /* POWER_MANAGER_CFG_H */
