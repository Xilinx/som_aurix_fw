/**
 * @file    PwrGood_Mon.h
 * @brief   Power good polling and debounce monitor.
 *
 * PwrGood_Mon watches a set of rails and debounces their PG inputs.
 * It is called on every main loop iteration. When any monitored rail
 * loses power good the registered fault callback is invoked with the
 * index of the failing rail within the set.
 */

#ifndef PWRGOOD_MON_H
#define PWRGOOD_MON_H

#include "PowerManager_Cfg.h"
#include "Ifx_Types.h"

/** Callback invoked when a rail PG is lost. railIdx is the table index. */
typedef void (*PgFaultCb_t)(const PwrRail_Cfg_t *rail, uint8 railIdx);

/**
 * @brief Arm monitoring for a set of rails.
 *        Previous monitoring is cleared and replaced.
 * @param rails     Pointer to rail config array.
 * @param count     Number of rails in the array.
 * @param faultCb   Function called on PG loss.
 */
void PwrGood_MonArm(const PwrRail_Cfg_t *rails, uint8 count, PgFaultCb_t faultCb);

/**
 * @brief Disarm all monitoring (called during power-down or fault recovery).
 */
void PwrGood_MonDisarm(void);

/**
 * @brief Run one monitoring iteration.  Call from the main loop.
 *        Reads each armed rail's PG pin and debounces before reporting fault.
 */
void PwrGood_MonRun(void);

/**
 * @brief Poll until all rails in the set have asserted PG, or timeout.
 * @return TRUE if all PG asserted within timeout, FALSE on timeout.
 */
boolean PwrGood_WaitAllPg(const PwrRail_Cfg_t *rails, uint8 count,
                           uint32 rampDelayMs, uint32 timeoutMs,
                           uint8 *failIdx);

#endif /* PWRGOOD_MON_H */
