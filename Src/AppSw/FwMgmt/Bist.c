/**
 * @file    Bist.c
 * @brief   POST and periodic PFlash CRC integrity check
 */

#include "Bist.h"
#include "Crc32.h"
#include "DFlash.h"
#include "PFlash.h"
#include "Swap.h"
#include "NvLog.h"
#include "FusaSpi.h"
#include "Stm_Timer.h"
#include "Uart_Debug.h"

/* ================================================================== */
/*  State                                                             */
/* ================================================================== */

static Bist_Stats_t s_stats;
static boolean      s_postDone = FALSE;

/* Periodic interval in milliseconds */
#define BIST_PERIOD_MS  ((uint32)BIST_PERIOD_HOURS * 3600u * 1000u)

/* ================================================================== */
/*  Private: CRC the active PFlash bank                               */
/* ================================================================== */

/**
 * Compute CRC-32 over `size` bytes starting at `baseAddr`.
 * Calls keepAliveCb every BIST_CHUNK_SIZE bytes.
 */
static uint32 prv_CrcFlashRegion(uint32 baseAddr, uint32 size,
                                  void (*keepAliveCb)(void))
{
    uint32 crc = Crc32_Init();
    uint32 offset = 0u;
    uint32 lastProgressKb = 0u;

    while (offset < size)
    {
        uint32 chunkLen = size - offset;
        if (chunkLen > BIST_CHUNK_SIZE)
            chunkLen = BIST_CHUNK_SIZE;

        const uint8 *pFlash = (const uint8 *)(baseAddr + offset);
        crc = Crc32_Update(crc, pFlash, chunkLen);
        offset += chunkLen;

        if (keepAliveCb != NULL_PTR)
            keepAliveCb();

        /* Progress every 512 KB */
        uint32 currentKb = offset / 1024u;
        if ((currentKb - lastProgressKb) >= 512u)
        {
            lastProgressKb = currentKb;
            Debug_Printf("[BIST] %u / %u KB\r\n",
                         (unsigned)(offset / 1024u),
                         (unsigned)(size / 1024u));
        }
    }

    return Crc32_Final(crc);
}

/**
 * Run the CRC check and return the result.
 */
static Bist_Status_t prv_RunCheck(void (*keepAliveCb)(void))
{
    DFlash_SotaMeta_t meta;
    DFlash_Status_t ds;
    uint32 activeBase;
    uint32 computedCrc;

    /* Read SOTA metadata */
    ds = DFlash_ReadSotaMeta(&meta);
    if (ds != DFLASH_OK || meta.magic != DFLASH_SOTA_MAGIC)
    {
        Debug_Print("[BIST] No valid SOTA metadata — skipping CRC check\r\n");
        s_stats.expectedCrc = 0u;
        s_stats.imageSize   = 0u;
        return BIST_ERR_NO_META;
    }

    s_stats.expectedCrc = meta.imageCrc;

    /* Determine active bank base address */
    if (meta.activeBank == 0xAAu)
        activeBase = PFLASH_BANK_A_BASE;
    else
        activeBase = PFLASH_BANK_B_BASE;

    /* Image size: if not stored in metadata, use full bank size.
     * The FwUpdate protocol pads to 256-byte boundary and stores
     * the original size, but older metadata may not have it.
     * For safety, CRC the stored imageSize if nonzero, else full bank. */
    if (meta.reserved0 != 0u)
    {
        /* reserved0 repurposed as imageSize in v0.2 metadata */
        s_stats.imageSize = meta.reserved0;
    }
    else
    {
        /* Fallback: CRC the entire bank (padded image is 0xFF-filled) */
        s_stats.imageSize = (activeBase == PFLASH_BANK_A_BASE)
                            ? PFLASH_BANK_A_SIZE : PFLASH_BANK_B_SIZE;
    }

    Debug_Printf("[BIST] Checking 0x%08X, %u bytes, expect CRC=0x%08X\r\n",
                 (unsigned)activeBase, (unsigned)s_stats.imageSize,
                 (unsigned)s_stats.expectedCrc);
                 
    if (s_stats.expectedCrc == 0u)
    {
        Debug_Print("[BIST] No reference CRC in metadata — skipping\r\n");
        return BIST_ERR_NO_META;
    }

    Debug_Printf("[BIST] Checking 0x%08X, %u bytes, expect CRC=0x%08X\r\n",
                 (unsigned)activeBase, (unsigned)s_stats.imageSize,
                 (unsigned)s_stats.expectedCrc);
    /* Compute CRC */
    computedCrc = prv_CrcFlashRegion(activeBase, s_stats.imageSize,
                                      keepAliveCb);
    s_stats.postCrc = computedCrc;

    if (computedCrc != s_stats.expectedCrc)
    {
        Debug_Printf("[BIST] FAIL: computed=0x%08X expected=0x%08X\r\n",
                     (unsigned)computedCrc, (unsigned)s_stats.expectedCrc);
        return BIST_ERR_CRC;
    }

    Debug_Printf("[BIST] PASS: CRC=0x%08X\r\n", (unsigned)computedCrc);
    return BIST_OK;
}

/* ================================================================== */
/*  Public API                                                        */
/* ================================================================== */

Bist_Status_t Bist_RunPost(void (*keepAliveCb)(void))
{
    uint32 startMs = Stm_GetTimeMs();

    Debug_Print("[BIST] Running POST...\r\n");

    s_stats.postResult = prv_RunCheck(keepAliveCb);
    s_stats.postDurationMs = Stm_GetTimeMs() - startMs;
    s_stats.lastBistMs     = Stm_GetTimeMs();
    s_stats.lastBistResult = s_stats.postResult;
    s_stats.bistRunCount   = 0u;
    s_postDone = TRUE;

    Debug_Printf("[BIST] POST %s in %u ms\r\n",
                 (s_stats.postResult == BIST_OK) ? "PASSED" : "FAILED",
                 (unsigned)s_stats.postDurationMs);

    /* On failure: log, alert, but don't halt — the image may still
     * be partially functional, and halting prevents recovery */
    if (s_stats.postResult == BIST_ERR_CRC)
    {
        uint32 nvData[4] = { s_stats.postCrc, s_stats.expectedCrc,
                             s_stats.imageSize, 0u };
        NvLog_Write(NVLOG_EVT_FAULT_BIOS, NVLOG_SRC_SYSTEM,
                    NVLOG_SEV_ERROR, nvData);
        FusaSpi_AssertAlert();
    }

    return s_stats.postResult;
}

void Bist_Run(void (*keepAliveCb)(void))
{
    if (!s_postDone) return;
    if (BIST_PERIOD_HOURS == 0u) return;

    if ((Stm_GetTimeMs() - s_stats.lastBistMs) < BIST_PERIOD_MS)
        return;

    Debug_Print("[BIST] Periodic check...\r\n");

    s_stats.lastBistResult = prv_RunCheck(keepAliveCb);
    s_stats.lastBistMs     = Stm_GetTimeMs();
    s_stats.bistRunCount++;

    if (s_stats.lastBistResult == BIST_ERR_CRC)
    {
        uint32 nvData[4] = { s_stats.postCrc, s_stats.expectedCrc,
                             s_stats.bistRunCount, 0u };
        NvLog_Write(NVLOG_EVT_FAULT_BIOS, NVLOG_SRC_SYSTEM,
                    NVLOG_SEV_ERROR, nvData);
        FusaSpi_AssertAlert();

        Debug_Printf("[BIST] PERIODIC FAIL #%u\r\n",
                     (unsigned)s_stats.bistRunCount);
    }
}

void Bist_GetStats(Bist_Stats_t *pStats)
{
    if (pStats != NULL_PTR)
        *pStats = s_stats;
}

void Bist_DumpStatus(void)
{
    Debug_Printf("[BIST] POST: %s CRC=0x%08X expected=0x%08X (%u ms)\r\n",
                 (s_stats.postResult == BIST_OK) ? "PASS" :
                 (s_stats.postResult == BIST_ERR_NO_META) ? "SKIP" : "FAIL",
                 (unsigned)s_stats.postCrc,
                 (unsigned)s_stats.expectedCrc,
                 (unsigned)s_stats.postDurationMs);
    if (s_stats.bistRunCount > 0u)
    {
        Debug_Printf("[BIST] Periodic: %u runs, last %s\r\n",
                     (unsigned)s_stats.bistRunCount,
                     (s_stats.lastBistResult == BIST_OK) ? "PASS" : "FAIL");
    }
}