/**
 * @file    VoltMon.h
 * @brief   EVADC-based voltage monitoring for power rail health.
 *
 * Periodically scans EVADC channels mapped to power rail sense dividers,
 * converts raw ADC counts to millivolts, compares against configurable
 * UV/OV thresholds, and reports faults via callback.
 *
 * EVADC groups from GP_AURIX_Subsystem_PinDefn.xlsx:
 *   Group 0  AN0-AN3   VDDCR, VDDCR_CCD, VDDCR_SOC, VDDCR_SR  (VID, S0)
 *   Group 1  AN8-AN10  VDD_MEM_CHA, VDD_MEMQ_CHA, VDDIO_MEM_CHA (S0)
 *   Group 2  AN16-AN18 VDD_MEM_CHB, VDD_MEMQ_CHB, VDDIO_MEM_CHB (S0)
 *   Group 3  AN24-AN29 VDD_MISC, VDD_1V2, VDD_1V8 + S5 variants (S5/S0)
 *   Group 4  AN40-AN43 VDDIO_3V3, VDDIO_AUDIO, VDDIO_MEM_VAA   (S5/S0)
 *
 * iLLD: IfxEvadc_Adc.h
 */

#ifndef VOLTMON_H
#define VOLTMON_H

#include "Ifx_Types.h"

/* Maximum number of monitored channels across all groups */
#define VOLTMON_MAX_CHANNELS    24u

/* ADC reference: 5.0V external precision reference per ARD (AURIX ADC
 * external reference is a discrete voltage device that is 5.0V) */
#define VOLTMON_VREF_MV         5000u
#define VOLTMON_ADC_MAX         4095u    /* 12-bit resolution */

/* Fault severity levels — matches FuSa requirement escalation chain */
typedef enum
{
    VOLTMON_OK          = 0u,
    VOLTMON_WARNING     = 1u,   /* threshold crossed, log only */
    VOLTMON_FAULT       = 2u,   /* confirmed UV/OV, notify APU */
    VOLTMON_CRITICAL    = 3u    /* emergency, disable rail */
} VoltMon_Severity_t;

/* Per-channel configuration */
typedef struct
{
    const char         *name;           /* Rail name for logging */
    uint8               evadcGroup;     /* EVADC group index (0-4) */
    uint8               evadcChannel;   /* Channel within group (0-15) */
    uint8               resultReg;      /* Result register index */
    uint16              nominalMv;      /* Nominal voltage in mV */
    uint16              uvWarnMv;       /* Undervoltage warning threshold */
    uint16              uvFaultMv;      /* Undervoltage fault threshold */
    uint16              ovWarnMv;       /* Overvoltage warning threshold */
    uint16              ovFaultMv;      /* Overvoltage fault threshold */
    uint16              dividerScale;   /* Resistor divider scale x1000
                                         * (e.g. 2:1 divider = 2000) */
} VoltMon_ChCfg_t;

/* Callback for voltage fault notification */
typedef void (*VoltMon_FaultCb_t)(const VoltMon_ChCfg_t *ch,
                                  uint16 measuredMv,
                                  VoltMon_Severity_t severity);

/**
 * @brief  Initialise the EVADC module, groups, and channels.
 *
 * Call after Port_Init() and Stm_Init().  The EVADC is configured for
 * software-triggered queue scan mode — conversions are started
 * explicitly by VoltMon_Scan().
 */
void VoltMon_Init(void);

/**
 * @brief  Register a callback for voltage fault events.
 */
void VoltMon_RegisterFaultCb(VoltMon_FaultCb_t cb);

/**
 * @brief  Trigger a scan of all configured channels and check thresholds.
 *
 * Call periodically from the main loop (e.g. every 10-100ms).
 * Triggers queue conversion, waits for results, reads each channel,
 * applies divider scaling, and checks against UV/OV thresholds.
 */
void VoltMon_Scan(void);

/**
 * @brief  Get the last measured voltage for a channel (in mV).
 * @param  chIdx  Index into the channel configuration table
 * @return Voltage in millivolts, or 0 if not yet scanned
 */
uint16 VoltMon_GetLastMv(uint8 chIdx);

/**
 * @brief  Get the number of configured monitoring channels.
 */
uint8 VoltMon_GetChannelCount(void);

#endif /* VOLTMON_H */