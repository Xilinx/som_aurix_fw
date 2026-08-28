/**
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/**
 * @file    Tlf35585.h
 * @brief   Infineon TLF35585 Safety PMIC driver — SPI (QSPI2) + GPIO.
 *
 * Hardware connections (GP_AURIX_Subsystem_PinDefn.xlsx):
 *   QSPI2_M  P15.2  SCS   — chip select (active low)
 *   QSPI2_M  P15.3  SCL   — SPI clock
 *   QSPI2_M  P15.6  SDI   — MOSI (AURIX -> TLF)
 *   QSPI2_M  P15.7  SDO   — MISO (TLF -> AURIX)
 *   GPIO     P33.8  ERR   — PMIC error flag (input)
 *   GPIO     P33.9  SS    — PMIC safe-state flag (input)
 *   GPIO     P33.10 WAKE  — PMIC wake control (input)
 *   GPIO     P33.11 WDI   — Window watchdog trigger (output)
 *
 * The TLF35585 internal watchdog starts counting at PMIC power-on,
 * BEFORE the AURIX application code executes.  Tlf35585_Init() MUST
 * be called as early as possible after Port_Init() and Stm_Init()
 * to prevent a watchdog-induced AURIX reset.
 *
 * References:
 *   Infineon TLF35585 datasheet, Rev 1.1, Chapter 10 (SPI)
 *   Infineon TLF35585 datasheet, Rev 1.1, Chapter 14 (Watchdog)
 *   GP AURIX SW ARD r0.1, PMIC-001 through PMIC-011
 */

#ifndef TLF35585_H
#define TLF35585_H

#include "Ifx_Types.h"

/* ---- TLF35585 SPI frame format ------------------------------------------
 *  32-bit frame: [CMD(1) | ADDR(6) | PARITY(1) | DATA(16) | STATUS(8)]
 *  Write: CMD=1, Read: CMD=0
 *  Parity: even parity over CMD+ADDR+DATA bits
 * ------------------------------------------------------------------------ */

/* Register addresses (6-bit) — subset needed for PMC operation */
#define TLF_REG_DEVCFG0         0x00u   /* Device configuration 0 */
#define TLF_REG_DEVCFG2         0x02u   /* Device configuration 2 */
#define TLF_REG_DEVSTAT         0x06u   /* Device status */
#define TLF_REG_PROTREG         0x09u   /* Protection register (unlock writes) */
#define TLF_REG_SYSPCFG0        0x04u   /* System power config 0 */
#define TLF_REG_WDCFG0          0x0Cu   /* Watchdog configuration 0 */
#define TLF_REG_WDCFG1          0x0Du   /* Watchdog configuration 1 */
#define TLF_REG_WWDSCMD         0x10u   /* Window watchdog service command */
#define TLF_REG_FWDCFG          0x11u   /* Functional watchdog config */
#define TLF_REG_FWDSTAT0        0x12u   /* Functional watchdog status 0 */
#define TLF_REG_RSYSPCFG0       0x19u   /* Read-back system power config */
#define TLF_REG_IF              0x3Eu   /* Interrupt flags */
#define TLF_REG_DEVCTRL         0x35u   /* Device control (state transitions) */
#define TLF_REG_DEVCFG_RB       0x34u   /* Device config read-back */

/* ---- Additional Register Addresses -------------------------------------- */
#define TLF_REG_SYSPCFG1        0x05u   /* System power config 1 */
#define TLF_REG_WWDCFG0         0x0Eu   /* Window WD closed window config */
#define TLF_REG_WWDCFG1         0x0Fu   /* Window WD open window config */
#define TLF_REG_WWDSTAT         0x13u   /* Window WD status (error counter) */
#define TLF_REG_DEVCTRLN        0x36u   /* Device control inverted */
#define TLF_REG_SYSSF           0x1Cu   /* System safety flags */
#define TLF_REG_MONSF1          0x1Du   /* Monitor safety flags 1 */
#define TLF_REG_MONSF2          0x1Eu   /* Monitor safety flags 2 */
#define TLF_REG_INITERR         0x1Fu   /* Init error flags */

/* ---- WDCFG0 Bit Definitions (addr 0x0C) -------------------------------- */
#define TLF_WDCFG0_WWDEN       (1u << 3u)
#define TLF_WDCFG0_FWDEN       (1u << 2u)
#define TLF_WDCFG0_WWDTSEL     (1u << 1u)  /* 1=SPI trigger */
#define TLF_WDCFG0_WDCYC       (1u << 0u)  /* 0=0.1ms, 1=1ms base */

/* Protection register unlock keys */
#define TLF_UNLOCK_KEY0         0xABCDu
#define TLF_UNLOCK_KEY1         0xDCBAu
#define TLF_UNLOCK_KEY2         0xCDA9u
#define TLF_UNLOCK_KEY3         0x9AC5u


/* DEVCTRL state transition commands */
#define TLF_GOTO_NORMAL         0x0001u
#define TLF_GOTO_STANDBY        0x0009u
#define TLF_GOTO_SLEEP          0x000Du

#define TLF_SYSPCFG1_ERREN     (1u << 3u)  /* Enable ERR input monitor */

/* Watchdog service response seeds (see datasheet Ch 14.4)
 * The TLF35585 expects a specific response based on the question
 * register value.  The response is: seed XOR question. */

/* ---- Return codes ------------------------------------------------------- */
typedef enum
{
    TLF_OK       = 0,
    TLF_ERR_SPI  = 1,   /* SPI transfer failure */
    TLF_ERR_ID   = 2,   /* unexpected device ID */
    TLF_ERR_WDT  = 3    /* watchdog service failure */
} Tlf35585_Status_t;

/* ---- Public API --------------------------------------------------------- */

/**
 * @brief  Initialise QSPI2 for TLF35585 communication and configure the PMIC.
 *
 * Sequence:
 *   1. Configure QSPI2 master (1 MHz, CPOL=0, CPHA=1, 32-bit frame)
 *   2. Read DEVSTAT to verify device presence
 *   3. Unlock protection registers
 *   4. Configure window watchdog period (longest window for boot margin)
 *   5. Service the watchdog once immediately
 *   6. Transition PMIC to NORMAL state if not already there
 *
 * @return TLF_OK on success, error code otherwise.
 */
Tlf35585_Status_t Tlf35585_Init(void);

/**
 * @brief  Service the TLF35585 watchdog.
 *
 * Must be called periodically from the main loop.  Performs both:
 *   1. SPI window-watchdog service (WWDSCMD register write)
 *   2. GPIO WDI pin toggle (P33.11) for the hardware watchdog path
 *
 * The watchdog window is configured during init.  This function
 * tracks its own timing via STM and only sends the SPI service
 * command when inside the open window.  Safe to call every loop
 * iteration — early calls are no-ops.
 */
void Tlf35585_ServiceWdt(void);

/**
 * @brief  Read a TLF35585 register over SPI.
 * @param  addr   6-bit register address
 * @param  data   pointer to store the 16-bit register value
 * @return TLF_OK on success
 */
Tlf35585_Status_t Tlf35585_ReadReg(uint8 addr, uint16 *data);

/**
 * @brief  Write a TLF35585 register over SPI.
 * @param  addr   6-bit register address
 * @param  data   16-bit value to write
 * @return TLF_OK on success
 */
Tlf35585_Status_t Tlf35585_WriteReg(uint8 addr, uint16 data);

/**
 * @brief  Check if the PMIC ERR pin (P33.8) is asserted.
 * @return TRUE if ERR is active (fault condition)
 */
boolean Tlf35585_IsErrActive(void);

/**
 * @brief  Check if the PMIC Safe State pin (P33.9) is asserted.
 * @return TRUE if PMIC is in safe state
 */
boolean Tlf35585_IsSafeStateActive(void);

#endif /* TLF35585_H */