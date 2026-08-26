/**
 * @file    Bist.h
 * @brief   Power-On Self Test and periodic Built-In Self Test
 *
 * POST: CRC-32 of the active PFlash bank compared against the
 * image CRC stored in SOTA DFlash metadata.
 *
 * Periodic BIST: re-runs the same CRC check every BIST_PERIOD_HOURS.
 *
 * If the CRC doesn't match, the firmware image is corrupted —
 * log to NvLog, assert FUSA_ALERT, and optionally trigger a
 * recovery reboot to the alternate bank.
 */

#ifndef BIST_H
#define BIST_H

#include "Ifx_Types.h"

/* ================================================================== */
/*  Configuration                                                     */
/* ================================================================== */

/** Periodic BIST interval in hours (0 = disabled) */
#ifndef BIST_PERIOD_HOURS
#define BIST_PERIOD_HOURS       4u
#endif

/** CRC check chunk size — read PFlash in 4KB blocks to allow
 *  WDT servicing between chunks */
#define BIST_CHUNK_SIZE         4096u

/* ================================================================== */
/*  Status                                                            */
/* ================================================================== */

typedef enum
{
    BIST_OK             = 0u,
    BIST_ERR_NO_META    = 1u,   /* SOTA metadata not found/invalid */
    BIST_ERR_CRC        = 2u,   /* CRC mismatch — image corrupt */
    BIST_ERR_NOT_RUN    = 3u,   /* POST hasn't run yet */
} Bist_Status_t;

typedef struct
{
    Bist_Status_t postResult;       /**< Result of boot-time POST */
    uint32        postCrc;          /**< CRC computed during POST */
    uint32        expectedCrc;      /**< CRC from SOTA metadata */
    uint32        imageSize;        /**< Image size from metadata */
    uint32        postDurationMs;   /**< How long POST took */
    uint32        lastBistMs;       /**< Timestamp of last periodic check */
    uint32        bistRunCount;     /**< Number of periodic checks completed */
    Bist_Status_t lastBistResult;   /**< Result of last periodic check */
} Bist_Stats_t;

/* ================================================================== */
/*  Public API                                                        */
/* ================================================================== */

/**
 * @brief  Run Power-On Self Test.
 *
 * Reads the SOTA metadata from DFlash to get the expected CRC
 * and image size, then CRC-32s the active PFlash bank in 4KB
 * chunks (servicing the WDT between chunks).
 *
 * Call after DFlash_Init and BootValid_CheckOnStartup, but before
 * entering the main loop.
 *
 * @param  keepAliveCb  Called between 4KB chunks (pass Tlf35585_ServiceWdt)
 * @return BIST_OK or error code
 */
Bist_Status_t Bist_RunPost(void (*keepAliveCb)(void));

/**
 * @brief  Periodic service — call from main loop.
 *
 * Checks if BIST_PERIOD_HOURS has elapsed since the last check.
 * If so, re-runs the CRC check.  Non-blocking between chunks
 * is NOT implemented in v0.2 — the periodic check blocks for
 * ~50ms (4MB / 80MHz read speed) with WDT service between chunks.
 */
void Bist_Run(void (*keepAliveCb)(void));

/**
 * @brief  Get POST/BIST statistics.
 */
void Bist_GetStats(Bist_Stats_t *pStats);

/**
 * @brief  Print POST/BIST status to debug UART.
 */
void Bist_DumpStatus(void);

#endif /* BIST_H */