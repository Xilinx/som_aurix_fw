/**
 * @file    Stm_Timer.h
 * @brief   STM-based microsecond / millisecond timer for CPU0.
 *
 * Uses STM0 (System Timer Module 0) which is always-on and clocked from
 * the SRI clock. Provides a monotonic 64-bit tick counter and blocking
 * delay helpers used by the power manager and I2C timeout logic.
 */

#ifndef STM_TIMER_H
#define STM_TIMER_H

#include "Ifx_Types.h"

/**
 * @brief Initialise STM0 for use by CPU0.
 *        Must be called after clock initialisation.
 */
void Stm_Init(void);

/**
 * @brief Return the current time in microseconds (wraps after ~584,542 years).
 */
uint64 Stm_GetTimeMicros(void);

/**
 * @brief Return the current time in milliseconds.
 */
uint32 Stm_GetTimeMs(void);

/**
 * @brief Blocking delay. Spins for at least the requested number of milliseconds.
 *        Do not call from an interrupt context.
 */
void Stm_DelayMs(uint32 ms);

/**
 * @brief Blocking delay in microseconds.
 */
void Stm_DelayUs(uint32 us);

/**
 * @brief Return TRUE if the given number of milliseconds has elapsed since
 *        *pTimestamp. On first call pass 0 in *pTimestamp.
 *        Updates *pTimestamp when the timeout fires so it can be reused as a
 *        periodic interval timer.
 */
boolean Stm_IsElapsedMs(uint32 *pTimestamp, uint32 periodMs);

#endif /* STM_TIMER_H */
