/**
 * @file    BootValid.c
 * @brief   Boot validation / A-B fallback implementation
 *
 * See BootValid.h for the full algorithm description.
 */

#include "BootValid.h"
#include "DFlash.h"
#include "Uart_Debug.h"

/* Swap module — used for revert on failed boot.
 * On TARGET_EVAL_BOARD we still include the header for the
 * function signature, but the revert path is gated.               */
#include "Swap.h"

/* ------------------------------------------------------------------ */
/*  Private helpers                                                   */
/* ------------------------------------------------------------------ */

/** Check whether SOTA metadata is valid (has been written at least once). */
static boolean isMetaValid(const DFlash_SotaMeta_t *pMeta)
{
    return (pMeta->magic == DFLASH_SOTA_MAGIC);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

BootValid_Status_t BootValid_CheckOnStartup(void)
{
    DFlash_SotaMeta_t meta;
    DFlash_Status_t   dStatus;

    /* Read current SOTA metadata */
    dStatus = DFlash_ReadSotaMeta(&meta);
    if (dStatus != DFLASH_OK)
    {
        Debug_Printf("[BOOT] DFlash read failed (%u)\r\n", (unsigned)dStatus);
        return BOOTVALID_ERR_DFLASH;
    }

    /* If magic is invalid, this is either a fresh board or the
     * sector was corrupted.  Treat as normal boot.                  */
    if (!isMetaValid(&meta))
    {
        Debug_Print("[BOOT] No SOTA metadata found — normal boot\r\n");
        return BOOTVALID_OK;
    }

    /* If no update is pending, nothing to do */
    if (meta.pendingUpdate == 0u)
    {
        Debug_Printf("[BOOT] Bank 0x%02X committed — normal boot\r\n",
                     (unsigned)meta.activeBank);
        return BOOTVALID_OK;
    }

    /* ---- Update is pending — evaluate boot counter -------------- */
    meta.bootCounter++;

    Debug_Printf("[BOOT] Update pending — boot attempt %u / %u\r\n",
                 (unsigned)meta.bootCounter, (unsigned)BOOTVALID_MAX_RETRIES);

    if (meta.bootCounter >= BOOTVALID_MAX_RETRIES)
    {
        /* Too many failed boots — revert to the other bank */
        Debug_Print("[BOOT] Max retries reached — reverting swap\r\n");

#if defined(TARGET_EVAL_BOARD)
        /* On the eval board SWAP is not provisioned, so we can't
         * actually call Swap_ChangeMode().  Clear the pending flag
         * so we don't loop forever, and return REVERTED so the
         * caller knows what happened.                               */
        Debug_Print("[BOOT] EVAL: skipping actual swap revert\r\n");
        meta.pendingUpdate = 0u;
        meta.bootCounter   = 0u;
        dStatus = DFlash_WriteSotaMeta(&meta);
        if (dStatus != DFLASH_OK)
        {
            Debug_Printf("[BOOT] EVAL: failed to clear pending flag (%u)\r\n",
                         (unsigned)dStatus);
        }
        return BOOTVALID_REVERTED;
#else
        /* Production path: revert to the previous bank.
         * Determine which bank to revert TO — it's the opposite
         * of the one recorded in the metadata.                      */
        {
            uint8 revertBank = (meta.activeBank == 0xAAu) ? 0x55u : 0xAAu;

            /* Clear the pending flag BEFORE reverting so the old
             * bank boots into a clean state.                        */
            meta.pendingUpdate = 0u;
            meta.bootCounter   = 0u;
            meta.activeBank    = revertBank;
            dStatus = DFlash_WriteSotaMeta(&meta);
            if (dStatus != DFLASH_OK)
            {
                Debug_Printf("[BOOT] CRITICAL: failed to write revert metadata (%u)\r\n",
                             (unsigned)dStatus);
                return BOOTVALID_ERR_DFLASH;
            }

            Debug_Printf("[BOOT] Reverting to bank 0x%02X — triggering system reset\r\n",
                         (unsigned)revertBank);

            /* Issue the swap and reset.  If this succeeds, the
             * function never returns — the device resets and
             * SSW boots from the reverted bank.                     */
            Swap_ChangeMode(revertBank);
            Swap_TriggerSystemReset();

            /* Should never reach here */
            return BOOTVALID_ERR_SWAP;
        }
#endif /* TARGET_EVAL_BOARD */
    }

    /* Counter is below max — write incremented counter back and
     * let boot continue.  The application has until the next
     * reboot to call BootValid_CommitUpdate().                      */
    dStatus = DFlash_WriteSotaMeta(&meta);
    if (dStatus != DFLASH_OK)
    {
        Debug_Printf("[BOOT] Failed to write boot counter (%u)\r\n",
                     (unsigned)dStatus);
        return BOOTVALID_ERR_DFLASH;
    }

    return BOOTVALID_PENDING;
}

BootValid_Status_t BootValid_CommitUpdate(void)
{
    DFlash_SotaMeta_t meta;
    DFlash_Status_t   dStatus;

    dStatus = DFlash_ReadSotaMeta(&meta);
    if (dStatus != DFLASH_OK)
    {
        return BOOTVALID_ERR_DFLASH;
    }

    /* Nothing to commit if metadata is absent or already committed */
    if (!isMetaValid(&meta) || meta.pendingUpdate == 0u)
    {
        return BOOTVALID_OK;
    }

    /* Clear the pending flag — update is now permanent */
    meta.pendingUpdate = 0u;
    meta.bootCounter   = 0u;

    dStatus = DFlash_WriteSotaMeta(&meta);
    if (dStatus != DFLASH_OK)
    {
        Debug_Printf("[BOOT] Failed to commit update (%u)\r\n",
                     (unsigned)dStatus);
        return BOOTVALID_ERR_DFLASH;
    }

    Debug_Printf("[BOOT] Update committed — bank 0x%02X is now permanent\r\n",
                 (unsigned)meta.activeBank);

    return BOOTVALID_OK;
}

BootValid_Status_t BootValid_PrepareForUpdate(uint32 imageCrc, uint32 targetBank)
{
    DFlash_SotaMeta_t meta;
    DFlash_Status_t   dStatus;

    meta.magic         = DFLASH_SOTA_MAGIC;
    meta.pendingUpdate = 1u;
    meta.bootCounter   = 0u;
    meta.reserved0     = 0u;
    meta.imageCrc      = imageCrc;
    meta.activeBank    = targetBank;
    meta.reserved1     = 0u;
    meta.reserved2     = 0u;

    dStatus = DFlash_WriteSotaMeta(&meta);
    if (dStatus != DFLASH_OK)
    {
        Debug_Printf("[BOOT] Failed to prepare update metadata (%u)\r\n",
                     (unsigned)dStatus);
        return BOOTVALID_ERR_DFLASH;
    }

    Debug_Printf("[BOOT] Update prepared — target bank 0x%02X, CRC 0x%08X\r\n",
                 (unsigned)targetBank, (unsigned)imageCrc);

    return BOOTVALID_OK;
}