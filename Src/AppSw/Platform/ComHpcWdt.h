/**
 * @file    ComHpcWdt.h
 * @brief   COM-HPC host watchdog — Mode 1 (WD_OUT + PLTRST#).
 *
 * The host periodically strobes WD_STROBE# (P20.9) to prove liveness.
 * If the strobe stops, the watchdog fires:
 *   1. WD_OUT (P20.8) driven HIGH
 *   2. PLTRST# (P34.2) driven LOW (carrier reset)
 *   3. Module remains in this state until platform reset
 *
 * An enable delay (1s–600s) is applied after ComHpcWdt_Enable() before
 * the watchdog begins monitoring.  This gives the host time to boot
 * and start its strobe loop.
 *
 * The ERU callback for WD_STROBE# (ERU_CB_WD_STROBE) feeds into
 * ComHpcWdt_OnStrobeIsr() which resets the countdown.
 *
 * Hardware:
 *   WD_STROBE#  P20.9  ERUIN7 — host heartbeat input (rising edge)
 *   WD_OUT      P20.8  GPIO Out — watchdog timeout indicator
 *   PLTRST#     P34.2  GPIO Out — carrier platform reset
 */

#ifndef COMHPCWDT_H
#define COMHPCWDT_H

#include "Ifx_Types.h"

/* Default enable delay in seconds — host must begin strobing within
 * this window after ComHpcWdt_Enable() is called. */
#define COMHPC_WDT_DEFAULT_ENABLE_DELAY_S   60u

/* Default timeout in milliseconds — if no strobe arrives within this
 * period after the enable delay expires, the watchdog fires. */
#define COMHPC_WDT_DEFAULT_TIMEOUT_MS       1000u

/* Limits per COM-HPC spec */
#define COMHPC_WDT_ENABLE_DELAY_MIN_S       1u
#define COMHPC_WDT_ENABLE_DELAY_MAX_S       600u

/**
 * @brief  Initialise the watchdog module.  Does NOT start monitoring.
 *         Call ComHpcWdt_Enable() to begin the enable delay countdown.
 */
void ComHpcWdt_Init(void);

/**
 * @brief  Start the enable delay countdown.
 *         After enableDelayS seconds, the watchdog begins monitoring
 *         for WD_STROBE# edges.  Call this when the system enters
 *         PM_STATE_ON (x86 is booting and will eventually start strobing).
 *
 * @param  enableDelayS  Seconds before monitoring starts (1–600).
 * @param  timeoutMs     Strobe timeout in ms once monitoring is active.
 */
void ComHpcWdt_Enable(uint16 enableDelayS, uint32 timeoutMs);

/**
 * @brief  Disable the watchdog.  Deasserts WD_OUT if it was asserted.
 *         Call on shutdown or when entering S5/OFF.
 */
void ComHpcWdt_Disable(void);

/**
 * @brief  ISR-safe strobe callback.  Resets the timeout countdown.
 *         Register this as ERU_CB_WD_STROBE via Eru_RegisterCallback().
 */
void ComHpcWdt_OnStrobeIsr(void);

/**
 * @brief  Periodic tick — call from main loop.
 *         Checks enable delay expiry and strobe timeout.
 */
void ComHpcWdt_Run(void);

/**
 * @brief  Returns TRUE if the watchdog has timed out (WD_OUT asserted).
 */
boolean ComHpcWdt_HasFired(void);

#endif /* COMHPCWDT_H */