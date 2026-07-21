/**
 * @file    Platform_PinCfg.c
 * @brief   AppPin_t constant definitions for all board GPIO signals.
 *
 * Source: GP_AURIX_Subsystem_PinDefn.xlsx, sheet AURIX_IO.
 *
 * Initialised with {portIdx, pinIdx} integer constants only — no hardware
 * register pointers — so Tasking ctc accepts these as constant expressions
 * without E306.
 */

#include "Platform_PinCfg.h"

#if defined(TARGET_EVAL_BOARD)

/* ---- Eval Board Pin Map -------------------------------------------------
 * KIT-A2G-TC387-5V-TRB
 * Power enables mapped to onboard LEDs for visual state machine feedback.
 * Power good inputs mapped to header pins — use jumpers to VCC to simulate.
 * ----------------------------------------------------------------------- */

/* Power Enables (LEDs — directly observable) */
const AppPin_t PIN_MAIN_12V_EFUSE_EN    = {20u, 11u};  /* P20.11 LED0 */
const AppPin_t PIN_PWR_GROUP_B_EN       = {20u, 12u};  /* P20.12 LED1 */
const AppPin_t PIN_PWR_GROUP_C_EN       = {20u, 13u};  /* P20.13 LED2 */
const AppPin_t PIN_PWR_GROUP_D_EN       = {20u, 14u};  /* P20.14 LED3 */

/* FuSa Status (standby LEDs) */
const AppPin_t PIN_FUSA_STATUS0         = {33u,  4u};  /* P33.4 */
const AppPin_t PIN_FUSA_STATUS1         = {33u,  5u};  /* P33.5 */

/* APU Control Outputs (standby LEDs + headers) */
const AppPin_t PIN_APU_PWR_GOOD         = {33u,  6u};  /* P33.6 */
const AppPin_t PIN_COLD_RST             = {33u,  7u};  /* P33.7 */
const AppPin_t PIN_RSMRST_L             = {10u,  1u};  /* P10.1 header */
const AppPin_t PIN_WARM_RST             = {10u,  2u};  /* P10.2 header */

/* Simulated Power Good Inputs (headers — jumper to VCC) */
const AppPin_t PIN_VR_APU_3V3_PG        = { 0u,  0u};  /* P00.0 */
const AppPin_t PIN_GROUP_B_PG           = { 0u,  1u};  /* P00.1 */
const AppPin_t PIN_GROUP_C_PG           = { 2u,  0u};  /* P02.0 */
const AppPin_t PIN_GROUP_D_PG           = { 2u,  1u};  /* P02.1 */

/* Control Inputs (headers — buttons/switches) */
const AppPin_t PIN_CB_PWRBTN_L          = { 2u,  4u};  /* P02.4 */
const AppPin_t PIN_VIN_PWR_OK           = { 0u,  2u};  /* P00.2 — tie HIGH */

/* THERMTRIP (ERU capable) */
const AppPin_t PIN_THERMTRIP_L          = {10u,  5u};  /* P10.5 header */

#else /* TARGET_GP_SOM */

/* ==========================================================================
 * APU Power Control — Enable Outputs
 * ========================================================================== */
const AppPin_t PIN_MAIN_12V_EFUSE_EN  = {0u,  0u};
const AppPin_t PIN_PWR_GROUP_B_EN     = {0u,  3u};
const AppPin_t PIN_PWR_GROUP_C_EN     = {0u,  4u};
const AppPin_t PIN_PWR_GROUP_D_EN     = {0u,  5u};

/* ==========================================================================
 * APU Power Control — Power Good Inputs (Port 0)
 * ========================================================================== */
const AppPin_t PIN_MAIN_12V_EFUSE_PG  = {0u,  1u};
const AppPin_t PIN_VR_APU_3V3_PG      = {0u,  2u};
const AppPin_t PIN_VDD_MISC_PG        = {0u,  6u};
const AppPin_t PIN_VDD_1V2_PG         = {0u,  7u};
const AppPin_t PIN_VDD_1V8_PG         = {0u,  8u};
const AppPin_t PIN_VDD_MEMQ_CHA_PG    = {0u,  9u};
const AppPin_t PIN_VDD_MEMQ_CHB_PG    = {0u, 10u};
const AppPin_t PIN_VDDIO_MEM_CHA_PG   = {0u, 11u};
const AppPin_t PIN_VDDIO_MEM_CHB_PG   = {0u, 12u};

/* ==========================================================================
 * APU Power Control — Power Good Inputs (Port 1)
 * ========================================================================== */
const AppPin_t PIN_VDD_MEM_CHA_PG     = {1u,  3u};
const AppPin_t PIN_VDD_MEM_CHB_PG     = {1u,  4u};
const AppPin_t PIN_MP2825A_1_PG       = {1u,  5u};
const AppPin_t PIN_MP2825A_2_PG       = {1u,  6u};
const AppPin_t PIN_VDDCR_PG           = {1u,  7u};

/* ==========================================================================
 * COM-HPC FuSa Outputs (Port 2)
 * ========================================================================== */
const AppPin_t PIN_FUSA_SPI_ALERT     = {2u,  0u};
const AppPin_t PIN_FUSA_ALERT_L       = {2u,  1u};
const AppPin_t PIN_FUSA_VOLTAGE_ERR_L = {2u,  2u};
const AppPin_t PIN_FUSA_STATUS0       = {2u,  8u};
const AppPin_t PIN_FUSA_STATUS1       = {2u,  9u};
const AppPin_t PIN_PROCHOT_L          = {2u, 10u};
const AppPin_t PIN_CATERR_L           = {2u, 11u};

/* ==========================================================================
 * Fan Control (Port 10)
 * ========================================================================== */
const AppPin_t PIN_FAN_TACHIN         = {10u,  0u};
const AppPin_t PIN_FAN_PWM            = {10u,  1u};

/* ==========================================================================
 * Thermal / USB PD Alert (Port 10)
 * ========================================================================== */
const AppPin_t PIN_CARRIER_HOT        = {10u,  2u};
const AppPin_t PIN_THERMTRIP_L        = {10u,  3u};
const AppPin_t PIN_USBC_PD_ALERT_L    = {10u,  7u};
const AppPin_t PIN_USBC_PD_INT_TO_APU = {10u,  8u};

/* ==========================================================================
 * APU APML Interface (Port 11)
 * ========================================================================== */
const AppPin_t PIN_APU_PROCHOT_L      = {11u,  9u};
const AppPin_t PIN_APML_ALERT         = {11u, 10u};

/* ==========================================================================
 * COM-HPC USB / HPD (Port 13)
 * ========================================================================== */
const AppPin_t PIN_DP2_HPD            = {13u,  0u};
const AppPin_t PIN_DP3_HPD            = {13u,  3u};

/* ==========================================================================
 * MCU Debug UART (Port 14)
 * ========================================================================== */
const AppPin_t PIN_DEBUG_TXD          = {14u,  0u};
const AppPin_t PIN_DEBUG_RXD          = {14u,  1u};
const AppPin_t PIN_UART_MUX_SEL       = {14u,  6u};

/* ==========================================================================
 * APU State Control (Port 15)
 * ========================================================================== */
const AppPin_t PIN_APU_RESET_IN_L     = {15u,  5u};

/* ==========================================================================
 * COM-HPC General Management (Port 20)
 * ========================================================================== */
const AppPin_t PIN_SLEEP_L            = {20u,  6u};
const AppPin_t PIN_RSMRST_OUT_L       = {20u,  7u};
const AppPin_t PIN_WD_OUT             = {20u,  8u};
const AppPin_t PIN_WD_STROBE_L        = {20u,  9u};
const AppPin_t PIN_RAPID_SD           = {20u, 10u};
const AppPin_t PIN_LID_L              = {20u, 11u};
const AppPin_t PIN_TAMPER_L           = {20u, 12u};

/* ==========================================================================
 * Boot Select / BIOS ROM (Port 22)
 * ========================================================================== */
const AppPin_t PIN_BSEL_0             = {22u,  0u};
const AppPin_t PIN_BSEL_1             = {22u,  1u};
const AppPin_t PIN_BSEL_2             = {22u,  2u};
const AppPin_t PIN_APU_ROM_SPI_SEL    = {22u,  8u};

/* ==========================================================================
 * COM-HPC Carrier-Side Inputs (Port 33)
 * ========================================================================== */
const AppPin_t PIN_CB_PWRBTN_L        = {33u,  0u};
const AppPin_t PIN_VIN_PWR_OK         = {33u,  1u};
const AppPin_t PIN_CB_RSTBTN_L        = {33u,  2u};
const AppPin_t PIN_CB_AC_PRESENT      = {33u,  3u};
const AppPin_t PIN_CB_BATLOW_L        = {33u,  4u};

/* ==========================================================================
 * APU State Control Signals (Port 33)
 * ========================================================================== */
const AppPin_t PIN_SLP_S3             = {33u,  5u};
const AppPin_t PIN_SLP_S5             = {33u,  6u};
const AppPin_t PIN_APU_PWR_GOOD       = {33u,  7u};
const AppPin_t PIN_APU_PWROK          = {33u, 12u};
const AppPin_t PIN_APU_PCC_L          = {33u, 13u};
const AppPin_t PIN_WARM_RST           = {33u, 14u};
const AppPin_t PIN_COLD_RST           = {33u, 15u};

/* ==========================================================================
 * TLF35585 PMIC FuSa GPIO (Port 33)
 * ========================================================================== */
const AppPin_t PIN_TLF_ERR            = {33u,  8u};
const AppPin_t PIN_TLF_SS             = {33u,  9u};
const AppPin_t PIN_TLF_WAKE           = {33u, 10u};
const AppPin_t PIN_TLF_WDI            = {33u, 11u};

/* ==========================================================================
 * APU State Control (Port 34)
 * ========================================================================== */
const AppPin_t PIN_MMC_RSMRST_L       = {34u,  1u};
const AppPin_t PIN_PLTRST_L           = {34u,  2u};
const AppPin_t PIN_APU_PWRBTN         = {34u,  3u};
const AppPin_t PIN_BLINK              = {34u,  4u};

const AppPin_t PIN_TESTMODE           = {20u, 2u};

#endif