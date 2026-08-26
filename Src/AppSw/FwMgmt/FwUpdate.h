/**
 * @file    FwUpdate.h
 * @brief   UART-based firmware update protocol
 *
 * Implements a framed protocol for receiving a firmware binary
 * over UART, programming it to the inactive PFlash bank, and
 * triggering the A/B swap.
 *
 * Protocol frames (host → AURIX):
 *   SYNC    : 4 bytes  magic 0x55AA55AA
 *   HEADER  : 12 bytes  [imageSize:u32] [imageCrc:u32] [targetBank:u32]
 *   DATA    : 264 bytes [seqNum:u32] [chunkCrc:u32] [data:256 bytes]
 *
 * Protocol frames (AURIX → host):
 *   ACK     : 4 bytes  0x06060606
 *   NAK     : 4 bytes  0x15151515  (followed by 1-byte error code)
 *
 * Flow:
 *   1. Host sends SYNC, AURIX replies ACK
 *   2. Host sends HEADER, AURIX validates, erases inactive bank, replies ACK
 *   3. Host sends DATA chunks in sequence, AURIX programs each page, replies ACK per chunk
 *   4. After last chunk, AURIX verifies full-image CRC, writes SOTA metadata, writes UCB_SWAP, replies ACK
 *   5. AURIX triggers system reset
 *
 * The protocol is driven by FwUpdate_Run() called from the main loop.
 */

#ifndef FWUPDATE_H
#define FWUPDATE_H

#include "Ifx_Types.h"

/* ------------------------------------------------------------------ */
/*  Protocol constants                                                */
/* ------------------------------------------------------------------ */
#define FWUPDATE_SYNC_MAGIC     0x55AA55AAu
#define FWUPDATE_ACK_MAGIC      0x06060606u
#define FWUPDATE_NAK_MAGIC      0x15151515u

#define FWUPDATE_CHUNK_SIZE     256u    /* matches PFLASH_PAGE_SIZE   */

/** Timeout waiting for a DATA chunk after ACK (ms) */
#ifndef FWUPDATE_CHUNK_TIMEOUT_MS
#define FWUPDATE_CHUNK_TIMEOUT_MS   5000u
#endif

/* ------------------------------------------------------------------ */
/*  State machine states                                              */
/* ------------------------------------------------------------------ */
typedef enum
{
    FWUPDATE_IDLE       = 0,    /* waiting for SYNC                  */
    FWUPDATE_HEADER     = 1,    /* waiting for HEADER                */
    FWUPDATE_ERASING    = 2,    /* erasing inactive bank             */
    FWUPDATE_RECEIVING  = 3,    /* receiving + programming chunks    */
    FWUPDATE_VERIFYING  = 4,    /* verifying full-image CRC          */
    FWUPDATE_COMMITTING = 5,    /* writing SOTA meta + UCB_SWAP      */
    FWUPDATE_DONE       = 6,    /* success, about to reset           */
    FWUPDATE_ERROR      = 7,    /* fatal error, session aborted      */
} FwUpdate_State_t;

/* ------------------------------------------------------------------ */
/*  Error codes (sent as NAK payload)                                 */
/* ------------------------------------------------------------------ */
typedef enum
{
    FWUPDATE_ERR_NONE       = 0,
    FWUPDATE_ERR_BAD_SYNC   = 1,
    FWUPDATE_ERR_BAD_HEADER = 2,
    FWUPDATE_ERR_IMG_TOO_BIG = 3,
    FWUPDATE_ERR_ERASE_FAIL = 4,
    FWUPDATE_ERR_SEQ_NUM    = 5,
    FWUPDATE_ERR_CHUNK_CRC  = 6,
    FWUPDATE_ERR_WRITE_FAIL = 7,
    FWUPDATE_ERR_IMG_CRC    = 8,
    FWUPDATE_ERR_SWAP_FAIL  = 9,
    FWUPDATE_ERR_TIMEOUT    = 10,
    FWUPDATE_ERR_META_FAIL  = 11,
} FwUpdate_Error_t;

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

/**
 * @brief  Initialise the firmware update module.
 *
 * Resets the state machine to IDLE.  Does NOT init the UART —
 * call UartXfer_Init() separately before this.
 */
void FwUpdate_Init(void);

/**
 * @brief  Run one iteration of the update state machine.
 *
 * Call from the main loop.  Non-blocking in most states (returns
 * immediately if no data is available).  The ERASING state may
 * block for several seconds while the inactive bank is erased.
 *
 * @return Current state after this iteration.
 */
FwUpdate_State_t FwUpdate_Run(void);

/**
 * @brief  Get the current state.
 */
FwUpdate_State_t FwUpdate_GetState(void);

/**
 * @brief  Get the last error code (valid when state == ERROR).
 */
FwUpdate_Error_t FwUpdate_GetError(void);

/**
 * @brief  Abort an in-progress update and return to IDLE.
 */
void FwUpdate_Abort(void);

#endif /* FWUPDATE_H */