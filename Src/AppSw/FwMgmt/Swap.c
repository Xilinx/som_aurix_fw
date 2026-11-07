/**
 * @file    Swap.c
 * @brief   UCB_SWAP A/B bank swap controller implementation
 *
 * UCB_SWAP lives in DFlash0 UCB region (NOT the EEPROM data region).
 * Addresses from the TC38x User Manual:
 *
 *   UCB_SWAP_ORIG  : 0xAF402E00  (UCB sector 23)
 *   UCB_SWAP_COPY  : 0xAF403E00  (as provisioned by tools/gen_ucb_swap.py)
 *
 * Each UCB_SWAP block has 16 entries, each entry is 16 bytes:
 *   Offset +0x00 : MARKERLx     (swap mode: 0x55=A, 0xAA=B)
 *   Offset +0x04 : MARKERHx     (address of MARKERLx for validity)
 *   Offset +0x08 : CONFIRMATIONLx (0x57B5327F = confirmed)
 *   Offset +0x0C : CONFIRMATIONHx (address of CONFIRMATIONLx)
 * and the UCB confirmation at +0x1F0 (0x43211234 UNLOCKED, then 0).
 *
 * The SSW scans entries 0..15 and uses the LAST one with a valid
 * CONFIRMATION.  Invalidation is done by writing 0xFFFFFFFF to
 * CONFIRMATIONLx (breaks the confirmation code check).
 *
 * Endurance: ~100 erase/program cycles per UCB, 500 for all UCBs together
 * (TC38x data sheet).  Entries are used round-robin, so the table is only
 * erased once it is full (~16 x 100 swaps over the device life), and only
 * as a confirmed user action (Swap_ResetTable via CLI fwswap).
 */

#include "Swap.h"
#include "PFlash.h"
#include "IfxFlash.h"
#include "IfxScuWdt.h"
#include "IfxCpu.h"
#include "Uart_Debug.h"
#include "IfxScuRcu.h"
#include "NvLog.h"


#include "IfxScu_reg.h"



/* ------------------------------------------------------------------ */
/*  UCB addresses                                                     */
/* ------------------------------------------------------------------ */
#define UCB_SWAP_ORIG_BASE      0xAF402E00U
#define UCB_SWAP_COPY_BASE      0xAF403E00U
#define UCB_ENTRY_SIZE          16U         /* bytes per entry        */

/* Offsets within each 16-byte entry */
#define OFF_MARKERL             0x00U
#define OFF_MARKERH             0x04U
#define OFF_CONFIRML            0x08U
#define OFF_CONFIRMH            0x0CU

/* UCB confirmation word, identical to the factory block */
#define OFF_UCB_CONFIRM         0x1F0U
#define UCB_CONFIRM_UNLOCKED    0x43211234U

/* DFlash page size — UCB writes use the same 8-byte page mechanism */
#define DFLASH_PAGE             8U
#define FLASH_MODULE            0

/* Invalidation value — breaks the confirmation check */
#define INVALID_WORD            0xFFFFFFFFU

/* Macro for direct memory read */
#define MEM32(a)                (*(volatile uint32 *)(a))

/* ------------------------------------------------------------------ */
/*  SCU registers for swap status                                     */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/*  Private: write an 8-byte page to the UCB region                   */
/*                                                                    */
/*  UCB writes follow the same DFlash page-write sequence but target  */
/*  the UCB address range.  The iLLD functions work unchanged.        */
/* ------------------------------------------------------------------ */
static boolean ucbWritePage(uint32 pageAddr, uint32 wordL, uint32 wordU)
{
    uint16 pw = IfxScuWdt_getSafetyWatchdogPassword();

    IfxFlash_clearStatus(FLASH_MODULE);                 /* DMU_HF_CLRE */

    IfxFlash_enterPageMode(pageAddr);
    IfxFlash_waitUnbusy(FLASH_MODULE, IfxFlash_FlashType_D0);
    IfxFlash_loadPage2X32(pageAddr, wordL, wordU);

    IfxScuWdt_clearSafetyEndinit(pw);
    IfxFlash_writePage(pageAddr);
    IfxScuWdt_setSafetyEndinit(pw);

    IfxFlash_waitUnbusy(FLASH_MODULE, IfxFlash_FlashType_D0);

    uint32 err = MODULE_DMU.HF_ERRSR.U;
    if (MODULE_DMU.HF_ERRSR.B.PVER || MODULE_DMU.HF_ERRSR.B.PROER ||
        MODULE_DMU.HF_ERRSR.B.SQER || MODULE_DMU.HF_ERRSR.B.OPER)
    {
        Debug_Printf("[SWAP] write 0x%08X failed: ERRSR=0x%08X\r\n",
                     (unsigned)pageAddr, (unsigned)err);
        return FALSE;
    }
    return TRUE;
}

/** Erase one UCB sector (the whole UCB_SWAP_ORIG or _COPY block). */
static boolean ucbEraseSector(uint32 sectorAddr)
{
    uint16 pw = IfxScuWdt_getSafetyWatchdogPassword();

    IfxFlash_clearStatus(FLASH_MODULE);

    IfxScuWdt_clearSafetyEndinit(pw);
    IfxFlash_eraseSector(sectorAddr);                   /* not eraseMultipleSectors */
    IfxScuWdt_setSafetyEndinit(pw);

    IfxFlash_waitUnbusy(FLASH_MODULE, IfxFlash_FlashType_D0);

    if (MODULE_DMU.HF_ERRSR.B.EVER || MODULE_DMU.HF_ERRSR.B.PROER ||
        MODULE_DMU.HF_ERRSR.B.SQER)
    {
        Debug_Printf("[SWAP] erase 0x%08X failed: ERRSR=0x%08X\r\n",
                     (unsigned)sectorAddr, (unsigned)MODULE_DMU.HF_ERRSR.U);
        return FALSE;
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  Private: write one swap entry to a UCB_SWAP block                 */
/*                                                                    */
/*  Each entry is 16 bytes = 2 DFlash pages:                          */
/*    Page 0: MARKERLx + MARKERHx                                     */
/*    Page 1: CONFIRMATIONLx + CONFIRMATIONHx                         */
/* ------------------------------------------------------------------ */
static boolean writeEntry(uint32 ucbBase, uint32 index, uint32 markerVal)
{
    uint32 entryAddr    = ucbBase + (index * UCB_ENTRY_SIZE);
    uint32 markerLAddr  = entryAddr + OFF_MARKERL;
    uint32 confirmLAddr = entryAddr + OFF_CONFIRML;

    if (!ucbWritePage(markerLAddr, markerVal, markerLAddr))
    {
        return FALSE;
    }
    return ucbWritePage(confirmLAddr, SWAP_CONFIRMATION, confirmLAddr);
}

static boolean ucbPageProgrammed(uint32 pageAddr)
{
    IfxFlash_clearStatus(FLASH_MODULE);
    IfxFlash_verifyErasedPage(pageAddr);
    IfxFlash_waitUnbusy(FLASH_MODULE, IfxFlash_FlashType_D0);
    return (MODULE_DMU.HF_ERRSR.B.EVER != 0u);   /* EVER set => not erased */
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

uint8 Swap_GetCurrentBank(void)
{
    uint32 i, last = 0xFFFFFFFFu;

    for (i = 0u; i < SWAP_NUM_ENTRIES; i++)
    {
        uint32 e = UCB_SWAP_ORIG_BASE + i * UCB_ENTRY_SIZE;
        if (!ucbPageProgrammed(e) || !ucbPageProgrammed(e + 8u)) { break; }
        last = e;
    }
    if (last == 0xFFFFFFFFu) { return 0xFFu; }

    uint32 marker  = *(volatile uint32 *)last;
    uint32 confirm = *(volatile uint32 *)(last + OFF_CONFIRML);
    if (confirm != SWAP_CONFIRMATION) { return 0xFFu; }
    return (uint8)(marker & 0xFFu);
}

uint32 Swap_GetInactiveBase(void)
{
    /* Key off ADDRCFG (what the SSW mapped at 0xA0000000 for THIS boot),
     * not the UCB marker (what the NEXT reset will apply).  Erase/write
     * safety must follow the bank that is actually running.
     *   A active -> PF2 (PFLASH_BANK_B_BASE) is unmapped
     *   B active -> PF0 (PFLASH_BANK_A_BASE) is unmapped
     *   unknown  -> 0 (caller must skip; never erase blindly)            */
    uint8 active = Swap_GetActiveBank();

    if (active == SWAP_BANK_A)  return PFLASH_BANK_B_BASE;
    if (active == SWAP_BANK_B)  return PFLASH_BANK_A_BASE;
    return 0u;
}

uint32 Swap_GetInactiveLogicalBase(void)
{
    /* ADDRCFG swaps PF0 and PF2 in the CPU map.  The unmapped bank is
     * therefore always visible at PFLASH_BANK_B_BASE (0xA0600000). */
    return (Swap_GetActiveBank() == 0u) ? 0u : PFLASH_BANK_B_BASE;
}

uint32 Swap_GetActiveLogicalBase(void)
{
    return (Swap_GetActiveBank() == 0u) ? 0u : PFLASH_BANK_A_BASE;
}

uint32 Swap_InactivePhysToLogical(uint32 physAddr)
{
    uint32 physBase = Swap_GetInactiveBase();
    uint32 logBase  = Swap_GetInactiveLogicalBase();

    if ((physBase == 0u) || (logBase == 0u) || (physAddr < physBase))
    {
        return 0u;
    }
    return logBase + (physAddr - physBase);
}

/* Next free slot = first entry whose MARKER page is erased.
 * Derived from the UCB itself, not from STMEM1. */
uint32 Swap_EntriesUsed(void)
{
    uint32 n = 0u;

    while ((n < SWAP_NUM_ENTRIES) &&
           ucbPageProgrammed(UCB_SWAP_ORIG_BASE + (n * UCB_ENTRY_SIZE)))
    {
        n++;
    }
    return n;
}

boolean Swap_TableFullForUpdate(void)
{
    return (Swap_EntriesUsed() >= (SWAP_NUM_ENTRIES - SWAP_ROLLBACK_RESERVE));
}

Swap_Status_t Swap_ChangeMode(uint8 targetBank)
{
    uint32  next;
    boolean irq, ok;

    if (targetBank != SWAP_BANK_A && targetBank != SWAP_BANK_B)
    {
        Debug_Printf("[SWAP] Invalid target bank: 0x%02X\r\n", (unsigned)targetBank);
        return SWAP_ERR_INVALID_BANK;
    }

    next = Swap_EntriesUsed();
    if (next >= SWAP_NUM_ENTRIES)
    {
        Debug_Printf("[SWAP] UCB_SWAP table full (%u/%u entries used)\r\n",
                     (unsigned)next, (unsigned)SWAP_NUM_ENTRIES);
        return SWAP_ERR_FULL;
    }

    Debug_Printf("[SWAP] writing entry %u = 0x%02X\r\n",
                 (unsigned)next, (unsigned)targetBank);

    irq = IfxCpu_disableInterrupts();
    ok  = writeEntry(UCB_SWAP_COPY_BASE, next, (uint32)targetBank) &&
          writeEntry(UCB_SWAP_ORIG_BASE, next, (uint32)targetBank);
    IfxCpu_restoreInterrupts(irq);

    if (!ok)
    {
        Debug_Print("[SWAP] UCB write failed\r\n");
        return SWAP_ERR_UCB_WRITE;
    }
    if ((SWAP_NUM_ENTRIES - (next + 1u)) <= SWAP_LOW_ENTRIES_WARN)
    {
        Debug_Printf("[SWAP] WARNING: %u of %u UCB_SWAP entries left; when the table is "
                     "full, fwswap asks before erasing it\r\n",
                     (unsigned)(SWAP_NUM_ENTRIES - (next + 1u)), (unsigned)SWAP_NUM_ENTRIES);
    }
    return SWAP_OK;
}

void Swap_TriggerSystemReset(void)
{
    Debug_Print("[SWAP] Triggering system reset...\r\n");
    Debug_FlushBlocking();                      /* flush the UART instead of a spin delay */

    IfxScuRcu_performReset(IfxScuRcu_ResetType_system, 0x01u);   /* sets RSTCON.SW, RSTCON2.USRINFO, SWRSTCON.SWRSTREQ */

    for (;;) {}                              /* not reached */
}

/* Erase one UCB_SWAP block and rebuild it as tools/gen_ucb_swap.py
 * provisions it: entry 0 = bank, UCB confirmation = UNLOCKED. */
static boolean prv_RebuildBlock(uint32 base, uint8 bank)
{
    boolean irq = IfxCpu_disableInterrupts();
    boolean ok  = ucbEraseSector(base) &&
                  writeEntry(base, 0u, (uint32)bank) &&
                  ucbWritePage(base + OFF_UCB_CONFIRM, UCB_CONFIRM_UNLOCKED, 0u);
    IfxCpu_restoreInterrupts(irq);

    return ok &&
           (MEM32(base + OFF_MARKERL)  == (uint32)bank) &&
           (MEM32(base + OFF_CONFIRML) == SWAP_CONFIRMATION) &&
           (MEM32(base + OFF_UCB_CONFIRM) == UCB_CONFIRM_UNLOCKED);
}

Swap_Status_t Swap_ResetTable(uint8 bank)
{
    uint32 used = Swap_EntriesUsed();

    if ((bank != SWAP_BANK_A) && (bank != SWAP_BANK_B))
    {
        return SWAP_ERR_INVALID_BANK;
    }
    /* Each erase costs one of ~100 cycles: never erase a table with room left. */
    if (used < (SWAP_NUM_ENTRIES - SWAP_ROLLBACK_RESERVE))
    {
        Debug_Printf("[SWAP] table not full (%u/%u) - not erasing\r\n",
                     (unsigned)used, (unsigned)SWAP_NUM_ENTRIES);
        return SWAP_ERR_NOT_FULL;
    }

    /* COPY first, ORIG only once COPY reads back good: a power loss or
     * failure at any step leaves at least one block with a valid entry. */
    Debug_Printf("[SWAP] erasing UCB_SWAP (%u/%u used), entry 0 = 0x%02X\r\n",
                 (unsigned)used, (unsigned)SWAP_NUM_ENTRIES, (unsigned)bank);
    if (!prv_RebuildBlock(UCB_SWAP_COPY_BASE, bank))
    {
        Debug_Print("[SWAP] COPY rebuild failed - ORIG untouched\r\n");
        NvLog_WriteU32(NVLOG_EVT_SWAP_TABLE_RESET, NVLOG_SRC_FWUPDATE,
                       NVLOG_SEV_ERROR, 0x100u | bank);
        return SWAP_ERR_UCB_WRITE;
    }
    if (!prv_RebuildBlock(UCB_SWAP_ORIG_BASE, bank) || (Swap_GetCurrentBank() != bank))
    {
        Debug_Print("[SWAP] ORIG rebuild failed - COPY holds entry 0\r\n");
        NvLog_WriteU32(NVLOG_EVT_SWAP_TABLE_RESET, NVLOG_SRC_FWUPDATE,
                       NVLOG_SEV_ERROR, 0x200u | bank);
        return SWAP_ERR_UCB_WRITE;
    }

    Debug_Print("[SWAP] UCB_SWAP table reset OK\r\n");
    NvLog_WriteU32(NVLOG_EVT_SWAP_TABLE_RESET, NVLOG_SRC_FWUPDATE,
                   NVLOG_SEV_WARNING, bank);
    (void)NvLog_Flush();
    return SWAP_OK;
}

boolean Swap_PageProgrammed(uint32 pageAddr)
{
    return ucbPageProgrammed(pageAddr);
}

/* Kept for the CLI; identical behaviour now */
Swap_Status_t Swap_ChangeModeForce(uint8 targetBank)
{
    return Swap_ChangeMode(targetBank);
}

uint8 Swap_GetActiveBank(void)
{
    switch (SCU_SWAPCTRL.B.ADDRCFG)
    {
        case 1u: return SWAP_BANK_A;     /* standard map  */
        case 2u: return SWAP_BANK_B;     /* alternate map */
        default: return 0u;              /* unrecognized / SWAP not enabled */
    }
}