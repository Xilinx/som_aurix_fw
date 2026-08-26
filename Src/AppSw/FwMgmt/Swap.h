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
 * UCB_SWAP has 16 marker entry slots.  Each swap consumes one
 * slot (invalidate previous + write new).  When all 16 are used
 * the entire UCB_SWAP sector is erased and the next entry is
 * written at index 0.
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

/* ------------------------------------------------------------------ */
/*  Status / debug info                                               */
/* ------------------------------------------------------------------ */
typedef enum
{
    SWAP_OK             = 0,
    SWAP_ERR_NOT_ENABLED = 1,   /* SWAPEN not set in UCB_OTP0        */
    SWAP_ERR_INVALID_BANK = 2,  /* targetBank not 0x55 or 0xAA       */
    SWAP_ERR_UCB_WRITE  = 3,    /* UCB write/erase failed            */
} Swap_Status_t;

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

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
 * @brief  Get the physical base address of the inactive bank.
 * @return PFLASH_BANK_A_BASE or PFLASH_BANK_B_BASE, or 0 on error.
 */
uint32 Swap_GetInactiveBase(void);

/**
 * @brief  Write a new UCB_SWAP marker to select the given bank.
 *
 * Follows the [SOTA-DEMO] strategy:
 *   1. Read current marker index from SCU_STMEM1.SWAP_DW_INDEX
 *   2. Invalidate the current entry (write 0xFFFFFFFF to CONFIRMATION)
 *   3. Write new marker at next index with targetBank + CONFIRMATION
 *   4. If index would exceed 15, erase UCB_SWAP and write at 0
 *   5. Mirror to UCB_SWAP_COPY
 *
 * Does NOT trigger a reset — call Swap_TriggerSystemReset() after.
 *
 * @param  targetBank  SWAP_BANK_A or SWAP_BANK_B.
 * @return SWAP_OK or error code.
 */
Swap_Status_t Swap_ChangeMode(uint8 targetBank);

/**
 * @brief  Trigger a system reset so SSW re-evaluates UCB_SWAP.
 *
 * Uses the SCU system reset request.  This function does not return.
 */
void Swap_TriggerSystemReset(void);

#endif /* SWAP_H */