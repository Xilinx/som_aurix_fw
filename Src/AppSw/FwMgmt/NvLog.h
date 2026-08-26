/**
 * @file    NvLog.h
 * @brief   Non-volatile event logging to DFlash0
 *
 * Implements FW-MGMT-09 / PMC-DIA-004: 4× 32KB rotating crash-log
 * capture buffers in DFlash0.  Events are buffered in RAM and flushed
 * to DFlash periodically or on demand.
 *
 * DFlash0 layout (from DFlash.h):
 *   0xAF000000  SOTA metadata       4 KB   (1 sector)
 *   0xAF001000  Crash log slot 0   32 KB   (8 sectors)
 *   0xAF009000  Crash log slot 1   32 KB   (8 sectors)
 *   0xAF011000  Crash log slot 2   32 KB   (8 sectors)
 *   0xAF019000  Crash log slot 3   32 KB   (8 sectors)
 *   0xAF021000  USB carrier config 16 KB   (4 sectors)
 *
 * Each slot layout:
 *   Offset 0x0000: NvLog_SlotHeader_t  (32 bytes)
 *   Offset 0x0020: Event records       (32 bytes each)
 *   ...
 *   Offset 0x7FFF: End of slot
 *
 * Slot capacity: (32768 - 32) / 32 = 1023 events per slot.
 *
 * The active slot index is stored in the first sector of the
 * crash log region as part of the slot header.  On boot, NvLog_Init
 * scans all four slot headers to find the one with the highest
 * boot counter — that's the active slot.
 *
 * Write strategy:
 *   - Events are buffered in a RAM ring buffer (NVLOG_RAM_BUF_COUNT)
 *   - NvLog_Flush() writes buffered events to the active DFlash slot
 *   - NvLog_Flush() is called periodically from the main loop
 *   - NvLog_SealSlot() is called on emergency/unexpected shutdown:
 *     flushes remaining events, marks the slot as sealed, advances
 *     to the next slot (erasing it if needed)
 *
 * The implementation does NOT migrate logs between slots.
 * Old slots are preserved until overwritten by rotation.
 */

#ifndef NVLOG_H
#define NVLOG_H

#include "Ifx_Types.h"
#include "DFlash.h"

/* ================================================================== */
/*  Event types                                                       */
/* ================================================================== */
typedef enum
{
    NVLOG_EVT_BOOT              = 0x01u,    /* System boot */
    NVLOG_EVT_SHUTDOWN_GRACEFUL = 0x02u,    /* Graceful shutdown */
    NVLOG_EVT_SHUTDOWN_FORCED   = 0x03u,    /* Forced shutdown (long press) */
    NVLOG_EVT_SHUTDOWN_WDT      = 0x04u,    /* Watchdog-induced shutdown */
    NVLOG_EVT_SHUTDOWN_FAULT    = 0x05u,    /* Fault-induced shutdown */
    NVLOG_EVT_SHUTDOWN_THERMAL  = 0x06u,    /* Thermal shutdown */
    NVLOG_EVT_SHUTDOWN_OPERATOR = 0x07u,    /* Operator-requested shutdown */

    NVLOG_EVT_STATE_CHANGE      = 0x10u,    /* PM state transition */
    NVLOG_EVT_RESET_COLD        = 0x11u,    /* Cold reset */
    NVLOG_EVT_RESET_WARM        = 0x12u,    /* Warm reset */
    NVLOG_EVT_RESET_CF9         = 0x13u,    /* CF9 cold reset */

    NVLOG_EVT_FAULT_PG_TIMEOUT  = 0x20u,    /* Power good timeout */
    NVLOG_EVT_FAULT_PG_LOSS     = 0x21u,    /* Power good loss */
    NVLOG_EVT_FAULT_VOLTAGE     = 0x22u,    /* Voltage out of range */
    NVLOG_EVT_FAULT_THERMTRIP   = 0x23u,    /* THERMTRIP# asserted */
    NVLOG_EVT_FAULT_BIOS        = 0x24u,    /* BIOS validation fail */
    NVLOG_EVT_FAULT_RETRY       = 0x25u,    /* Retry attempt */
    NVLOG_EVT_FAULT_LATCHOFF    = 0x26u,    /* Latch-off after retries exhausted */

    NVLOG_EVT_THERMAL_WARN      = 0x30u,    /* Temperature warning threshold */
    NVLOG_EVT_THERMAL_ERROR     = 0x31u,    /* Temperature error threshold */
    NVLOG_EVT_THERMAL_CLEAR     = 0x32u,    /* Temperature dropped below hysteresis */
    NVLOG_EVT_THERMAL_APML_FAIL = 0x33u,    /* APML I2C read failure */

    NVLOG_EVT_PMIC_FAULT        = 0x40u,    /* TLF PMIC fault */
    NVLOG_EVT_PMIC_WDT_MISS     = 0x41u,    /* TLF WDT service missed */
    NVLOG_EVT_PMIC_SAFE_STATE   = 0x42u,    /* TLF entered safe state */
    NVLOG_EVT_PMIC_STATE_CHANGE = 0x43u,    /* TLF state transition */

    NVLOG_EVT_USBPD_ATTACH      = 0x50u,    /* USB-C attach */
    NVLOG_EVT_USBPD_DETACH      = 0x51u,    /* USB-C detach */
    NVLOG_EVT_USBPD_CONTRACT    = 0x52u,    /* PD contract negotiated */
    NVLOG_EVT_USBPD_FAULT       = 0x53u,    /* USB PD fault */
    NVLOG_EVT_USBPD_HPD         = 0x54u,    /* HPD event */

    NVLOG_EVT_MISC_RAPID_SHDN   = 0x60u,    /* RAPID_SHUTDOWN state change */
    NVLOG_EVT_MISC_LID          = 0x61u,    /* LID# state change */
    NVLOG_EVT_MISC_TAMPER       = 0x62u,    /* TAMPER# state change */

    NVLOG_EVT_COMHPC_WDT        = 0x70u,    /* COM-HPC watchdog event */

    NVLOG_EVT_FWUPDATE_START    = 0x80u,    /* FW update started */
    NVLOG_EVT_FWUPDATE_OK       = 0x81u,    /* FW update completed */
    NVLOG_EVT_FWUPDATE_FAIL     = 0x82u,    /* FW update failed */

    NVLOG_EVT_SLOT_SEALED       = 0xFEu,    /* Slot sealed (last event in slot) */
    NVLOG_EVT_MARKER            = 0xFFu,    /* Placeholder / marker */
} NvLog_EventType_t;

/* ================================================================== */
/*  Event severity                                                    */
/* ================================================================== */
typedef enum
{
    NVLOG_SEV_INFO    = 0u,
    NVLOG_SEV_WARNING = 1u,
    NVLOG_SEV_ERROR   = 2u,
    NVLOG_SEV_FATAL   = 3u,
} NvLog_Severity_t;

/* ================================================================== */
/*  Event source module                                               */
/* ================================================================== */
typedef enum
{
    NVLOG_SRC_SYSTEM    = 0u,
    NVLOG_SRC_POWER     = 1u,
    NVLOG_SRC_THERMAL   = 2u,
    NVLOG_SRC_VOLTAGE   = 3u,
    NVLOG_SRC_PMIC      = 4u,
    NVLOG_SRC_USBPD     = 5u,
    NVLOG_SRC_FWUPDATE  = 6u,
    NVLOG_SRC_COMHPC    = 7u,
    NVLOG_SRC_DEBUG     = 8u,
} NvLog_Source_t;

/* ================================================================== */
/*  Event record (32 bytes — aligned for DFlash write)                */
/* ================================================================== */
typedef struct
{
    uint32 timestamp;       /**< ms since boot */
    uint8  eventType;
    uint8  severity;
    uint8  source;
    uint8  seqNum;
    uint32 data[4];         /**< 16-byte payload */
    uint32 checksum;        /**< XOR over first 24 bytes */
    uint32 reserved;        /**< pad to 32 for DFlash 8-byte page alignment */
} NvLog_Event_t;   

/* ================================================================== */
/*  Slot header (32 bytes — first record in each 32KB slot)           */
/* ================================================================== */
#define NVLOG_SLOT_MAGIC        0x4E564C47u     /* "NVLG" */
#define NVLOG_SLOT_SEALED       0x5345414Cu     /* "SEAL" */

typedef struct
{
    uint32 magic;           /**< NVLOG_SLOT_MAGIC when active */
    uint32 bootCounter;     /**< Incremented each boot cycle */
    uint32 eventCount;      /**< Number of events written to this slot */
    uint32 sealMagic;       /**< NVLOG_SLOT_SEALED when slot is closed */
    uint32 sealTimestamp;   /**< Timestamp when slot was sealed */
    uint32 shutdownType;    /**< NvLog_EventType_t of the shutdown that sealed it */
    uint32 reserved[2];
} NvLog_SlotHeader_t;       /* 32 bytes */

/* ================================================================== */
/*  Configuration                                                     */
/* ================================================================== */

/** Number of events buffered in RAM before auto-flush */
#ifndef NVLOG_RAM_BUF_COUNT
#define NVLOG_RAM_BUF_COUNT         32u
#endif

/** Auto-flush interval (ms) — flushed from main loop */
#ifndef NVLOG_FLUSH_INTERVAL_MS
#define NVLOG_FLUSH_INTERVAL_MS     5000u
#endif

#define SLOT_COUNT          DFLASH_CRASHLOG_NUM_SLOTS    /* 4 */
#define SLOT_SIZE           DFLASH_CRASHLOG_SLOT_SIZE    /* 32 KB */
#define SECTOR_SIZE         DFLASH0_SECTOR_SIZE          /* 4 KB */
#define SECTORS_PER_SLOT    (SLOT_SIZE / SECTOR_SIZE)    /* 8 */
#define HEADER_SIZE         sizeof(NvLog_SlotHeader_t)   /* 32 */
#define EVENT_SIZE          sizeof(NvLog_Event_t)        /* 32 */

#define NVLOG_HEADER_SIZE   ((uint32)sizeof(NvLog_SlotHeader_t))

/** Maximum events per slot: (32768 - 32) / 32 = 1023 */
#define NVLOG_EVENTS_PER_SLOT  ((SLOT_SIZE - NVLOG_HEADER_SIZE) \
                                / (uint32)sizeof(NvLog_Event_t))


#define NVLOG_SLOT_ACTIVE   0xFFu
/* ================================================================== */
/*  Status                                                            */
/* ================================================================== */
typedef enum
{
    NVLOG_OK            = 0u,
    NVLOG_ERR_DFLASH    = 1u,
    NVLOG_ERR_FULL      = 2u,   /* Active slot is full */
    NVLOG_ERR_NOT_INIT  = 3u,
    NVLOG_ERR_PARAM     = 4u,
} NvLog_Status_t;

/* ================================================================== */
/*  Statistics (for debug CLI)                                        */
/* ================================================================== */
typedef struct
{
    uint8  activeSlot;          /**< Currently active slot (0-3) */
    uint32 bootCounter;         /**< Current boot counter */
    uint32 eventsInSlot;        /**< Events written to active slot this session */
    uint32 eventsBuffered;      /**< Events in RAM buffer pending flush */
    uint32 totalEventsLogged;   /**< Total events logged this session */
    uint32 flushCount;          /**< Number of flush operations */
    uint32 flushErrors;         /**< Number of failed flush attempts */
} NvLog_Stats_t;

/* ================================================================== */
/*  Public API                                                        */
/* ================================================================== */

/**
 * @brief  Initialise the NV log system.
 *
 * Scans all 4 slot headers to find the active slot (highest boot
 * counter).  If no valid slot is found, erases slot 0 and starts
 * fresh.  Increments the boot counter and writes a BOOT event.
 *
 * Must be called after DFlash_Init().
 */
NvLog_Status_t NvLog_Init(void);

/**
 * @brief  Log an event.
 *
 * Appends to the RAM buffer.  If the buffer is full, triggers
 * an immediate flush.  Non-blocking in the common case.
 *
 * @param  eventType  Event type code.
 * @param  source     Source module.
 * @param  severity   Event severity.
 * @param  pData      Optional 16-byte payload (4× uint32).
 *                    Pass NULL for events with no payload.
 */
NvLog_Status_t NvLog_Write(uint8 eventType, uint8 source, uint8 severity,
                           const uint32 *pData);

/**
 * @brief  Convenience: log with a single uint32 payload.
 */
NvLog_Status_t NvLog_WriteU32(uint8 eventType, uint8 source, uint8 severity,
                              uint32 value);

/**
 * @brief  Flush buffered events to DFlash.
 *
 * Called periodically from the main loop (every NVLOG_FLUSH_INTERVAL_MS).
 * Also called explicitly before shutdown or when the buffer is full.
 */
NvLog_Status_t NvLog_Flush(void);

/**
 * @brief  Periodic service — call from main loop.
 *
 * Handles auto-flush timing.  Non-blocking if no flush is due.
 */
void NvLog_Run(void);

/**
 * @brief  Seal the active slot and advance to the next.
 *
 * Called on emergency/unexpected shutdown.  Flushes remaining
 * events, writes a SLOT_SEALED event, marks the header, and
 * erases the next slot so it's ready for the next boot.
 *
 * @param  shutdownType  The event type that triggered the seal
 *                       (e.g. NVLOG_EVT_SHUTDOWN_FAULT).
 */
NvLog_Status_t NvLog_SealSlot(uint8 shutdownType);

/**
 * @brief  Read events from a specific slot.
 *
 * For the debug CLI to dump logged events.
 *
 * @param  slotIdx    Slot index (0-3).
 * @param  pEvents    Output buffer for events.
 * @param  maxEvents  Maximum events to read.
 * @param  pCount     Output: actual number of events read.
 */
NvLog_Status_t NvLog_ReadSlot(uint8 slotIdx, NvLog_Event_t *pEvents,
                              uint32 maxEvents, uint32 *pCount);

/**
 * @brief  Get current statistics.
 */
void NvLog_GetStats(NvLog_Stats_t *pStats);

/**
 * @brief  Print a summary of all slots to the debug UART.
 */
void NvLog_DumpSlotInfo(void);

/**
 * @brief  Print recent events from the active slot.
 *
 * @param  count  Number of most recent events to print (0 = all).
 */
void NvLog_DumpRecent(uint8 slot, uint32 maxEvents);



#endif /* NVLOG_H */