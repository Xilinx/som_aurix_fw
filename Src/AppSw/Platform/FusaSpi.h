/**
 * @file    FusaSpi.h
 * @brief   COM-HPC FUSA_SPI register map interface
 *
 * Implements PMC-FUSA-003: AURIX as SPI slave on the COM-HPC FUSA_SPI
 * bus.  A carrier-board safety controller (SPI master) reads status
 * registers to monitor the SoM.
 *
 * Hardware: QSPI3 in slave mode
 *   SCLK : P2.7  (SCLKA input)
 *   MOSI : P2.6  (MTSRA input)
 *   MISO : P2.5  (MRST output)
 *   CS#  : P2.4  (SLSIA input)
 *   ALERT: P2.0  (GPIO output, active low)
 *
 * Protocol:
 *   32-bit SPI frames, MSB first.
 *   Transaction 1: Master sends [ADDR:8 | 0x000000:24]
 *   Transaction 2: Slave responds with [DATA:32] for previous ADDR
 *   (Standard "address-then-read" SPI register protocol)
 *
 * The register map is a flat array of uint32 values updated
 * periodically by FusaSpi_Update() from the main loop.
 *
 * Build-time enable: FUSA_SPI_FEATURE_ENABLE (default 1)
 */

#ifndef FUSASPI_H
#define FUSASPI_H

#include "Ifx_Types.h"

/* ================================================================== */
/*  Build-time enable                                                 */
/* ================================================================== */
#ifndef FUSA_SPI_FEATURE_ENABLE
#define FUSA_SPI_FEATURE_ENABLE     1u
#endif

/* ================================================================== */
/*  Register map offsets (byte address = offset × 4)                  */
/*                                                                    */
/*  Master sends the register index (0x00–0x5F) in bits [31:24]      */
/*  of the first 32-bit frame.  Slave responds with the register      */
/*  value on the next frame.                                          */
/* ================================================================== */

/* ---- Identity & version (0x00–0x03) ---- */
#define FUSA_REG_MAGIC              0x00u   /* 0x46555341 "FUSA" */
#define FUSA_REG_MAP_VERSION        0x01u   /* Register map version (major.minor.patch packed) */
#define FUSA_REG_FW_VERSION         0x02u   /* Firmware version */
#define FUSA_REG_BUILD_CONFIG       0x03u   /* Build-time feature flags */

/* ---- Platform state (0x04–0x0B) ---- */
#define FUSA_REG_POWER_STATE        0x04u   /* Current PM_State_t */
#define FUSA_REG_LAST_RESET_CAUSE   0x05u   /* PM_ResetCause_t */
#define FUSA_REG_FUSA_STATUS        0x06u   /* 2-bit FUSA_STATUS + extended flags */
#define FUSA_REG_UPTIME_S           0x07u   /* Seconds since boot */
#define FUSA_REG_BOOT_COUNT         0x08u   /* NvLog boot counter */
#define FUSA_REG_RETRY_COUNT        0x09u   /* Current fault retry count */
#define FUSA_REG_PM_FLAGS           0x0Au   /* PM signal states (VIN_PWR_OK, SLP_S3, etc.) */
#define FUSA_REG_RESERVED_0B        0x0Bu

/* ---- Thermal / APML (0x0C–0x0F) ---- */
#define FUSA_REG_APU_TEMP_C         0x0Cu   /* APU die temperature (signed int32, °C) */
#define FUSA_REG_PROCHOT_STATUS     0x0Du   /* PROCHOT active flags */
#define FUSA_REG_THERMAL_STATE      0x0Eu   /* Thermal throttle state */
#define FUSA_REG_THERMTRIP_COUNT    0x0Fu   /* THERMTRIP event count this session */

/* ---- TLF PMIC (0x10–0x17) ---- */
#define FUSA_REG_TLF_DEVSTAT        0x10u   /* TLF device status register */
#define FUSA_REG_TLF_SYSSF          0x11u   /* TLF system status flags */
#define FUSA_REG_TLF_WDSTAT         0x12u   /* TLF watchdog status / error counter */
#define FUSA_REG_TLF_STATE          0x13u   /* TLF state (INIT/NORMAL/etc) */
#define FUSA_REG_TLF_WDT_SVC       0x14u   /* TLF WDT service count */
#define FUSA_REG_TLF_WDT_MISS      0x15u   /* TLF WDT miss count */
#define FUSA_REG_TLF_ERR_PIN        0x16u   /* TLF ERR pin state */
#define FUSA_REG_TLF_SS_PIN         0x17u   /* TLF safe-state pin state */

/* ---- Voltage monitoring (0x20–0x3F) ---- */
/* Channels 0–23: millivolt values from VoltMon */
#define FUSA_REG_VOLT_CH_BASE       0x20u   /* VOLT_CH0 = 0x20, CH1 = 0x21, ... CH23 = 0x37 */
#define FUSA_REG_VOLT_UV_FLAGS      0x38u   /* Undervoltage fault bitmask (1 bit per channel) */
#define FUSA_REG_VOLT_OV_FLAGS      0x39u   /* Overvoltage fault bitmask */
#define FUSA_REG_VOLT_WARN_FLAGS    0x3Au   /* Warning-level bitmask */
#define FUSA_REG_VOLT_TIMESTAMP     0x3Bu   /* Timestamp of last voltage scan (ms) */

/* ---- USB PD summary (0x40–0x43) ---- */
#define FUSA_REG_USBPD_PORT0_STATE  0x40u   /* Port 0: attach/role/alt-mode packed */
#define FUSA_REG_USBPD_PORT1_STATE  0x41u   /* Port 1: attach/role/alt-mode packed */
#define FUSA_REG_USBPD_FAULT_FLAGS  0x42u   /* USB PD fault flags */
#define FUSA_REG_USBPD_HPD_STATE    0x43u   /* HPD pin states */

/* ---- NV log summary (0x48–0x4B) ---- */
#define FUSA_REG_NVLOG_SLOT         0x48u   /* Active NV log slot */
#define FUSA_REG_NVLOG_EVENTS       0x49u   /* Events in active slot */
#define FUSA_REG_NVLOG_ERRORS       0x4Au   /* NV log flush errors */
#define FUSA_REG_NVLOG_LAST_EVT     0x4Bu   /* Last event type logged */

/* ---- COM-HPC misc (0x50–0x53) ---- */
#define FUSA_REG_COMHPC_WDT_STATE   0x50u   /* COM-HPC watchdog state */
#define FUSA_REG_RAPID_SHUTDOWN     0x51u   /* RAPID_SHUTDOWN pin state */
#define FUSA_REG_LID_STATE          0x52u   /* LID# pin state */
#define FUSA_REG_TAMPER_STATE       0x53u   /* TAMPER# pin state */

/* Total register count */
#define FUSA_REG_COUNT              0x60u   /* 96 registers × 4 bytes = 384 bytes */

/* ================================================================== */
/*  Build config flag bits (FUSA_REG_BUILD_CONFIG)                    */
/* ================================================================== */
#define FUSA_CFG_TLF_ENABLED        (1u << 0)
#define FUSA_CFG_COMHPC_WDT         (1u << 1)
#define FUSA_CFG_FUSA_SPI           (1u << 2)
#define FUSA_CFG_FUSA_GPIO          (1u << 3)
#define FUSA_CFG_VOLTMON            (1u << 4)
#define FUSA_CFG_USBPD              (1u << 5)
#define FUSA_CFG_SOTA               (1u << 6)
#define FUSA_CFG_SYSMON             (1u << 7)
#define FUSA_CFG_EVAL_BOARD         (1u << 8)
#define FUSA_CFG_FWD_ENABLE         (1u << 9)

/* ================================================================== */
/*  Register map version                                              */
/* ================================================================== */
#define FUSA_MAP_VERSION_MAJOR      1u
#define FUSA_MAP_VERSION_MINOR      0u
#define FUSA_MAP_VERSION_PATCH      0u
#define FUSA_MAP_VERSION_PACKED     ((FUSA_MAP_VERSION_MAJOR << 16) | \
                                    (FUSA_MAP_VERSION_MINOR << 8)  | \
                                     FUSA_MAP_VERSION_PATCH)

/* ================================================================== */
/*  Magic values                                                      */
/* ================================================================== */
#define FUSA_MAGIC                  0x46555341u     /* "FUSA" */
#define FUSA_FW_VERSION             0x00020000u     /* v0.2.0 */

/* ================================================================== */
/*  Configuration                                                     */
/* ================================================================== */

/** Update interval for populating the register map from live data */
#ifndef FUSA_SPI_UPDATE_INTERVAL_MS
#define FUSA_SPI_UPDATE_INTERVAL_MS 100u
#endif

/* SPI ISR priorities (must not conflict with QSPI2/TLF) */
#define QSPI3_TX_ISR_PRIO          40u
#define QSPI3_RX_ISR_PRIO          41u
#define QSPI3_ER_ISR_PRIO          42u

/* ================================================================== */
/*  Public API                                                        */
/* ================================================================== */

/**
 * @brief  Initialise the FUSA_SPI slave interface.
 *
 * Configures QSPI3 in slave mode, sets up ISRs, and populates
 * the register map with initial values.  Primes the first
 * exchange buffer so the slave is ready for the master's first
 * transaction.
 */
void FusaSpi_Init(void);

/**
 * @brief  Periodic update — populate registers from live system data.
 *
 * Call from the main loop.  Reads PowerManager state, VoltMon
 * channels, TLF status, thermal state, and NV log stats into
 * the register map.  Non-blocking.
 */
void FusaSpi_Update(void);

/**
 * @brief  Assert the FUSA_ALERT# pin (active low).
 *
 * Called when a safety-relevant event requires carrier notification.
 */
void FusaSpi_AssertAlert(void);

/**
 * @brief  Deassert the FUSA_ALERT# pin.
 */
void FusaSpi_DeassertAlert(void);

/**
 * @brief  Get a pointer to the register map for debug inspection.
 */
const uint32 *FusaSpi_GetRegMap(void);

/**
 * @brief  Print the full register map to the debug UART.
 */
void FusaSpi_DumpRegMap(void);

#endif /* FUSASPI_H */