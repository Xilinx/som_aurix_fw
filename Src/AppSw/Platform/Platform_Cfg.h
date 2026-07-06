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

/* AMD 58241 §16.1.5 Table 28 T1: S5 rails stable → RSMRST_L rising, min 10ms */
#define PM_RSMRST_DELAY_AFTER_S5_MS    10u

/* AMD 58241 §16.1.5 Table 30 T7: PWR_GOOD rising → RESET_L rising, min 28.5ms.
 * RESET_L must remain asserted for this period AFTER PWR_GOOD is asserted. */
#define PM_RESET_HOLD_AFTER_PWRGD_MS   30u

/* ---- CYPD6129 I2C addresses (7-bit, set by ADDR pin strapping) ---------- */
#define CYPD_PORT0_I2C_ADDR             0x08u
#define CYPD_PORT1_I2C_ADDR             0x40u

/* ---- CYPD6129 initialisation timing ------------------------------------- */
#define CYPD_RESET_PULSE_MS             10u   /* RESET_L low pulse width             */
#define CYPD_BOOT_DELAY_MS              100u  /* wait after reset before HPI access  */
#define CYPD_CMD_TIMEOUT_MS             50u   /* max time for a command to complete  */

/* ---- USB PD manager poll interval --------------------------------------- */
#define USBPD_MGR_POLL_INTERVAL_MS      5u

/* ---- Debug UART --------------------------------------------------------- */
/* Baud rate is defined in Uart_Debug.h */

/* ---- Number of power rails ---------------------------------------------- */
/* Must match the table length in PowerManager_Cfg.h */
#define PM_RAIL_ALW_COUNT               3u
#define PM_RAIL_S5_COUNT                2u
#define PM_RAIL_S3_COUNT                2u
#define PM_RAIL_S0_COUNT                4u

#endif /* PLATFORM_CFG_H */
