/**
 * @file    DFlash.c
 * @brief   DFlash0 read/write/erase driver for TC387
 *
 * Implementation follows the iLLD Flash_Programming code example
 * pattern from [CODE-EX].  DFlash operations do NOT require
 * relocation to PSPR (unlike PFlash) because the DMU command
 * interface for DFlash is independent of the PFlash bank the
 * CPU is executing from.
 *
 * Sequence per Infineon KB article "Programming and Verification
 * of DFLASH for TC3xx":
 *   Erase : clearSafetyEndinit → eraseMultipleSectors →
 *           setSafetyEndinit → waitUnbusy
 *   Write : enterPageMode → waitUnbusy →
 *           loadPage2X32 → clearSafetyEndinit →
 *           writePage → setSafetyEndinit → waitUnbusy
 *   Read  : direct memory-mapped access (pointer dereference)
 */

#include "DFlash.h"
#include "IfxFlash.h"
#include "IfxScuWdt.h"
#include "Uart_Debug.h"
#include "IfxCpu.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Private constants                                                 */
/* ------------------------------------------------------------------ */
#define FLASH_MODULE        0                       /* PMU module 0   */
#define DFLASH_TYPE         IfxFlash_FlashType_D0   /* DFlash bank 0  */
#define MEM32(a)            (*(volatile uint32 *)(a))

/* Timeout for waitUnbusy — number of polling iterations before
 * declaring a failure.  Conservative; real hardware completes in
 * microseconds per page.                                             */
#define DFLASH_BUSY_TIMEOUT 100000U

/* ------------------------------------------------------------------ */
/*  Private helpers                                                   */
/* ------------------------------------------------------------------ */

/** Check that an address falls within the DFlash0 user data region. */
static boolean isInRange(uint32 addr, uint32 len)
{
    if (addr < DFLASH0_BASE)                return FALSE;
    if ((addr + len) > DFLASH0_END)         return FALSE;
    return TRUE;
}

/** Check 8-byte page alignment. */
static boolean isPageAligned(uint32 addr)
{
    return ((addr & (DFLASH0_PAGE_SIZE - 1U)) == 0U);
}

/** Check 4 KB sector alignment. */
static boolean isSectorAligned(uint32 addr)
{
    return ((addr & (DFLASH0_SECTOR_SIZE - 1U)) == 0U);
}

/**
 * @brief  Write a single 8-byte page to DFlash0.
 *
 * Follows the exact iLLD sequence from [CODE-EX] writeDataFlash().
 * The caller is responsible for ensuring the page is erased.
 *
 * @param  pageAddr  8-byte-aligned absolute DFlash0 address.
 * @param  wordL     Lower 32 bits of the 8-byte page.
 * @param  wordU     Upper 32 bits of the 8-byte page.
 * @return DFLASH_OK or DFLASH_ERR_WRITE.
 */
static DFlash_Status_t writeSinglePage(uint32 pageAddr, uint32 wordL, uint32 wordU)
{
    uint16 password = IfxScuWdt_getSafetyWatchdogPassword();

    /* Enter page mode */
    IfxFlash_enterPageMode(pageAddr);

    /* Wait for page mode ready */
    IfxFlash_waitUnbusy(FLASH_MODULE, DFLASH_TYPE);

    /* Load the two 32-bit halves of the 8-byte page */
    IfxFlash_loadPage2X32(pageAddr, wordL, wordU);

    /* Write the page — requires safety ENDINIT cleared */
    IfxScuWdt_clearSafetyEndinit(password);
    IfxFlash_writePage(pageAddr);
    IfxScuWdt_setSafetyEndinit(password);

    /* Wait for write completion */
    IfxFlash_waitUnbusy(FLASH_MODULE, DFLASH_TYPE);

    return DFLASH_OK;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

void DFlash_Init(void)
{
    /* Currently no initialisation needed — DMU is available after
     * reset.  This is a placeholder for future multicore mutex or
     * state tracking if we move to the CPU0/CPU2 split.             */
}

DFlash_Status_t DFlash_Read(uint32 addr, void *pDest, uint32 len)
{
    if (!isInRange(addr, len))
    {
        return DFLASH_ERR_RANGE;
    }

    /* DFlash is memory-mapped; a simple memcpy is sufficient.
     * Erased DFlash reads as 0x00 without ECC errors.              */
    memcpy(pDest, (const void *)addr, len);

    return DFLASH_OK;
}

DFlash_Status_t DFlash_Write(uint32 addr, const void *pSrc, uint32 len)
{
    const uint8 *src = (const uint8 *)pSrc;
    uint32 remaining = len;
    uint32 currentAddr = addr;
    DFlash_Status_t status;

    /* Validate inputs */
    if (!isPageAligned(addr))
    {
        return DFLASH_ERR_ALIGN;
    }
    /* Round up len to full pages for range check */
    uint32 totalLen = ((len + DFLASH0_PAGE_SIZE - 1U) / DFLASH0_PAGE_SIZE) * DFLASH0_PAGE_SIZE;
    if (!isInRange(addr, totalLen))
    {
        return DFLASH_ERR_RANGE;
    }

    /* Write page by page (8 bytes each) */
    while (remaining > 0U)
    {
        uint32 wordL = 0U;
        uint32 wordU = 0U;
        uint32 chunk = (remaining >= DFLASH0_PAGE_SIZE) ? DFLASH0_PAGE_SIZE : remaining;

        /* Pack up to 8 bytes into two 32-bit words.
         * If fewer than 8 bytes remain, the rest stays 0x00
         * (which is the erase value, so no ECC issue).              */
        memcpy(&wordL, src, (chunk > 4U) ? 4U : chunk);
        if (chunk > 4U)
        {
            memcpy(&wordU, src + 4U, chunk - 4U);
        }

        status = writeSinglePage(currentAddr, wordL, wordU);
        if (status != DFLASH_OK)
        {
            return status;
        }

        currentAddr += DFLASH0_PAGE_SIZE;
        src         += DFLASH0_PAGE_SIZE;

        if (remaining >= DFLASH0_PAGE_SIZE)
        {
            remaining -= DFLASH0_PAGE_SIZE;
        }
        else
        {
            remaining = 0U;
        }
    }

    return DFLASH_OK;
}

DFlash_Status_t DFlash_EraseSectors(uint32 addr, uint32 numSectors)
{
    uint16 password;

    if (!isSectorAligned(addr))                             return DFLASH_ERR_ALIGN;
    if (!isInRange(addr, numSectors * DFLASH0_SECTOR_SIZE)) return DFLASH_ERR_RANGE;

    password = IfxScuWdt_getSafetyWatchdogPassword();

    IfxScuWdt_clearSafetyEndinit(password);
    IfxFlash_eraseMultipleSectors(addr, numSectors);
    IfxScuWdt_setSafetyEndinit(password);
    IfxFlash_waitUnbusy(FLASH_MODULE, DFLASH_TYPE);

    return DFLASH_OK;
}

DFlash_Status_t DFlash_ReadSotaMeta(DFlash_SotaMeta_t *pMeta)
{
    return DFlash_Read(DFLASH_SOTA_ADDR, pMeta, sizeof(DFlash_SotaMeta_t));
}

DFlash_Status_t DFlash_WriteSotaMeta(const DFlash_SotaMeta_t *pMeta)
{
    DFlash_Status_t status;

    status = DFlash_EraseSectors(DFLASH_SOTA_ADDR, 1U);
    if (status != DFLASH_OK) return status;

    status = DFlash_Write(DFLASH_SOTA_ADDR, pMeta, sizeof(DFlash_SotaMeta_t));
    return status;
}