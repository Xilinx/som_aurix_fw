/**
 * @file    Swap.h
 * @brief   UCB_SWAP A/B bank swap controller for TC387
 *
 * Controls the hardware PFlash bank swap mechanism via UCB_SWAP
 * registers.  Adapted from [SOTA-DEMO] swap.c.
 *
 * The swap takes effect on the next SYSTEM reset (not application
 * reset).  The SSW evaluates UCB_SWAP_ORIG for the latest valid
 * marker entry and installs the corresponding address map.
 *
 * UCB_SWAP has 16 marker entry slots.  Each swap (fwswap or an
 * automatic rollback) writes the next free slot.  A full table is
 * never erased automatically: UCB flash endures ~100 erase/program
 * cycles, so the erase is a confirmed user action (CLI fwswap ->
 * Swap_ResetTable), which rewrites entry 0 with the target bank.
 *
 * WARNING: Incorrect UCB writes can permanently brick the device.
 * Always write UCB_SWAP_COPY before UCB_SWAP_ORIG.
 *
 * References:
 *   [SOTA-DEMO] — swap.c Swap_ChangeMode() strategy
 *   [SWAP-REGS] — UCB register layout, bricking scenarios
 *   [SOTA-DOC]  — SSW evaluation sequence
 */

#ifndef SWAP_H
#define SWAP_H

#include "Ifx_Types.h"

/* Bank identifiers — match UCB_SWAP marker values */
#define SWAP_BANK_A         0x55u
#define SWAP_BANK_B         0xAAu

/* Confirmation code required by SSW to accept a marker entry */
#define SWAP_CONFIRMATION   0x57B5327Fu

/* Number of marker entry slots in UCB_SWAP */
#define SWAP_NUM_ENTRIES    16u

/* Entries fwswap leaves free for the automatic rollback in BootValid,
 * which runs at boot with no user to confirm a table erase. */
#ifndef SWAP_ROLLBACK_RESERVE
#define SWAP_ROLLBACK_RESERVE   1u
#endif

/* Warn after a swap once this many entries (or fewer) are left. */
#define SWAP_LOW_ENTRIES_WARN   2u

/* ------------------------------------------------------------------ */
/*  Status / debug info                                               */
/* ------------------------------------------------------------------ */
typedef enum
{
    SWAP_OK               = 0,
    SWAP_ERR_NOT_ENABLED  = 1,   /* SWAPEN not set in UCB_OTP0        */
    SWAP_ERR_INVALID_BANK = 2,   /* targetBank not 0x55 or 0xAA       */
    SWAP_ERR_UCB_WRITE    = 3,   /* UCB write/erase failed            */
    SWAP_ERR_FULL         = 4,   /* all 16 swap entries used          */
    SWAP_ERR_NOT_FULL     = 5,   /* table reset refused: room left    */
} Swap_Status_t;

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */


Swap_Status_t Swap_ChangeModeForce(uint8 targetBank);
boolean       Swap_PageProgrammed(uint32 pageAddr);   /* wrapper for the CLI dump */

/**
 * @brief  Get which bank is currently active.
 *
 * Reads SCU_STMEM1.SWAP_CFG:
 *   01b = Bank A (standard)
 *   10b = Bank B (alternate)
 *   00b = SWAP not enabled
 *
 * @return SWAP_BANK_A, SWAP_BANK_B, or 0 if SWAP is not enabled.
 */
uint8 Swap_GetCurrentBank(void);

/**
 * @brief  Get the physical base address of the currently-unmapped bank.
 *
 * Derived from Swap_GetActiveBank() (ADDRCFG = what is mapped this boot):
 *   A active -> PFLASH_BANK_B_BASE (PF2), B active -> PFLASH_BANK_A_BASE (PF0).
 * This is the only bank safe to erase/write while executing.
 *
 * @return PFLASH_BANK_A_BASE or PFLASH_BANK_B_BASE, or 0 if the active
 *         bank is unknown (caller MUST skip erase/write in that case).
 */
uint32 Swap_GetInactiveBase(void);

/**
 * @brief  CPU-load address of the currently-unmapped bank.
 *
 * Flash command sequences (erase/write) use physical addresses from
 * Swap_GetInactiveBase().  CPU loads follow SCU_SWAPCTRL.ADDRCFG:
 *   0xA0000000 always reads the running bank,
 *   0xA0600000 always reads the other physical bank.
 * CRC / verify must use this window, not the physical base — otherwise
 * a board running from B CRCs its own image at 0xA0000000 (IMG_CRC).
 *
 * @return PFLASH_BANK_B_BASE (0xA0600000), or 0 if ADDRCFG is unknown.
 */
uint32 Swap_GetInactiveLogicalBase(void);

/**
 * @brief  CPU-load address of the bank mapped at the boot vector.
 *
 * @return PFLASH_BANK_A_BASE (0xA0000000), or 0 if ADDRCFG is unknown.
 */
uint32 Swap_GetActiveLogicalBase(void);

/**
 * @brief  Translate a physical inactive-bank address to the CPU window.
 *
 * Used by readback/verify of a page that was programmed at a physical
 * address.  Returns 0 if the active map is unknown.
 */
uint32 Swap_InactivePhysToLogical(uint32 physAddr);

/**
 * @brief  Write a new UCB_SWAP marker to select the given bank.
 *
 * Writes targetBank + CONFIRMATION to the first erased entry, COPY
 * before ORIG.  The free entry is found from the UCB itself (verify-
 * erased), not from STMEM1.  Warns when SWAP_LOW_ENTRIES_WARN or fewer
 * entries are left.  Never erases: a full table returns SWAP_ERR_FULL.
 *
 * Does NOT trigger a reset — call Swap_TriggerSystemReset() after.
 *
 * @param  targetBank  SWAP_BANK_A or SWAP_BANK_B.
 * @return SWAP_OK or error code.
 */
Swap_Status_t Swap_ChangeMode(uint8 targetBank);

/** Number of programmed UCB_SWAP entries (0..16). */
uint32 Swap_EntriesUsed(void);

/** TRUE when fwswap has no entry left (SWAP_ROLLBACK_RESERVE kept free). */
boolean Swap_TableFullForUpdate(void);

/**
 * @brief  Erase UCB_SWAP and rewrite entry 0 = bank (user-confirmed only).
 *
 * Refuses (SWAP_ERR_NOT_FULL) unless Swap_TableFullForUpdate().  Rebuilds
 * COPY, reads it back, then ORIG, each block exactly as provisioned by
 * tools/gen_ucb_swap.py.  Logs NVLOG_EVT_SWAP_TABLE_RESET.  Costs one of
 * ~100 erase/program cycles of each UCB.  Does NOT reset.
 */
Swap_Status_t Swap_ResetTable(uint8 bank);

/**
 * @brief  Trigger a system reset so SSW re-evaluates UCB_SWAP.
 *
 * Uses the SCU system reset request.  This function does not return.
 */
void Swap_TriggerSystemReset(void) __attribute__((noreturn));

uint8 Swap_GetActiveBank(void);      /* bank the SSW mapped at 0xA0000000: 0x55 / 0xAA, or 0 if unknown */


#endif /* SWAP_H */