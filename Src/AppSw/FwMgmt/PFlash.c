/**
 * @file    PFlash.c
 * @brief   PFlash erase / program driver for TC387 SOTA updates
 *
 * Follows the [CODE-EX] Flash_Programming.c pattern exactly:
 *
 *   1. At init, memcpy the iLLD flash command functions into
 *      CPU0 PSPR and assign function pointers.
 *
 *   2. For erase: disable interrupts, call the PSPR-resident
 *      erase routine (clear ENDINIT → eraseMultipleSectors →
 *      set ENDINIT → waitUnbusy), restore interrupts.
 *
 *   3. For write: enter page mode → waitUnbusy → load page data
 *      via loadPage2X32 (8 bytes at a time, 32 iterations for a
 *      256-byte page) → clear ENDINIT → writePage → set ENDINIT
 *      → waitUnbusy.  All from PSPR.
 *
 * IMPORTANT: No other core may be reading from the PFlash bank
 * being written.  Since we only target the INACTIVE bank and
 * all code runs from the ACTIVE bank, this is naturally satisfied
 * in the single-core (CPU0-only) configuration.
 */

#include "PFlash.h"
#include "IfxFlash.h"
#include "IfxScuWdt.h"
#include "IfxCpu.h"
#include "Uart_Debug.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/*  PSPR relocation layout                                            */
/*                                                                    */
/*  CPU0 PSPR base: 0x70100000  (cached local alias)                  */
/*  We reserve space at an offset so we don't collide with stack      */
/*  or any other PSPR usage.  Adjust if your linker places other      */
/*  data in PSPR.                                                     */
/* ------------------------------------------------------------------ */
#define PSPR_RELOC_BASE             0x70100000U
/* Conservative size reservations per function (bytes).
 * These match [CODE-EX] — the actual compiled size of each iLLD
 * function is smaller, but we keep the padding for safety.          */
#define FUNC_SLOT_SIZE              0x100U  /* 256 bytes per slot    */

#define SLOT_ERASE_SECTORS          (PSPR_RELOC_BASE + 0 * FUNC_SLOT_SIZE)
#define SLOT_WAIT_UNBUSY            (PSPR_RELOC_BASE + 1 * FUNC_SLOT_SIZE)
#define SLOT_ENTER_PAGE_MODE        (PSPR_RELOC_BASE + 2 * FUNC_SLOT_SIZE)
#define SLOT_LOAD_PAGE_2X32         (PSPR_RELOC_BASE + 3 * FUNC_SLOT_SIZE)
#define SLOT_WRITE_PAGE             (PSPR_RELOC_BASE + 4 * FUNC_SLOT_SIZE)
#define SLOT_WRITE_BURST            (PSPR_RELOC_BASE + 5 * FUNC_SLOT_SIZE)

/* Flash module index (PMU 0) */
#define FLASH_MODULE                0

/* ------------------------------------------------------------------ */
/*  PSPR-resident function pointer table                              */
/* ------------------------------------------------------------------ */
typedef struct
{
    void  (*eraseSectors)(uint32 sectorAddr, uint32 numSectors);
    uint8 (*waitUnbusy)(uint32 flash, IfxFlash_FlashType flashType);
    uint8 (*enterPageMode)(uint32 pageAddr);
    void  (*loadPage2X32)(uint32 pageAddr, uint32 wordL, uint32 wordU);
    void  (*writePage)(uint32 pageAddr);
    void (*writeBurst)(uint32 pageAddr);
} PFlash_PsprFuncs_t;

static PFlash_PsprFuncs_t g_pspr;
static boolean            g_initialised = FALSE;
static void (*s_keepAliveCb)(void) = NULL_PTR;

/* ------------------------------------------------------------------ */
/*  Private helpers                                                   */
/* ------------------------------------------------------------------ */

static void PFlash_WaitAllUnbusy(void)
{
    while ((DMU_HF_STATUS.U & PFLASH_STATUS_BUSY_MASK) != 0u)
    {
        /* spin — reads of DMU SFRs are always allowed while a
         * physical bank is busy; only array reads of the busy
         * bank are not.                                         */
    }
}

/** Check 16 KB sector alignment. */
static boolean isSectorAligned(uint32 addr)
{
    return ((addr & (PFLASH_SECTOR_SIZE - 1U)) == 0U);
}

/** Check 256-byte burst alignment (the unit writeBurst programs). */
static boolean isBurstAligned(uint32 addr)
{
    return ((addr & (PFLASH_BURST_SIZE - 1U)) == 0U);
}

/** Check that an address falls within PF0..PF3 (the swappable region). */
static boolean isInSwappableRange(uint32 addr, uint32 len)
{
    uint32 end = addr + len;
    /* PF0 starts at 0xA0000000, PF3 ends at 0xA0800000 */
    return (addr >= PFLASH_PF0_BASE) && (end <= PFLASH_PF4_BASE);
}

/**
 * @brief  Determine the IfxFlash_FlashType for a given address.
 *
 * The waitUnbusy function needs to know which physical PFlash
 * bank to poll.  PF0 and PF2 are the 3 MB banks on separate
 * DMU modules.
 */

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

void PFlash_Init(void)
{
    /* Copy iLLD flash routines into CPU0 PSPR.
     * This is the exact pattern from [CODE-EX] copyFunctionsToPSPR(). */
    memcpy((void *)SLOT_ERASE_SECTORS,   (const void *)IfxFlash_eraseMultipleSectors, FUNC_SLOT_SIZE);
    memcpy((void *)SLOT_WAIT_UNBUSY,     (const void *)IfxFlash_waitUnbusy,           FUNC_SLOT_SIZE);
    memcpy((void *)SLOT_ENTER_PAGE_MODE, (const void *)IfxFlash_enterPageMode,        FUNC_SLOT_SIZE);
    memcpy((void *)SLOT_LOAD_PAGE_2X32,  (const void *)IfxFlash_loadPage2X32,         FUNC_SLOT_SIZE);
    memcpy((void *)SLOT_WRITE_PAGE,      (const void *)IfxFlash_writePage,            FUNC_SLOT_SIZE);
    memcpy((void *)SLOT_WRITE_BURST, (const void *)IfxFlash_writeBurst, FUNC_SLOT_SIZE);

    /* Assign function pointers to the PSPR copies */
    g_pspr.eraseSectors  = (void  (*)(uint32, uint32))                    SLOT_ERASE_SECTORS;
    g_pspr.waitUnbusy    = (uint8 (*)(uint32, IfxFlash_FlashType))        SLOT_WAIT_UNBUSY;
    g_pspr.enterPageMode = (uint8 (*)(uint32))                            SLOT_ENTER_PAGE_MODE;
    g_pspr.loadPage2X32  = (void  (*)(uint32, uint32, uint32))            SLOT_LOAD_PAGE_2X32;
    g_pspr.writePage     = (void  (*)(uint32))                            SLOT_WRITE_PAGE;
    g_pspr.writeBurst = (void (*)(uint32))SLOT_WRITE_BURST;
    g_initialised = TRUE;

    Debug_Print("[PFLASH] PSPR relocation complete\r\n");
}

PFlash_Status_t PFlash_EraseSector(uint32 sectorAddr)
{
    uint16              password;
    //IfxFlash_FlashType  ftype;
    boolean             irqState;

    if (!g_initialised)                         return PFLASH_ERR_NOT_INIT;
    if (!isSectorAligned(sectorAddr))           return PFLASH_ERR_ALIGN;
    if (!isInSwappableRange(sectorAddr, PFLASH_SECTOR_SIZE))
                                                return PFLASH_ERR_RANGE;

    //ftype    = getFlashType(sectorAddr);
    password = IfxScuWdt_getSafetyWatchdogPasswordInline();

    /* Disable interrupts — no code may execute from this PFlash
     * bank while the erase is in progress.  Since we're erasing
     * the INACTIVE bank and all code runs from the ACTIVE bank,
     * this is mainly to prevent an ISR from issuing a conflicting
     * DMU command.                                                  */
    irqState = IfxCpu_disableInterrupts();

    IfxScuWdt_clearSafetyEndinitInline(password);
    g_pspr.eraseSectors(sectorAddr, 1U);
    IfxScuWdt_setSafetyEndinitInline(password);

    PFlash_WaitAllUnbusy();
    {
        uint32 errsr = DMU_HF_ERRSR.U;
        IfxFlash_resetToRead(FLASH_MODULE);
        IfxCpu_restoreInterrupts(irqState);
        if (errsr != 0u)
        {
            Debug_Printf("[PFLASH] Write error, ERRSR=0x%08X\r\n", (unsigned)errsr);
            return PFLASH_ERR_WRITE;
        }
    }

    return PFLASH_OK;
}

PFlash_Status_t PFlash_EraseBank(uint32 bankBase)
{
    uint32          bankSize;
    uint32          numSectors;
    uint32          i;
    PFlash_Status_t status;

    if (bankBase == PFLASH_BANK_A_BASE)
        bankSize = PFLASH_BANK_A_SIZE;
    else if (bankBase == PFLASH_BANK_B_BASE)
        bankSize = PFLASH_BANK_B_SIZE;
    else
        return PFLASH_ERR_RANGE;

    numSectors = bankSize / PFLASH_SECTOR_SIZE;

    Debug_Printf("[PFLASH] Erasing bank at 0x%08X (%u sectors)...\r\n",
                 (unsigned)bankBase, (unsigned)numSectors);

    for (i = 0u; i < numSectors; i++)
    {
        uint32 addr = bankBase + (i * PFLASH_SECTOR_SIZE);
        status = PFlash_EraseSector(addr);
        if (status != PFLASH_OK)
        {
            Debug_Printf("[PFLASH] Erase failed at sector %u (addr 0x%08X, err %u)\r\n",
                         (unsigned)i, (unsigned)addr, (unsigned)status);
            return status;
        }

        /* Progress reporting every 32 sectors (512 KB) */
        if (((i + 1u) % 32u) == 0u)
        {
            Debug_Printf("[PFLASH] Erased %u / %u sectors\r\n",
                         (unsigned)(i + 1u), (unsigned)numSectors);
        }
        if (s_keepAliveCb != NULL_PTR)
        {
            s_keepAliveCb();
        }
    }

    Debug_Print("[PFLASH] Bank erase complete\r\n");
    return PFLASH_OK;
}

PFlash_Status_t PFlash_WritePage256(uint32 pageAddr, const uint8 *pData)
{
    uint16        password;
    boolean       irqState;
    uint32        offset;
    const uint32 *pWords = (const uint32 *)pData;

    if (!g_initialised)                              return PFLASH_ERR_NOT_INIT;
    if ((pageAddr & (PFLASH_BURST_SIZE - 1u)) != 0u) return PFLASH_ERR_ALIGN;  /* 256-byte aligned */
    if (!isInSwappableRange(pageAddr, PFLASH_BURST_SIZE)) return PFLASH_ERR_RANGE;
    if (!isBurstAligned(pageAddr))                  return PFLASH_ERR_ALIGN;

    password = IfxScuWdt_getSafetyWatchdogPasswordInline();
    irqState = IfxCpu_disableInterrupts();

    g_pspr.enterPageMode(pageAddr);
    PFlash_WaitAllUnbusy();

    /* Load the full 256-byte assembly buffer: 32 calls x 8 bytes */
    for (offset = 0u; offset < PFLASH_BURST_SIZE; offset += 8u)
    {
        uint32 idx = offset / 4u;
        g_pspr.loadPage2X32(pageAddr, pWords[idx], pWords[idx + 1u]);
    }

    IfxScuWdt_clearSafetyEndinitInline(password);
    g_pspr.writeBurst(pageAddr);                 /* <-- burst, not page */
    IfxScuWdt_setSafetyEndinitInline(password);

    PFlash_WaitAllUnbusy();

    {
        uint32 errsr = DMU_HF_ERRSR.U;
        IfxFlash_resetToRead(FLASH_MODULE);
        IfxCpu_restoreInterrupts(irqState);
        if (errsr != 0u)
        {
            Debug_Printf("[PFLASH] Write error, ERRSR=0x%08X\r\n", (unsigned)errsr);
            return PFLASH_ERR_WRITE;
        }
    }
    return PFLASH_OK;
}

PFlash_Status_t PFlash_VerifyPage256(uint32 pageAddr, const uint8 *pExpected)
{
    const uint8 *pFlash = (const uint8 *)pageAddr;
    uint32 i;
    for (i = 0u; i < PFLASH_PAGE_SIZE; i++)
    {
        if (pFlash[i] != pExpected[i])
        {
            Debug_Printf("[PFLASH] Verify mismatch at 0x%08X+%u: got 0x%02X, expected 0x%02X\r\n",
                         (unsigned)pageAddr, (unsigned)i,
                         (unsigned)pFlash[i], (unsigned)pExpected[i]);
            return PFLASH_ERR_VERIFY;
        }
    }

    return PFLASH_OK;
}

void PFlash_RegisterKeepAliveCb(void (*cb)(void))
{
    s_keepAliveCb = cb;
}