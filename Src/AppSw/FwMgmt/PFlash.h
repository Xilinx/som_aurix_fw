/**
 * @file    PFlash.h
 * @brief   PFlash erase / program driver for TC387 SOTA updates
 *
 * Provides sector erase and page-program operations targeting the
 * INACTIVE PFlash bank during an A/B firmware update.  The caller
 * must ensure the target addresses belong to the inactive bank;
 * writing to the active bank while executing from it will fault.
 *
 * Unlike DFlash, PFlash erase and write command sequences MUST
 * execute from PSPR (Program Scratch-Pad RAM) because the DMU
 * locks the PFlash module during the operation.  This driver
 * copies the required iLLD routines into CPU0 PSPR at init time,
 * following the pattern from [CODE-EX] Flash_Programming.c.
 *
 * PFlash on TC387 (10 MB variant):
 *   PF0 : 0xA0000000 – 0xA02FFFFF   3 MB  (Bank A standard)
 *   PF1 : 0xA0300000 – 0xA03FFFFF   1 MB  (Bank A standard)
 *   PF2 : 0xA0400000 – 0xA06FFFFF   3 MB  (Bank B alternate)
 *   PF3 : 0xA0700000 – 0xA07FFFFF   1 MB  (Bank B alternate)
 *   PF4 : 0xA0800000 – 0xA09FFFFF   2 MB  (not swappable)
 *
 *   Sector size  : 16 KB  (16384 bytes)
 *   Page size    : 32 bytes (burst) or 256 bytes (page program)
 *   Erase value  : 0x00
 *
 * This driver uses 256-byte page writes for throughput.  A full
 * 4 MB bank erase + program takes roughly 8–12 seconds.
 *
 * References:
 *   [iLLD]     - IfxFlash.h (eraseMultipleSectors, enterPageMode,
 *                loadPage2X32, writePage, waitUnbusy)
 *   [CODE-EX]  - Flash_Programming.c PSPR relocation pattern
 *   [SOTA-DOC] - NVM operations always use physical addresses
 */

#ifndef PFLASH_H
#define PFLASH_H

#include "Ifx_Types.h"

/* ------------------------------------------------------------------ */
/*  PFlash geometry                                                   */
/* ------------------------------------------------------------------ */
#define PFLASH_SECTOR_SIZE          (16U * 1024U)       /* 16 KB      */
#define PFLASH_HW_PAGE_SIZE         32U    /* smallest programmable unit (writePage)  */
#define PFLASH_BURST_SIZE           256U   /* burst program unit (writeBurst)         */
#define PFLASH_PAGE_SIZE            256U 

/* Physical base addresses — always valid regardless of swap state.
 * NVM erase/write commands use these, not the CPU-executable
 * (segment 8) addresses.                                            */
#define PFLASH_PF0_BASE             0xA0000000U         /* Bank A     */
#define PFLASH_PF1_BASE             0xA0300000U
#define PFLASH_PF2_BASE             0xA0400000U         /* Bank B     */
#define PFLASH_PF3_BASE             0xA0700000U
#define PFLASH_PF4_BASE             0xA0800000U         /* no swap    */

#define PFLASH_BANK_A_BASE          PFLASH_PF0_BASE
#define PFLASH_BANK_A_SIZE          (4U * 1024U * 1024U)  /* 4 MB    */
#define PFLASH_BANK_B_BASE          PFLASH_PF2_BASE
#define PFLASH_BANK_B_SIZE          (4U * 1024U * 1024U)  /* 4 MB    */

#define PFLASH_STATUS_BUSY_MASK   0x00000079u

/* ------------------------------------------------------------------ */
/*  Return codes                                                      */
/* ------------------------------------------------------------------ */
typedef enum
{
    PFLASH_OK             = 0,
    PFLASH_ERR_ALIGN      = 1,  /* address not aligned to page/sector */
    PFLASH_ERR_RANGE      = 2,  /* address outside valid PFlash range */
    PFLASH_ERR_ERASE      = 3,  /* erase command failed              */
    PFLASH_ERR_WRITE      = 4,  /* write command failed              */
    PFLASH_ERR_VERIFY     = 5,  /* read-back mismatch                */
    PFLASH_ERR_NOT_INIT   = 6,  /* Init() not called                 */
    PFLASH_ERR_ACTIVE     = 7,  /* target is the ACTIVE bank         */
} PFlash_Status_t;

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

/**
 * @brief  Initialise the PFlash driver.
 *
 * Copies the iLLD flash command routines into CPU0 PSPR so they
 * can execute while PFlash is busy.  Must be called once before
 * any erase or write operation.
 */
void PFlash_Init(void);

/**
 * @brief  Erase a single 16 KB PFlash sector.
 *
 * @param  sectorAddr  Physical sector-aligned address (must be in
 *                     the inactive bank).
 * @return PFLASH_OK or error code.
 */
PFlash_Status_t PFlash_EraseSector(uint32 sectorAddr);

/**
 * @brief  Erase an entire 4 MB bank (256 sectors).
 *
 * Convenience wrapper that erases every sector in the bank
 * starting at bankBase.  Used before programming a full
 * firmware image.
 *
 * @param  bankBase  PFLASH_BANK_A_BASE or PFLASH_BANK_B_BASE.
 * @return PFLASH_OK or error code (stops on first failure).
 */
PFlash_Status_t PFlash_EraseBank(uint32 bankBase);

/**
 * @brief  Program a 256-byte page to PFlash.
 *
 * The target page must be in erased state (all zeros).
 * Data shorter than 256 bytes is not supported — the caller
 * must provide a full page buffer, zero-padded if needed.
 *
 * @param  pageAddr  Physical 256-byte-aligned address.
 * @param  pData     Pointer to 256 bytes of source data.
 * @return PFLASH_OK or error code.
 */
PFlash_Status_t PFlash_WritePage256(uint32 pageAddr, const uint8 *pData);

/**
 * @brief  Verify a 256-byte page against a RAM buffer.
 *
 * Reads the PFlash page via memory-mapped access and compares
 * against the provided buffer byte-for-byte.
 *
 * @param  pageAddr  Physical 256-byte-aligned address.
 * @param  pExpected Pointer to 256 bytes of expected data.
 * @return PFLASH_OK if match, PFLASH_ERR_VERIFY if mismatch.
 */
PFlash_Status_t PFlash_VerifyPage256(uint32 pageAddr, const uint8 *pExpected);

/**
 * @brief  PFlash Callback 

 */
void PFlash_RegisterKeepAliveCb(void (*cb)(void));

#endif /* PFLASH_H */