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

/* ------------------------------------------------------------------ */
/*  UCB addresses                                                     */
/* ------------------------------------------------------------------ */
#define UCB_SWAP_ORIG_BASE      0xAF402E00U
#define UCB_SWAP_COPY_BASE      0xAF406E00U
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

/** Read SCU_STMEM1.SWAP_CFG (bits 1:0)
 *  00 = no swap, 01 = Bank A, 10 = Bank B                          */
static uint8 readSwapCfg(void)
{
    /* SCU_STMEM1 is at 0xF0036040 on TC38x.
     * The iLLD provides SCU_STMEM1 register access.                 */
    volatile uint32 *pStmem1 = (volatile uint32 *)0xF0036040U;
    return (uint8)((*pStmem1 >> 0) & 0x03U);
}

/** Read SCU_STMEM1.SWAP_DW_INDEX (bits 7:4) — which entry was used */
static uint8 readSwapIndex(void)
{
    volatile uint32 *pStmem1 = (volatile uint32 *)0xF0036040U;
    return (uint8)((*pStmem1 >> 4) & 0x0FU);
}

/* ------------------------------------------------------------------ */
/*  Private: write an 8-byte page to the UCB region                   */
/*                                                                    */
/*  UCB writes follow the same DFlash page-write sequence but target  */
/*  the UCB address range.  The iLLD functions work unchanged.        */
/* ------------------------------------------------------------------ */
static void ucbWritePage(uint32 pageAddr, uint32 wordL, uint32 wordU)
{
    uint16 pw = IfxScuWdt_getSafetyWatchdogPassword();

    IfxFlash_enterPageMode(pageAddr);
    IfxFlash_waitUnbusy(FLASH_MODULE, IfxFlash_FlashType_D0);

    IfxFlash_loadPage2X32(pageAddr, wordL, wordU);

    IfxScuWdt_clearSafetyEndinit(pw);
    IfxFlash_writePage(pageAddr);
    IfxScuWdt_setSafetyEndinit(pw);

    IfxFlash_waitUnbusy(FLASH_MODULE, IfxFlash_FlashType_D0);
}

/** Erase one UCB sector (the whole UCB_SWAP_ORIG or _COPY block). */
static void ucbEraseSector(uint32 sectorAddr)
{
    uint16 pw = IfxScuWdt_getSafetyWatchdogPassword();

    IfxScuWdt_clearSafetyEndinit(pw);
    IfxFlash_eraseMultipleSectors(sectorAddr, 1U);
    IfxScuWdt_setSafetyEndinit(pw);

    IfxFlash_waitUnbusy(FLASH_MODULE, IfxFlash_FlashType_D0);
}

/* ------------------------------------------------------------------ */
/*  Private: write one swap entry to a UCB_SWAP block                 */
/*                                                                    */
/*  Each entry is 16 bytes = 2 DFlash pages:                          */
/*    Page 0: MARKERLx + MARKERHx                                     */
/*    Page 1: CONFIRMATIONLx + CONFIRMATIONHx                         */
/* ------------------------------------------------------------------ */
static void writeEntry(uint32 ucbBase, uint32 index, uint32 markerVal)
{
    uint32 entryAddr   = ucbBase + (index * UCB_ENTRY_SIZE);
    uint32 markerLAddr = entryAddr + OFF_MARKERL;
    uint32 confirmLAddr = entryAddr + OFF_CONFIRML;

    /* Page 0: MARKERLx = swap mode, MARKERHx = address of MARKERLx */
    ucbWritePage(markerLAddr, markerVal, markerLAddr);

    /* Page 1: CONFIRMATIONLx = code, CONFIRMATIONHx = address of CONFIRMATIONLx */
    ucbWritePage(confirmLAddr, SWAP_CONFIRMATION, confirmLAddr);
}

/** Invalidate an entry by overwriting its CONFIRMATION page. */
static void invalidateEntry(uint32 ucbBase, uint32 index)
{
    uint32 entryAddr    = ucbBase + (index * UCB_ENTRY_SIZE);
    uint32 confirmLAddr = entryAddr + OFF_CONFIRML;

    /* Overwrite confirmation with invalid values.
     * Note: DFlash erase state is 0x00, and we can only flip bits
     * from 0→1 without an erase.  Writing 0xFFFFFFFF over a
     * confirmed entry (0x57B5327F) works because all the 0-bits
     * in the confirmation code get set to 1.  This is the strategy
     * from [SOTA-DEMO].                                             */
    ucbWritePage(confirmLAddr, INVALID_WORD, INVALID_WORD);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

uint8 Swap_GetCurrentBank(void)
{
    uint8 cfg = readSwapCfg();

    switch (cfg)
    {
        case 0x01u: return SWAP_BANK_A;
        case 0x02u: return SWAP_BANK_B;
        default:    return 0u;  /* SWAP not enabled */
    }
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
    uint8  currentIndex;
    uint32 nextIndex;
    boolean irqState;

    /* Validate target */
    if (targetBank != SWAP_BANK_A && targetBank != SWAP_BANK_B)
    {
        Debug_Printf("[SWAP] Invalid target bank: 0x%02X\r\n", (unsigned)targetBank);
        return SWAP_ERR_INVALID_BANK;
    }

    /* Check if SWAP is enabled */
    if (Swap_GetCurrentBank() == 0u)
    {
        Debug_Print("[SWAP] SWAP not enabled (SWAPEN not set)\r\n");
        return SWAP_ERR_NOT_ENABLED;
    }

    currentIndex = readSwapIndex();
    nextIndex    = (uint32)currentIndex + 1u;

    Debug_Printf("[SWAP] Current entry index: %u, target bank: 0x%02X\r\n",
                 (unsigned)currentIndex, (unsigned)targetBank);

    irqState = IfxCpu_disableInterrupts();

    if (nextIndex >= SWAP_NUM_ENTRIES)
    {
        /* All 16 slots used — must erase both UCB_SWAP blocks
         * and start at index 0.
         *
         * CRITICAL: erase COPY first, then ORIG.  If power is
         * lost between the two erases, ORIG is still valid and
         * the device boots correctly.  On the next successful
         * swap attempt both will be re-written.                     */
        Debug_Print("[SWAP] All 16 entries used — erasing UCB_SWAP\r\n");

        ucbEraseSector(UCB_SWAP_COPY_BASE);
        ucbEraseSector(UCB_SWAP_ORIG_BASE);

        nextIndex = 0u;
    }
    else
    {
        /* Invalidate the current entry in both blocks.
         * Write COPY first (safety-first ordering).                 */
        invalidateEntry(UCB_SWAP_COPY_BASE, currentIndex);
        invalidateEntry(UCB_SWAP_ORIG_BASE, currentIndex);
    }

    /* Write new entry — COPY first, then ORIG.
     * If power is lost after COPY but before ORIG, the device
     * will use ORIG's previous (now invalidated) state.  SSW
     * falls through to COPY which has the new entry → device
     * boots from the intended bank.                                 */
    writeEntry(UCB_SWAP_COPY_BASE, nextIndex, (uint32)targetBank);
    writeEntry(UCB_SWAP_ORIG_BASE, nextIndex, (uint32)targetBank);

    IfxCpu_restoreInterrupts(irqState);

    Debug_Printf("[SWAP] Wrote entry %u = 0x%02X to UCB_SWAP ORIG+COPY\r\n",
                 (unsigned)nextIndex, (unsigned)targetBank);

    return SWAP_OK;
}

void Swap_TriggerSystemReset(void)
{
    uint16 pw = IfxScuWdt_getSafetyWatchdogPassword();

    Debug_Print("[SWAP] Triggering system reset...\r\n");

    /* Small delay to let the UART finish transmitting */
    {
        volatile uint32 i;
        for (i = 0u; i < 1000000u; i++) {}
    }

    /* Request a system reset via SCU_RSTCON.
     * SW reset type 1 = system reset (evaluates SSW + UCB_SWAP).
     * Application reset (type 0) would NOT re-evaluate UCB_SWAP.    */
    IfxScuWdt_clearSafetyEndinit(pw);

    /* SCU_RSTCON2.USRINFO = 0x01 (SW-initiated reset marker) */
    *(volatile uint32 *)0xF0036048U = 0x00000001U;

    /* SCU_SWRSTCON.SWRSTREQ = 1 triggers the reset */
    *(volatile uint32 *)0xF0036060U = 0x00000002U;

    IfxScuWdt_setSafetyEndinit(pw);

    /* Should never reach here */
    for (;;) {}
}