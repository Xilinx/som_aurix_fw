/**
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/**
 * @file    Platform_Cfg.h
 * @brief   Board-level platform configuration constants.
 *
 * Central place for timing parameters, I2C addresses, and feature switches.
 * Hardware pin assignments live in Platform_PinCfg.h.
 */

#ifndef PLATFORM_CFG_H
#define PLATFORM_CFG_H

/* ---- Power Manager timing (milliseconds) -------------------------------- */
#define PM_RAIL_DEFAULT_RAMP_DELAY_MS   5u    /* min wait after EN before PG check */
#define PM_RAIL_DEFAULT_PG_TIMEOUT_MS   50u   /* max wait for PG to assert          */
#define PM_PG_DEBOUNCE_POLLS            3u    /* consecutive polls for PG stable     */
#define PM_INTER_RAIL_DELAY_MS          2u    /* gap between enabling successive rails */
#define PM_PWRGD_DEGLITCH_MS            5u    /* AMD 58241 §16.1.1: all rails stable ≥1ms before PWR_GOOD */

/* AMD 58241 §16.1.5 Table 28 T1: S5 rails stable -> RSMRST_L rising, min 10ms */
#define PM_RSMRST_DELAY_AFTER_S5_MS    10u

/* AMD 58241 §16.1.5 Table 30 T7: PWR_GOOD rising -> RESET_L rising, min 28.5ms.
 * RESET_L must remain asserted for this period AFTER PWR_GOOD is asserted. */
#define PM_RESET_HOLD_AFTER_PWRGD_MS   30u

/* ---- CYPD6129 I2C addresses (7-bit, set by ADDR pin strapping) ----------
 * Per GP_RoboticsCarrier_ARD strap configuration. Kept as two distinct
 * build-time constants (not derived from one another) since the two ports
 * are independently strapped and can be re-strapped independently on a
 * future carrier revision. */
#define CYPD_PORT0_I2C_ADDR             0x40u
#define CYPD_PORT1_I2C_ADDR             0x42u

#if (CYPD_PORT0_I2C_ADDR == CYPD_PORT1_I2C_ADDR)
#error "CYPD_PORT0_I2C_ADDR and CYPD_PORT1_I2C_ADDR must be distinct"
#endif

/* ---- CYPD6129 initialisation timing ------------------------------------- */
#define CYPD_RESET_PULSE_MS             10u   /* RESET_L low pulse width             */
#define CYPD_BOOT_DELAY_MS              100u  /* wait after reset before HPI access  */
#define CYPD_CMD_TIMEOUT_MS             50u   /* max time for a command to complete  */

/* ---- USB PD manager poll interval --------------------------------------- */
#define USBPD_MGR_POLL_INTERVAL_MS      5u

/* ---- Main loop pacing ----------------------------------------------------
 * Fixed target period for core0_main()'s for(;;) loop. Matches the existing
 * 5ms poll rate assumed by SYSMON_POLL_INTERVAL_MS / USBPD_MGR_POLL_INTERVAL_MS
 * so debounce-poll and dwell-poll counters map to a known real-time value. */
#define MAIN_LOOP_PERIOD_MS             5u

/* Minimum gap between repeated "loop overrun" log lines. Without this,
 * an overrun that persists reprints every iteration, which costs UART
 * time and makes the overrun worse. */
#define MAIN_LOOP_OVERRUN_LOG_INTERVAL_MS 1000u

/* ---- Debug UART --------------------------------------------------------- */
/* Baud rate is defined in Uart_Debug.h */

/* ---- Number of power rails ---------------------------------------------- */
/* Must match the table length in PowerManager_Cfg.h */
#define PM_RAIL_ALW_COUNT               3u
#define PM_RAIL_S5_COUNT                2u
#define PM_RAIL_S3_COUNT                2u
#define PM_RAIL_S0_COUNT                4u

#define PM_MAX_RETRIES              30u      /* attempts before latch-off, N retries */
#define PM_RETRY_DELAY_MS           500u 

/* ---- FuSa feature-set gate -----------------------------------------------
 * Default OFF. Guards enabling the COM-HPC watchdog (ComHpcWdt_Enable),
 * both on cold boot (PM_STATE_RAMP_S0) and on re-arm after a warm reset
 * (PM_STATE_WARM_RESET), pending FuSa sign-off. Gated symmetrically so the
 * watchdog posture doesn't depend on which reset path was taken. */
#define FUSA_FEATURE_ENABLE   0u

#if defined(TARGET_EVAL_BOARD) && defined(TARGET_GP_SOM)
#error "Cannot define both TARGET_EVAL_BOARD and TARGET_GP_SOM"
#endif

#if !defined(TARGET_EVAL_BOARD) && !defined(TARGET_GP_SOM)
#error "No target defined - build with BOARD=eval or BOARD=som"
#endif

#endif /* PLATFORM_CFG_H */
