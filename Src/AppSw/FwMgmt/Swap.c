/**
 * @file    Swap.c
 * @brief   UCB_SWAP A/B bank swap controller implementation
 *
 * UCB_SWAP lives in DFlash0 UCB region (NOT the EEPROM data region).
 * Addresses from the TC38x User Manual:
 *
 *   UCB_SWAP_ORIG  : 0xAF402E00  (UCB sector 23)
 *   UCB_SWAP_COPY  : 0xAF406E00  (UCB sector 23 copy)
 *
 * Each UCB_SWAP block has 16 entries, each entry is 16 bytes:
 *   Offset +0x00 : MARKERLx     (swap mode: 0x55=A, 0xAA=B)
 *   Offset +0x04 : MARKERHx     (address of MARKERLx for validity)
 *   Offset +0x08 : CONFIRMATIONLx (0x57B5327F = confirmed)
 *   Offset +0x0C : CONFIRMATIONHx (address of CONFIRMATIONLx)
 *
 * The SSW scans entries 0..15 and uses the LAST one with a valid
 * CONFIRMATION.  Invalidation is done by writing 0xFFFFFFFF to
 * CONFIRMATIONLx (breaks the confirmation code check).
 */

#include "Swap.h"
#include "PFlash.h"
#include "IfxFlash.h"
#include "IfxScuWdt.h"
#include "IfxCpu.h"
#include "Uart_Debug.h"
#include "IfxScuRcu.h"


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
    uint8 current = Swap_GetCurrentBank();

    if (current == SWAP_BANK_A)  return PFLASH_BANK_B_BASE;
    if (current == SWAP_BANK_B)  return PFLASH_BANK_A_BASE;
    return 0u;
}

Swap_Status_t Swap_ChangeMode(uint8 targetBank)
{
    uint32  next = 0u;
    boolean irq, ok;

    if (targetBank != SWAP_BANK_A && targetBank != SWAP_BANK_B)
    {
        Debug_Printf("[SWAP] Invalid target bank: 0x%02X\r\n", (unsigned)targetBank);
        return SWAP_ERR_INVALID_BANK;
    }

    /* Next free slot = first entry whose MARKER page is erased.
     * Derived from the UCB itself, not from STMEM1.                   */
    while (next < SWAP_NUM_ENTRIES &&
           ucbPageProgrammed(UCB_SWAP_ORIG_BASE + next * UCB_ENTRY_SIZE))
    {
        next++;
    }
    if (next >= SWAP_NUM_ENTRIES)
    {
        Debug_Print("[SWAP] all 16 entries used - run Swap_EraseAll first\r\n");
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
    return SWAP_OK;
}

void Swap_TriggerSystemReset(void)
{
    Debug_Print("[SWAP] Triggering system reset...\r\n");
    Debug_FlushBlocking();                      /* flush the UART instead of a spin delay */

    IfxScuRcu_performReset(IfxScuRcu_ResetType_system, 0x01u);   /* sets RSTCON.SW, RSTCON2.USRINFO, SWRSTCON.SWRSTREQ */

    for (;;) {}                              /* not reached */
}

Swap_Status_t Swap_EraseAll(void)
{
    boolean irq = IfxCpu_disableInterrupts();
    boolean ok;

    Debug_Print("[SWAP] Erasing UCB_SWAP...\r\n");
    ok = ucbEraseSector(UCB_SWAP_COPY_BASE) && ucbEraseSector(UCB_SWAP_ORIG_BASE);
    if (ok)
    {
        ok = writeEntry(UCB_SWAP_COPY_BASE, 0u, (uint32)SWAP_BANK_A) &&
             writeEntry(UCB_SWAP_ORIG_BASE, 0u, (uint32)SWAP_BANK_A);
    }

    IfxCpu_restoreInterrupts(irq);
    Debug_Printf("[SWAP] UCB_SWAP reset: %s\r\n", ok ? "OK" : "FAILED");
    return ok ? SWAP_OK : SWAP_ERR_UCB_WRITE;
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
        default: return 0xFFu;
    }
}