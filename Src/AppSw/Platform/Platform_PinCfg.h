/**
 * @file    Platform_PinCfg.h
 * @brief   TC387 GPIO pin assignments — all signals from GP_AURIX_Subsystem_PinDefn.xlsx.
 *
 * Pins are declared as extern const AppPin_t objects (defined in Platform_PinCfg.c).
 * Using AppPin_t {uint8 portIdx, uint8 pinIdx} instead of IfxPort_Pin avoids:
 *   - Tasking E306 (IfxPort_Pin contains hardware register pointer — not a constant expr)
 *   - Tasking E272 (IfxPort_Pxx_y are extern variables, not macros, in Tasking iLLD)
 *   - Tasking E333 (type mismatches from the above)
 */

#ifndef PLATFORM_PINCFG_H
#define PLATFORM_PINCFG_H

#include "AppPin.h"

/* ==========================================================================
 * APU Power Control — Enable Outputs (Port 0)
 * ========================================================================== */
extern const AppPin_t PIN_MAIN_12V_EFUSE_EN;   /* P0.0  GPIO Out */
extern const AppPin_t PIN_PWR_GROUP_B_EN;       /* P0.3  GPIO Out */
extern const AppPin_t PIN_PWR_GROUP_C_EN;       /* P0.4  GPIO Out */
extern const AppPin_t PIN_PWR_GROUP_D_EN;       /* P0.5  GPIO Out */

/* ==========================================================================
 * APU Power Control — Power Good Inputs (Port 0)
 * ========================================================================== */
extern const AppPin_t PIN_MAIN_12V_EFUSE_PG;   /* P0.1  GPIO In */
extern const AppPin_t PIN_VR_APU_3V3_PG;        /* P0.2  GPIO In */
extern const AppPin_t PIN_VDD_MISC_PG;          /* P0.6  GPIO In */
extern const AppPin_t PIN_VDD_1V2_PG;           /* P0.7  GPIO In */
extern const AppPin_t PIN_VDD_1V8_PG;           /* P0.8  GPIO In */
extern const AppPin_t PIN_VDD_MEMQ_CHA_PG;      /* P0.9  GPIO In */
extern const AppPin_t PIN_VDD_MEMQ_CHB_PG;      /* P0.10 GPIO In */
extern const AppPin_t PIN_VDDIO_MEM_CHA_PG;     /* P0.11 GPIO In */
extern const AppPin_t PIN_VDDIO_MEM_CHB_PG;     /* P0.12 GPIO In */

/* ==========================================================================
 * APU Power Control — Power Good Inputs (Port 1)
 * ========================================================================== */
extern const AppPin_t PIN_VDD_MEM_CHA_PG;       /* P1.3  GPIO In */
extern const AppPin_t PIN_VDD_MEM_CHB_PG;       /* P1.4  GPIO In */
extern const AppPin_t PIN_MP2825A_1_PG;         /* P1.5  GPIO In */
extern const AppPin_t PIN_MP2825A_2_PG;         /* P1.6  GPIO In */
extern const AppPin_t PIN_VDDCR_PG;             /* P1.7  GPIO In */

/* ==========================================================================
 * COM-HPC FuSa Outputs (Port 2)
 * ========================================================================== */
extern const AppPin_t PIN_FUSA_SPI_ALERT;       /* P2.0  GPIO Out */
extern const AppPin_t PIN_FUSA_ALERT_L;         /* P2.1  GPIO Out */
extern const AppPin_t PIN_FUSA_VOLTAGE_ERR_L;   /* P2.2  GPIO Out */
extern const AppPin_t PIN_FUSA_STATUS0;         /* P2.8  GPIO Out */
extern const AppPin_t PIN_FUSA_STATUS1;         /* P2.9  GPIO Out */
extern const AppPin_t PIN_PROCHOT_L;            /* P2.10 GPIO Out */
extern const AppPin_t PIN_CATERR_L;             /* P2.11 GPIO Out */

/* ==========================================================================
 * Fan Control (Port 10)
 * ========================================================================== */
extern const AppPin_t PIN_FAN_TACHIN;           /* P10.0 GPIO In  */
extern const AppPin_t PIN_FAN_PWM;              /* P10.1 GPIO Out */

/* ==========================================================================
 * Thermal / USB PD Alert (Port 10 — ERU inputs)
 * ========================================================================== */
extern const AppPin_t PIN_CARRIER_HOT;          /* P10.2 ERUIN2 */
extern const AppPin_t PIN_THERMTRIP_L;          /* P10.3 ERUIN3 */
extern const AppPin_t PIN_USBC_PD_ALERT_L;      /* P10.7 ERUIN0 — wired-OR from CYPDs */
extern const AppPin_t PIN_USBC_PD_INT_TO_APU;   /* P10.8 GPIO Out — forwards PD event to APU */


/* ==========================================================================
 * APU APML Interface (Port 11)
 * ========================================================================== */
extern const AppPin_t PIN_APU_PROCHOT_L;        /* P11.9  GPIO Out */
extern const AppPin_t PIN_APML_ALERT;           /* P11.10 ERUIN6  */

/* ==========================================================================
 * COM-HPC USB / USBC PD HPD (Port 13)
 * ========================================================================== */
extern const AppPin_t PIN_DP2_HPD;              /* P13.0 GPIO Out */
extern const AppPin_t PIN_DP3_HPD;              /* P13.3 GPIO Out */

/* ==========================================================================
 * MCU Debug UART (Port 14)
 * ========================================================================== */
extern const AppPin_t PIN_DEBUG_TXD;            /* P14.0 ASCLIN0 TX */
extern const AppPin_t PIN_DEBUG_RXD;            /* P14.1 ASCLIN0 RX */
extern const AppPin_t PIN_UART_MUX_SEL;         /* P14.6 GPIO Out   */

/* ==========================================================================
 * APU State Control (Port 15)
 * ========================================================================== */
extern const AppPin_t PIN_APU_RESET_IN_L;       /* P15.5 ERUIN4 — Input from APU */

/* ==========================================================================
 * COM-HPC General Management (Port 20)
 * ========================================================================== */
extern const AppPin_t PIN_SLEEP_L;              /* P20.6  GPIO In  */
extern const AppPin_t PIN_RSMRST_OUT_L;         /* P20.7  GPIO Out */
extern const AppPin_t PIN_WD_OUT;               /* P20.8  GPIO Out */
extern const AppPin_t PIN_WD_STROBE_L;          /* P20.9  ERUIN7   */
extern const AppPin_t PIN_RAPID_SD;             /* P20.10 GPIO In  */
extern const AppPin_t PIN_LID_L;                /* P20.11 GPIO In  */
extern const AppPin_t PIN_TAMPER_L;             /* P20.12 GPIO In  */

/* ==========================================================================
 * COM-HPC Boot Select / APU UART / BIOS ROM (Port 22)
 * ========================================================================== */
extern const AppPin_t PIN_BSEL_0;               /* P22.0  GPIO In */
extern const AppPin_t PIN_BSEL_1;               /* P22.1  GPIO In */
extern const AppPin_t PIN_BSEL_2;               /* P22.2  GPIO In */
extern const AppPin_t PIN_APU_ROM_SPI_SEL;      /* P22.8  GPIO Out */

/* ==========================================================================
 * COM-HPC Carrier-Side Inputs (Port 33)
 * ========================================================================== */
extern const AppPin_t PIN_CB_PWRBTN_L;          /* P33.0  GPIO In  */
extern const AppPin_t PIN_VIN_PWR_OK;           /* P33.1  GPIO In  */
extern const AppPin_t PIN_CB_RSTBTN_L;          /* P33.2  GPIO In  */
extern const AppPin_t PIN_CB_AC_PRESENT;        /* P33.3  GPIO In  */
extern const AppPin_t PIN_CB_BATLOW_L;          /* P33.4  GPIO In  */

/* ==========================================================================
 * APU State Control Signals (Port 33)
 * ========================================================================== */
extern const AppPin_t PIN_SLP_S3;               /* P33.5  GPIO In  — active HIGH */
extern const AppPin_t PIN_SLP_S5;               /* P33.6  GPIO In  — active HIGH */
extern const AppPin_t PIN_APU_PWR_GOOD;         /* P33.7  GPIO Out */
extern const AppPin_t PIN_APU_PWROK;            /* P33.12 GPIO In  */
extern const AppPin_t PIN_APU_PCC_L;            /* P33.13 GPIO In  */
extern const AppPin_t PIN_WARM_RST;             /* P33.14 GPIO Out */
extern const AppPin_t PIN_COLD_RST;             /* P33.15 GPIO Out */

/* ==========================================================================
 * TLF35585 PMIC FuSa GPIO (Port 33)
 * ========================================================================== */
extern const AppPin_t PIN_TLF_ERR;              /* P33.8  GPIO Bidir */
extern const AppPin_t PIN_TLF_SS;               /* P33.9  GPIO Bidir */
extern const AppPin_t PIN_TLF_WAKE;             /* P33.10 GPIO Bidir */
extern const AppPin_t PIN_TLF_WDI;              /* P33.11 GPIO Out   */
extern const AppPin_t PIN_TESTMODE;            /* P20.2 — reserved */     
/* ==========================================================================
 * APU State Control (Port 34)
 * ========================================================================== */
extern const AppPin_t PIN_MMC_RSMRST_L;         /* P34.1 GPIO Out */
extern const AppPin_t PIN_PLTRST_L;             /* P34.2 GPIO Out */
extern const AppPin_t PIN_APU_PWRBTN;           /* P34.3 GPIO Out */
extern const AppPin_t PIN_BLINK;                /* P34.4 GPIO Out */

/* ==========================================================================
 * Convenience aliases
 * ========================================================================== */
#define PIN_PWRBTN_L            PIN_CB_PWRBTN_L
#define PIN_SLP_S3_ACTIVE       PIN_SLP_S3
#define PIN_SLP_S5_ACTIVE       PIN_SLP_S5
#define PIN_COMHPC_PWRGD        PIN_APU_PWR_GOOD
#define PIN_APU_RESET_OUT_L     PIN_COLD_RST
#define PIN_CYPD_ALERT_L        PIN_USBC_PD_ALERT_L
#define PIN_CYPD0_INT_L         PIN_USBC_PD_ALERT_L
#define PIN_CYPD1_INT_L         PIN_USBC_PD_ALERT_L
#define PIN_CYPD0_RESET_L       PIN_PLTRST_L
#define PIN_CYPD1_RESET_L       PIN_PLTRST_L

/* ==========================================================================
 * Peripheral-mode pins (configured by driver, not GPIO — listed for reference)
 * --------------------------------------------------------------------------
 * ASCLIN0  P14.0/P14.1   MCU Debug UART (TX/RX)
 * ASCLIN4  P22.5/P22.6   APU sideband UART (RXD/TXD)
 * I2C0     P13.1/P13.2   USBC PD I2C (SCL/SDA)
 * I2C1     P11.13/P11.14 APML I2C (SDA/SCL)
 * QSPI0    P22.7/9/10/11 BIOS ROM SPI (CLK/MISO/MOSI/CS)
 * QSPI2    P15.2/3/6/7   TLF35585 PMIC SPI (SCS/SCL/SDI/SDO)
 * QSPI3    P2.4/5/6/7    FuSa SPI Slave (CS/MISO/MOSI/CLK)
 * ========================================================================== */

/* ==========================================================================
 * ERU Input Channel Assignments
 * --------------------------------------------------------------------------
 * ERUIN0   P10.7   USBC_PD_ALERT#
 * ERUIN1   P10.8   USBC_PD_INT
 * ERUIN2   P10.2   CARRIER_HOT
 * ERUIN3   P10.3   THERMTRIP#
 * ERUIN4   P15.5   APU_RESET_L
 * ERUIN6   P11.10  APML_ALERT
 * ERUIN7   P20.9   WD_STROBE#
 * ========================================================================== */


 
#endif /* PLATFORM_PINCFG_H */
