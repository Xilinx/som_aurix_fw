/**
 * @file    Ifx_Cfg.h
 * @brief   Top-level iLLD configuration for the TC387 COM-HPC controller.
 *
 * This file is included by iLLD headers via the IFX_CFG_H inclusion guard.
 * Set MCU variant, oscillator frequency, and any iLLD feature switches here.
 */

#ifndef IFX_CFG_H
#define IFX_CFG_H

/* ---- MCU variant -------------------------------------------------------- */
#ifndef IFX_CFG_TC3XX_DEVICE
#define IFX_CFG_TC3XX_DEVICE        IFX_CFG_TC38XA
#endif /* IFX_CFG_TC3XX_DEVICE */

#ifndef DEVICE_TC38X
#define DEVICE_TC38X    1
#endif

#ifndef IFX_PIN_PACKAGE_516
#if defined(TARGET_EVAL_BOARD)
#define IFX_PIN_PACKAGE_LFBGA292    1
#else
#define IFX_PIN_PACKAGE_516    1
#endif
#endif

#ifndef IFX_CFG_SSW_ENABLE_TRICORE0
#define IFX_CFG_SSW_ENABLE_TRICORE0   (1U)
#endif
#ifndef IFX_CFG_SSW_ENABLE_TRICORE1
#define IFX_CFG_SSW_ENABLE_TRICORE1   (1U)
#endif
#ifndef IFX_CFG_SSW_ENABLE_TRICORE2
#define IFX_CFG_SSW_ENABLE_TRICORE2   (1U)
#endif
#ifndef IFX_CFG_SSW_ENABLE_TRICORE3
#define IFX_CFG_SSW_ENABLE_TRICORE3   (1U)
#endif

/* ---- External oscillator ------------------------------------------------ */
/* Verify against the board crystal / XTAL specification. */
#define IFX_CFG_SCU_XTAL_FREQUENCY  20000000u   /* 20 MHz board crystal     */

/* ---- CPU clock target --------------------------------------------------- */
#define IFX_CFG_SCU_PLL_FREQUENCY   300000000u  /* 300 MHz fCPU             */

/* ---- iLLD feature enables ----------------------------------------------- */
#define IFX_USE_SW_MANAGED_INT      0
#define IFX_CFG_USE_COMMUNITY       0

/* ---- Watchdog ------------------------------------------------------------ */
/* Watchdog is disabled in Cpu0_Main during development.
 * Set to 1 to enable iLLD watchdog servicing if required. */
#define IFX_CFG_SCU_ENABLE_WATCHDOG 0

#endif /* IFX_CFG_H */
