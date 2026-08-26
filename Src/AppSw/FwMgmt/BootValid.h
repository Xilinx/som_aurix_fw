/**
 * @file    BootValid.h
 * @brief   Boot validation / A-B fallback for SOTA updates (Approach 1)
 *
 * Implements the DFlash-based boot counter strategy:
 *
 *   1. Before triggering a swap, the update module writes a
 *      "pending update" flag + counter = 0 into the SOTA metadata
 *      sector of DFlash0.
 *
 *   2. On every subsequent boot, BootValid_CheckOnStartup() reads
 *      the metadata.  If the pending flag is set it increments the
 *      counter and writes it back.  If the counter reaches
 *      BOOTVALID_MAX_RETRIES the function reverts to the previous
 *      bank via Swap_ChangeMode() and resets.
 *
 *   3. Once the PMC state machine reaches a healthy state,
 *      BootValid_CommitUpdate() clears the pending flag —
 *      the new image is now the permanent active image.
 *
 * On the eval board (TARGET_EVAL_BOARD), the swap-revert path is
 * a no-op because SWAP is not provisioned, but the counter logic
 * is still exercised and logged.
 *
 * References:
 *   [SOTA-DEMO] — swap trigger / revert concept
 *   [SWAP-REGS] — UCB_SWAP marker values (0x55 / 0xAA)
 */

#ifndef BOOTVALID_H
#define BOOTVALID_H

#include "Ifx_Types.h"

/* ------------------------------------------------------------------ */
/*  Configuration                                                     */
/* ------------------------------------------------------------------ */

/** Maximum consecutive boot attempts before reverting to the
 *  previous bank.  Three strikes and you're out.                    */
#ifndef BOOTVALID_MAX_RETRIES
#define BOOTVALID_MAX_RETRIES       3u
#endif

/** How long (ms) the main application has to call CommitUpdate()
 *  before the NEXT reboot treats it as a failed boot.  This is
 *  informational only — the timeout is enforced by the reboot
 *  itself, not by a software timer.  Documented here so the
 *  integration knows the budget.                                    */
#ifndef BOOTVALID_HEALTH_TIMEOUT_MS
#define BOOTVALID_HEALTH_TIMEOUT_MS 15000u
#endif

/* ------------------------------------------------------------------ */
/*  Return codes                                                      */
/* ------------------------------------------------------------------ */
typedef enum
{
    BOOTVALID_OK            = 0,  /* Normal boot, no update pending   */
    BOOTVALID_PENDING       = 1,  /* Update pending, counter < max    */
    BOOTVALID_REVERTED      = 2,  /* Counter hit max, revert issued   */
    BOOTVALID_ERR_DFLASH    = 3,  /* DFlash read/write failed         */
    BOOTVALID_ERR_SWAP      = 4,  /* Swap revert failed               */
} BootValid_Status_t;

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

/**
 * @brief  Early-startup boot validation check.
 *
 * MUST be called very early in core0_main(), after DFlash_Init()
 * and Debug_Init() but BEFORE PowerManager_Init().
 *
 * Behaviour:
 *   - Reads SOTA metadata from DFlash0.
 *   - If magic is invalid or pendingUpdate == 0 → returns
 *     BOOTVALID_OK immediately (normal boot).
 *   - If pendingUpdate == 1 and bootCounter < max → increments
 *     counter, writes it back, returns BOOTVALID_PENDING.
 *   - If pendingUpdate == 1 and bootCounter >= max → reverts
 *     the swap, triggers a system reset, and never returns.
 *     (On eval board: logs the revert intent and returns
 *     BOOTVALID_REVERTED instead of resetting.)
 *
 * @return Status code (see BootValid_Status_t).
 */
BootValid_Status_t BootValid_CheckOnStartup(void);

/**
 * @brief  Commit the current update as good.
 *
 * Called after the PMC state machine reaches a healthy state
 * (PM_STATE_ON or equivalent).  Clears the pending-update flag
 * and zeroes the boot counter so subsequent reboots are normal.
 *
 * Safe to call when no update is pending — it's a no-op if
 * the magic is invalid or pendingUpdate is already 0.
 *
 * @return BOOTVALID_OK on success, BOOTVALID_ERR_DFLASH on failure.
 */
BootValid_Status_t BootValid_CommitUpdate(void);

/**
 * @brief  Prepare the SOTA metadata for an incoming update.
 *
 * Called by the FwUpdate module BEFORE triggering the swap.
 * Sets pendingUpdate = 1, bootCounter = 0, and records the
 * image CRC and target bank.
 *
 * @param  imageCrc   CRC32 of the new firmware image.
 * @param  targetBank 0x55 (Bank A) or 0xAA (Bank B).
 * @return BOOTVALID_OK on success, BOOTVALID_ERR_DFLASH on failure.
 */
BootValid_Status_t BootValid_PrepareForUpdate(uint32 imageCrc, uint32 targetBank);

#endif /* BOOTVALID_H */