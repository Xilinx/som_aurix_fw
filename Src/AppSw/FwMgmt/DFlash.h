/**
 * @file    DFlash.h
 * @brief   DFlash0 read/write/erase driver for TC387
 *
 * Provides a simple abstraction over iLLD DMU flash commands for
 * DFlash0 (EEPROM emulation) memory.  Used by the SOTA boot
 * validation, NV logging, and carrier-card configuration subsystems.
 *
 * DFlash0 on TC38x:
 *   - Base address   : 0xAF000000
 *   - Total size     : 256 KB  (single-ended mode, default)
 *   - Sector size    : 4 KB
 *   - Page size      : 8 bytes  (smallest programmable unit)
 *   - Erase value    : 0x00  (TriCore NVM erase state is all-zeros)
 *
 * DFlash1 is reserved for future HSM use and is NOT touched by this
 * driver (per FW-MGMT-07).
 *
 * References:
 *   [iLLD]    - IfxFlash.h API
 *   [CODE-EX] - Flash_Programming.c  (iLLD_TC3XX_ADS_Verify_PFlash_Erase)
 *   [SWAP-REGS] - UCB lives in DFlash0 UCB region; this driver avoids it
 */

#ifndef DFLASH_H
#define DFLASH_H

#include "Ifx_Types.h"

/* ------------------------------------------------------------------ */
/*  DFlash0 physical layout                                           */
/* ------------------------------------------------------------------ */
#define DFLASH0_BASE                0xAF000000U
#define DFLASH0_SIZE                (256U * 1024U)          /* 256 KB  */
#define DFLASH0_SECTOR_SIZE         (4U * 1024U)            /* 4 KB    */
#define DFLASH0_PAGE_SIZE           8U                      /* 8 bytes */
#define DFLASH0_END                 (DFLASH0_BASE + DFLASH0_SIZE)

/* ------------------------------------------------------------------ */
/*  DFlash0 memory map — user data region                             */
/*                                                                    */
/*  The UCB region (0xAF400000+) is separate from this data area.     */
/*  All offsets below are relative to DFLASH0_BASE.                   */
/*                                                                    */
/*  FW-MGMT-08 : SOTA metadata          4 KB  (1 sector)             */
/*  FW-MGMT-09 : Crash log buffers    128 KB  (32 sectors, 4x32KB)   */
/*  FW-MGMT-10 : Carrier USB config    16 KB  (4 sectors)            */
/*             : Reserved              108 KB                         */
/*                                    ------                          */
/*                            Total   256 KB                          */
/* ------------------------------------------------------------------ */

/* SOTA metadata — boot counter, pending-update flag, image CRC */
#define DFLASH_SOTA_OFFSET          0x00000000U
#define DFLASH_SOTA_SIZE            DFLASH0_SECTOR_SIZE             /* 4 KB  */
#define DFLASH_SOTA_ADDR            (DFLASH0_BASE + DFLASH_SOTA_OFFSET)

/* Crash / emergency-shutdown log — 4 x 32 KB rotating buffers */
#define DFLASH_CRASHLOG_OFFSET      (DFLASH_SOTA_OFFSET + DFLASH_SOTA_SIZE)
#define DFLASH_CRASHLOG_SIZE        (128U * 1024U)                  /* 128 KB */
#define DFLASH_CRASHLOG_ADDR        (DFLASH0_BASE + DFLASH_CRASHLOG_OFFSET)
#define DFLASH_CRASHLOG_SLOT_SIZE   (32U * 1024U)                   /* 32 KB each */
#define DFLASH_CRASHLOG_NUM_SLOTS   4U

/* Carrier-card USB-C configuration */
#define DFLASH_USBCFG_OFFSET       (DFLASH_CRASHLOG_OFFSET + DFLASH_CRASHLOG_SIZE)
#define DFLASH_USBCFG_SIZE         (16U * 1024U)                   /* 16 KB */
#define DFLASH_USBCFG_ADDR         (DFLASH0_BASE + DFLASH_USBCFG_OFFSET)

/* ------------------------------------------------------------------ */
/*  SOTA metadata structure (lives at DFLASH_SOTA_ADDR)               */
/*                                                                    */
/*  Kept small and page-aligned so a single 8-byte page write can     */
/*  update the boot counter atomically.                               */
/* ------------------------------------------------------------------ */
#define DFLASH_SOTA_MAGIC           0x534F5441U  /* "SOTA" in ASCII  */

typedef struct
{
    uint32 magic;           /* DFLASH_SOTA_MAGIC when valid          */
    uint32 pendingUpdate;   /* 1 = update pending, 0 = committed     */
    uint32 bootCounter;     /* incremented each boot while pending   */
    uint32 reserved0;       /* pad to 16 bytes (2 pages)             */
    uint32 imageCrc;        /* CRC32 of the last successfully        */
                            /* programmed update image                */
    uint32 activeBank;      /* 0x55 = Bank A, 0xAA = Bank B          */
    uint32 reserved1;
    uint32 reserved2;       /* total: 32 bytes = 4 DFlash pages      */
} DFlash_SotaMeta_t;

/* ------------------------------------------------------------------ */
/*  Return codes                                                      */
/* ------------------------------------------------------------------ */
typedef enum
{
    DFLASH_OK           = 0,
    DFLASH_ERR_ALIGN    = 1,   /* address not page-aligned            */
    DFLASH_ERR_RANGE    = 2,   /* address outside DFlash0 data region */
    DFLASH_ERR_ERASE    = 3,   /* erase command failed / timeout      */
    DFLASH_ERR_WRITE    = 4,   /* write command failed / timeout      */
    DFLASH_ERR_VERIFY   = 5,   /* read-back mismatch after write      */
} DFlash_Status_t;

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

/**
 * @brief  Initialise the DFlash driver.
 *
 * Currently a no-op (DMU is always available after reset), but
 * provides a hook for future mutex or state init if needed for
 * multicore.
 */
void DFlash_Init(void);

/**
 * @brief  Read raw bytes from DFlash0.
 *
 * DFlash is memory-mapped, so this is a simple memcpy from the
 * flash address.  Provided as an API for consistency and to
 * centralise range-checking.
 *
 * @param  addr   Absolute DFlash0 address to read from.
 * @param  pDest  Destination buffer in RAM.
 * @param  len    Number of bytes to read.
 * @return DFLASH_OK or DFLASH_ERR_RANGE.
 */
DFlash_Status_t DFlash_Read(uint32 addr, void *pDest, uint32 len);

/**
 * @brief  Write data to DFlash0.
 *
 * Writes in 8-byte page increments.  The caller must ensure the
 * target region has been erased first (erase value is 0x00).
 * Data smaller than a full page is zero-padded.
 *
 * @param  addr   Absolute DFlash0 address (must be 8-byte aligned).
 * @param  pSrc   Source data in RAM.
 * @param  len    Number of bytes to write (rounded up to 8-byte pages).
 * @return DFLASH_OK or error code.
 */
DFlash_Status_t DFlash_Write(uint32 addr, const void *pSrc, uint32 len);

/**
 * @brief  Erase one or more 4 KB sectors of DFlash0.
 *
 * @param  addr       Absolute DFlash0 sector-aligned address.
 * @param  numSectors Number of contiguous 4 KB sectors to erase.
 * @return DFLASH_OK or error code.
 */
DFlash_Status_t DFlash_EraseSectors(uint32 addr, uint32 numSectors);

/**
 * @brief  Read the SOTA metadata structure.
 *
 * Reads the DFLASH_SOTA_ADDR region into the provided struct.
 * Returns DFLASH_OK even if the magic is invalid — caller checks
 * the magic field to determine if the data is meaningful.
 *
 * @param  pMeta  Pointer to caller-allocated DFlash_SotaMeta_t.
 * @return DFLASH_OK or DFLASH_ERR_RANGE.
 */
DFlash_Status_t DFlash_ReadSotaMeta(DFlash_SotaMeta_t *pMeta);

/**
 * @brief  Write the SOTA metadata structure.
 *
 * Erases the SOTA sector and writes the provided struct.
 * The magic field should be set to DFLASH_SOTA_MAGIC by the caller.
 *
 * @param  pMeta  Pointer to the metadata to write.
 * @return DFLASH_OK or error code.
 */
DFlash_Status_t DFlash_WriteSotaMeta(const DFlash_SotaMeta_t *pMeta);

#endif /* DFLASH_H */