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
 * @file    SysMonitor.h
 * @brief   Platform signal monitoring and default drive interface.
 *
 * Responsibilities:
 *   - APU_PROCHOT_L (P11.9): driven HIGH by default (open-drain output).
 *     Future: APML interface will assert LOW to throttle APU.
 *     Hardware note: pin must be open-drain so the APU can independently
 *     pull it LOW to signal a thermal event without bus contention.
 *
 *   - PROCHOT# (P2.10, PIN_PROCHOT_L): COM-HPC carrier output, active low.
 *     Driven HIGH by default.  Driven LOW automatically when APU_PROCHOT_L
 *     is observed LOW (APU or TC387 asserting the shared open-drain line).
 *
 *   - CATERR# (P2.11, PIN_CATERR_L): COM-HPC carrier output, active low.
 *     Driven HIGH by default.  Future: assert LOW on catastrophic APU fault.
 */

#ifndef SYS_MONITOR_H
#define SYS_MONITOR_H

#include "Ifx_Types.h"


#define SYSMON_POLL_INTERVAL_MS     5u   /* main-loop poll rate for PROCHOT# */
#define SYSMON_LOG_INTERVAL_MS      1000u /* re-log PROCHOT assertion once/sec */

#define SYSMON_THERMAL_POLL_MS      100u

/* Temperature thresholds in degrees C — update from AMD thermal spec */
#define SYSMON_WARNING_TEMP_C       85      /* assert PROCHOT above this     */
#define SYSMON_WARNING_HYST_C       75      /* release PROCHOT below this    */
#define SYSMON_SHUTDOWN_TEMP_C      105
#define SYSMON_TEMP_INVALID         (-128)
#define SYSMON_APML_PROCHOT_ENABLE  0u


#define SBTSI_I2C_ADDR_7BIT     0x4Cu
#define SBTSI_REG_CPU_TEMP_INT  0x01u
#define SBTSI_REG_CPU_TEMP_DEC  0x10u

#define SYSMON_I2C_FAIL_LIMIT   5u

#define SYSMON_PROCHOT_CLEAR_POLLS   3u

/**
 * @brief Initialise SysMonitor.
 *        Sets APU_PROCHOT_L, PROCHOT#, and CATERR# to their default
 *        (deasserted) states.  Call once after Port_Init().
 */
void SysMonitor_Init(void);

/**
 * @brief Run one SysMonitor iteration.  Call from main loop on every pass.
 *        Reads APU_PROCHOT_L and propagates its state to the carrier PROCHOT#.
 */
void SysMonitor_Run(void);

/**
 * @brief Assert APU_PROCHOT_L LOW (drive APU PROCHOT from TC387 side).
 *        Used in future by APML-based thermal management.
 */
void SysMonitor_AssertApuProchot(void);

/**
 * @brief Deassert APU_PROCHOT_L HIGH (release open-drain drive).
 */
void SysMonitor_DeassertApuProchot(void);

/**
 * @brief Assert CATERR# LOW (signal catastrophic error to carrier).
 */
void SysMonitor_AssertCaterr(void);

/**
 * @brief Deassert CATERR# HIGH (clear catastrophic error signal).
 */
void SysMonitor_DeassertCaterr(void);

typedef void (*SysMonitor_ShutdownCb_t)(void);
void SysMonitor_RegisterShutdownCb(SysMonitor_ShutdownCb_t cb);

#define SYSMON_CARRIER_HOT_ENABLE   0u

#if (SYSMON_CARRIER_HOT_ENABLE == 1u)
#define SYSMON_CARRIER_DWELL_MS     1000u
#endif

/**
 * @brief Returns TRUE if PROCHOT is currently asserted
 *        (thermal throttle or carrier hot).
 */
boolean SysMonitor_IsThrottling(void);

#endif /* SYS_MONITOR_H */
