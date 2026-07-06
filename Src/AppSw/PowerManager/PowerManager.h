/**
 * @file    PowerManager.h
 * @brief   COM-HPC / Strix Halo power sequencing state machine interface.
 */

#ifndef POWER_MANAGER_H
#define POWER_MANAGER_H

#include "Ifx_Types.h"

/**
 * @brief ACPI-aligned power manager states.
 */
typedef enum
{
    PM_STATE_OFF        = 0,    /* all rails off, system unpowered              */
    PM_STATE_RAMP_ALW,          /* enabling EFUSE + pre-checks                  */
    PM_STATE_RAMP_S5,           /* enabling S5 (soft-off) rails                 */
    PM_STATE_RAMP_S3,           /* enabling S3 (suspend-to-RAM) rails           */
    PM_STATE_RAMP_S0,           /* enabling S0 (full power) rails               */
    PM_STATE_ON,                /* all rails up, COM-HPC PWRGD asserted         */
    PM_STATE_S5,                /* THERMTRIP suspend: Group B + EFUSE on,       */
                                /* Group C/D off, SoC held in reset             */
    PM_STATE_DN_S0_S3,          /* ordered power-down: S0 → S3                 */
    PM_STATE_DN_S3_S5,          /* ordered power-down: S3 → S5                 */
    PM_STATE_DN_S5_OFF,         /* ordered power-down: S5 → off                */
    PM_STATE_FAULT,             /* PG loss or sequence timeout — all off        */
} PM_State_t;

/**
 * @brief Initialise the power manager.
 *        All VRM enables must already be deasserted (Port_Init responsibility).
 *        Enters PM_STATE_OFF and waits for a power-on request.
 */
void PowerManager_Init(void);

/**
 * @brief Run one power manager iteration. Call from main loop.
 */
void PowerManager_Run(void);

/**
 * @brief Return the current power manager state.
 */
PM_State_t PowerManager_GetState(void);

/**
 * @brief Request a power-on transition (equivalent to a PWR_BTN press).
 *        Safe to call from any context; the state machine processes it on
 *        the next PowerManager_Run() call.
 */
void PowerManager_RequestPowerOn(void);

/**
 * @brief Request an orderly power-down.
 */
void PowerManager_RequestPowerOff(void);

#endif /* POWER_MANAGER_H */
