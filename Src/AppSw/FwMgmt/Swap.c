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
    uint32 raw = SCU_STMEM1.U;
    uint8  cfg = (uint8)(raw & 0x03u);
    Debug_Printf("[SWAP] readSwapCfg: SCU_STMEM1=0x%08X, bits[1:0]=%u\r\n",
                 (unsigned)raw, (unsigned)cfg);
    return cfg;
}

static uint8 readSwapIndex(void)
{
    return (uint8)((SCU_STMEM1.U >> 4) & 0x0Fu);
}

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


/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

uint8 Swap_GetCurrentBank(void)
{
    uint8 cfg = readSwapCfg();
    uint8 bank;
    switch (cfg)
    {
        case 0x01u: bank = SWAP_BANK_A; break;
        case 0x02u: bank = SWAP_BANK_B; break;
        default:    bank = 0xFFu; break;
    }
    Debug_Printf("[SWAP] GetCurrentBank: cfg=%u -> bank=0x%02X\r\n",
                 (unsigned)cfg, (unsigned)bank);
    return bank;
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
    if (Swap_GetCurrentBank() == 0xFFu)
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

Swap_Status_t Swap_EraseAll(void)
{
    boolean irqState = IfxCpu_disableInterrupts();

    Debug_Print("[SWAP] Erasing UCB_SWAP...\r\n");
    boolean okCopy = ucbEraseSector(UCB_SWAP_COPY_BASE);
    Debug_Printf("[SWAP] COPY erase: %s\r\n", okCopy ? "OK" : "FAILED");

    boolean okOrig = ucbEraseSector(UCB_SWAP_ORIG_BASE);
    Debug_Printf("[SWAP] ORIG erase: %s\r\n", okOrig ? "OK" : "FAILED");

    writeEntry(UCB_SWAP_COPY_BASE, 0u, (uint32)SWAP_BANK_A);
    
    writeEntry(UCB_SWAP_ORIG_BASE, 0u, (uint32)SWAP_BANK_A);

    IfxCpu_restoreInterrupts(irqState);

    Debug_Print("[SWAP] UCB_SWAP reset complete\r\n");
    return SWAP_OK;
}