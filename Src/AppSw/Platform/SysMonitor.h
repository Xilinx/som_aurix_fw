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
