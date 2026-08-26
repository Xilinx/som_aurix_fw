/**
 * @file    PowerManager.h
 * @brief   COM-HPC / Strix Halo power sequencing state machine interface.
 */

#ifndef POWER_MANAGER_H
#define POWER_MANAGER_H

#include "Ifx_Types.h"
#include "VoltMon.h"
#include "Platform_Cfg.h"


#if (PM_PWRBTN_DEBOUNCE_POLLS < 1u)
#error "PM_PWRBTN_DEBOUNCE_POLLS must be >= 1"
#endif
#if (PM_RSTBTN_DEBOUNCE_POLLS < 1u)
#error "PM_RSTBTN_DEBOUNCE_POLLS must be >= 1"
#endif

/**
 * @brief ACPI-aligned power manager states.
 */
typedef enum
{
    PM_STATE_OFF        = 0,    /* all rails off, system unpowered              */
    PM_STATE_POWER_UP,
    PM_STATE_RAMP_ALW,          /* enabling EFUSE + pre-checks                  */
    PM_STATE_RAMP_VR3V3,
    PM_STATE_RAMP_S5,           /* enabling S5 (soft-off) rails                 */
    PM_STATE_RAMP_S3,           /* enabling S3 (suspend-to-RAM) rails           */
    PM_STATE_RAMP_S0,           /* enabling S0 (full power) rails               */
    PM_STATE_ON,                /* all rails up, COM-HPC PWRGD asserted         */
    PM_STATE_S5,                /* THERMTRIP suspend: Group B + EFUSE on,       */
                                /* Group C/D off, SoC held in reset             */
    PM_STATE_DN_S0_S3,          /* ordered power-down: S0 -> S3                 */
    PM_STATE_DN_S3_S5,          /* ordered power-down: S3 -> S5                 */
    PM_STATE_DN_S5_OFF,         /* ordered power-down: S5 -> off                */
    PM_STATE_FAULT,             /* PG loss or sequence timeout — all off        */
    PM_STATE_WARM_RESET 
} PM_State_t;

typedef enum
{
    PM_RESET_CAUSE_NONE         = 0u,
    PM_RESET_CAUSE_PG_TIMEOUT   = 1u,   /* PGOOD not asserted in time */
    PM_RESET_CAUSE_PG_LOSS      = 2u,   /* PGOOD lost during operation */
    PM_RESET_CAUSE_THERMAL      = 3u,   /* THERMTRIP# assertion */
    PM_RESET_CAUSE_WATCHDOG     = 4u,   /* COM-HPC host watchdog timeout */
    PM_RESET_CAUSE_HOST_REQUEST = 5u,   /* SLP_S5 or PWRBTN shutdown */
    PM_RESET_CAUSE_VOLTAGE      = 6u,   /* EVADC UV/OV fault */
    PM_RESET_CAUSE_BIOS_FAIL    = 7u,    /* BIOS ROM validation failure */
    PM_RESET_CAUSE_COLD_RST     = 8u   /* CF9 cold reset — auto-restart after dwell */
} PM_ResetCause_t;

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
 * @brief  Get the cause of the most recent fault/reset.
 */
PM_ResetCause_t PowerManager_GetResetCause(void);

/**
 * @brief  Get the number of retry attempts since last successful boot.
 */
uint8 PowerManager_GetRetryCount(void);

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

/**
 * @brief Request an orderly power-down.
 */
void PowerManager_OnThermtripIsr(void);

/* Voltage fault callback for VoltMon.
 * Called from main-loop context by VoltMon_Scan(). */
void PowerManager_OnVoltageFault(const VoltMon_ChCfg_t *ch,  uint16 measuredMv, VoltMon_Severity_t severity);

/**
 * @brief  Voltage fault callback for VoltMon.
 *
 * Called from main-loop context by VoltMon_Scan() when a channel
 * crosses its UV/OV fault threshold.  Initiates a fault response
 * based on severity.
 *
 * @param  ch          Pointer to the faulting channel's configuration.
 * @param  measuredMv  The measured voltage in millivolts.
 * @param  severity    Fault severity level.
 */
void PowerManager_OnVoltageFault(const VoltMon_ChCfg_t *ch,
                                 uint16 measuredMv,
                                 VoltMon_Severity_t severity);

/**
 * @brief  Request a warm reset of the APU.
 *
 */
void PowerManager_RequestWarmReset(void);

/**
 * @brief  Request a cold reboot of the APU.
 *
 */
void PowerManager_RequestColdReset(void);

/**
 * @brief  Request an immediate forced power off.
 *
 */
void PowerManager_RequestForcedOff(void);

/**
 * @brief  Clear a latched fault and return to PM_STATE_OFF.
 *
 */
void PowerManager_ClearFault(void);

#endif /* POWER_MANAGER_H */
