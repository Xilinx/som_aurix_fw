/**
 * @file    NvLog.c
 * @brief   Non-volatile event logging to DFlash0
 *
 * See NvLog.h for architecture description and DFlash layout.
 */

#include "NvLog.h"
#include "DFlash.h"
#include "Stm_Timer.h"
#include "Uart_Debug.h"
#include <string.h>

/* ================================================================== */
/*  Internal constants                                                */
/* ================================================================== */



/* Byte offset within a slot where events start (after header) */
#define EVENTS_OFFSET       HEADER_SIZE

/* ================================================================== */
/*  Driver state                                                      */
/* ================================================================== */

static boolean          s_initialised   = FALSE;
static uint8            s_activeSlot    = 0u;
static uint32           s_bootCounter   = 0u;
static uint32           s_writeOffset   = 0u;   /* Byte offset into active slot for next event */
static uint32           s_eventsInSlot  = 0u;   /* Events written to DFlash in active slot */
static uint8            s_seqNum        = 0u;

/* RAM event buffer (ring) */
static NvLog_Event_t    s_ramBuf[NVLOG_RAM_BUF_COUNT];
static uint32           s_ramHead       = 0u;   /* Next write position */
static uint32           s_ramCount      = 0u;   /* Events in buffer */

/* Flush timing */
static uint32           s_lastFlushMs   = 0u;

/* Statistics */
static uint32           s_totalLogged   = 0u;
static uint32           s_flushCount    = 0u;
static uint32           s_flushErrors   = 0u;
/* ================================================================== */
/*  Private helpers                                                   */
/* ================================================================== */

typedef char NvLog_AssertEvt32[(sizeof(NvLog_Event_t)   == 32u) ? 1 : -1];
typedef char NvLog_AssertHdr32[(sizeof(NvLog_SlotHeader_t) == 32u) ? 1 : -1];

/** Get the DFlash base address for a given slot index. */
static uint32 prv_SlotAddr(uint8 slotIdx)
{
    return DFLASH_CRASHLOG_ADDR + ((uint32)slotIdx * SLOT_SIZE);
}

/** Compute XOR checksum over the first 28 bytes of an event. */
static uint32 prv_Checksum(const NvLog_Event_t *pEvt)
{
    const uint32 *w = (const uint32 *)pEvt;
    uint32 sum = 0u;
    uint32 i;
    for (i = 0u; i < (sizeof(NvLog_Event_t) / 4u); i++)
    {
        if (&w[i] != &pEvt->checksum)      /* skip the field, wherever it is */
            sum ^= w[i];
    }
    return sum;
}

/** Read a slot header from DFlash. */
static NvLog_Status_t prv_ReadSlotHeader(uint8 slotIdx, NvLog_SlotHeader_t *pHdr)
{
    uint32 addr = prv_SlotAddr(slotIdx);
    DFlash_Status_t ds = DFlash_Read(addr, pHdr, HEADER_SIZE);
    return (ds == DFLASH_OK) ? NVLOG_OK : NVLOG_ERR_DFLASH;
}

/** Write a slot header to DFlash (erase the first sector, write header). */
static NvLog_Status_t prv_WriteSlotHeader(uint8 slotIdx, const NvLog_SlotHeader_t *pHdr)
{
    uint32 addr = prv_SlotAddr(slotIdx);
    DFlash_Status_t ds;

    /* Erase the first sector of the slot to update the header */
    ds = DFlash_EraseSectors(addr, 1u);
    if (ds != DFLASH_OK) return NVLOG_ERR_DFLASH;

    ds = DFlash_Write(addr, pHdr, HEADER_SIZE);
    return (ds == DFLASH_OK) ? NVLOG_OK : NVLOG_ERR_DFLASH;
}

/** Erase all sectors in a slot. */
static NvLog_Status_t prv_EraseSlot(uint8 slotIdx)
{
    uint32 addr = prv_SlotAddr(slotIdx);
    DFlash_Status_t ds = DFlash_EraseSectors(addr, SECTORS_PER_SLOT);
    return (ds == DFLASH_OK) ? NVLOG_OK : NVLOG_ERR_DFLASH;
}


/** Build an event record from parameters. */
static void prv_BuildEvent(NvLog_Event_t *pEvt, uint8 eventType, uint8 source,
                           uint8 severity, const uint32 *pData)
{
    pEvt->timestamp = Stm_GetTimeMs();
    pEvt->eventType = eventType;
    pEvt->severity  = severity;
    pEvt->source    = source;
    pEvt->seqNum    = s_seqNum++;

    if (pData != NULL_PTR)
    {
        pEvt->data[0] = pData[0];
        pEvt->data[1] = pData[1];
        pEvt->data[2] = pData[2];
        pEvt->data[3] = pData[3];
    }
    else
    {
        pEvt->data[0] = 0u;
        pEvt->data[1] = 0u;
        pEvt->data[2] = 0u;
        pEvt->data[3] = 0u;
    }

    pEvt->checksum = prv_Checksum(pEvt);
}

/** Get event type name for debug printing. */
static const char *prv_EventName(uint8 evt)
{
    switch (evt)
    {
        case NVLOG_EVT_BOOT:              return "BOOT";
        case NVLOG_EVT_SHUTDOWN_GRACEFUL: return "SHUT_GRACE";
        case NVLOG_EVT_SHUTDOWN_FORCED:   return "SHUT_FORCE";
        case NVLOG_EVT_SHUTDOWN_WDT:      return "SHUT_WDT";
        case NVLOG_EVT_SHUTDOWN_FAULT:    return "SHUT_FAULT";
        case NVLOG_EVT_SHUTDOWN_THERMAL:  return "SHUT_THERM";
        case NVLOG_EVT_STATE_CHANGE:      return "STATE";
        case NVLOG_EVT_RESET_COLD:        return "RST_COLD";
        case NVLOG_EVT_RESET_WARM:        return "RST_WARM";
        case NVLOG_EVT_FAULT_PG_TIMEOUT:  return "PG_TIMEOUT";
        case NVLOG_EVT_FAULT_PG_LOSS:     return "PG_LOSS";
        case NVLOG_EVT_FAULT_VOLTAGE:     return "VOLT_FAULT";
        case NVLOG_EVT_FAULT_THERMTRIP:   return "THERMTRIP";
        case NVLOG_EVT_FAULT_RETRY:       return "RETRY";
        case NVLOG_EVT_FAULT_LATCHOFF:    return "LATCHOFF";
        case NVLOG_EVT_THERMAL_WARN:      return "THERM_WARN";
        case NVLOG_EVT_THERMAL_ERROR:     return "THERM_ERR";
        case NVLOG_EVT_THERMAL_CLEAR:     return "THERM_CLR";
        case NVLOG_EVT_PMIC_FAULT:        return "PMIC_FAULT";
        case NVLOG_EVT_PMIC_WDT_MISS:     return "PMIC_WDT";
        case NVLOG_EVT_PMIC_SAFE_STATE:   return "PMIC_SS";
        case NVLOG_EVT_USBPD_ATTACH:      return "PD_ATTACH";
        case NVLOG_EVT_USBPD_DETACH:      return "PD_DETACH";
        case NVLOG_EVT_SLOT_SEALED:       return "SEALED";
        default:                          return "UNKNOWN";
    }
}


static NvLog_Status_t prv_WriteEventToDFlash(const NvLog_Event_t *evt)
{
    uint32 addr;

    if (s_eventsInSlot >= NVLOG_EVENTS_PER_SLOT)
        return NVLOG_ERR_FULL;

    addr = prv_SlotAddr(s_activeSlot) + NVLOG_HEADER_SIZE
         + (s_eventsInSlot * (uint32)sizeof(NvLog_Event_t));

    if (DFlash_Write(addr, evt, sizeof(NvLog_Event_t)) != DFLASH_OK)
    {
        uint32 peek[2];
        DFlash_Read(addr, peek, 8u);      /* same mechanism as header read */
        Debug_Printf("[NVLOG] wr fail: slot=%u n=%u addr=%08X flash=%08X %08X\r\n",
                     (unsigned)s_activeSlot, (unsigned)s_eventsInSlot,
                     (unsigned)addr, (unsigned)peek[0], (unsigned)peek[1]);
        return NVLOG_ERR_DFLASH;
    }

    s_eventsInSlot++;               /* sole cursor, advances on success only */
    return NVLOG_OK;
}

static void prv_ReadEvent(uint8 slot, uint32 idx, NvLog_Event_t *evt)
{
    uint32 addr = prv_SlotAddr(slot) + NVLOG_HEADER_SIZE
                + (idx * (uint32)sizeof(NvLog_Event_t));
    /* same read mechanism as prv_ReadSlotHeader — DFlash_Read or
     * direct memcpy from the mapped address, whichever that uses */
    (void)DFlash_Read(addr, evt, sizeof(NvLog_Event_t));
}

static uint32 prv_CountEventsInSlot(uint8 slot)
{
    uint32 n;
    for (n = 0u; n < NVLOG_EVENTS_PER_SLOT; n++)
    {
        NvLog_Event_t evt;
        prv_ReadEvent(slot, n, &evt);
        if (evt.eventType == 0u)
            break;                       /* first blank record */
        /* optional: verify checksum; stop on corrupt too */
    }
    return n;
}

/* ================================================================== */
/*  Public API                                                        */
/* ================================================================== */

NvLog_Status_t NvLog_Init(void)
{
    NvLog_SlotHeader_t hdr;
    uint32 highestBoot = 0u;
    uint8  bestSlot    = 0u;
    boolean foundValid = FALSE;
    uint8  i;

    Debug_Print("[NVLOG] Init: scanning slots...\r\n");

    /* Scan all 4 slots to find the one with the highest boot counter */
    for (i = 0u; i < SLOT_COUNT; i++)
    {
        if (prv_ReadSlotHeader(i, &hdr) == NVLOG_OK)
        {
            if (hdr.magic == NVLOG_SLOT_MAGIC)
            {
                Debug_Printf("[NVLOG]   Slot %u: boot=%u events=%u %s\r\n",
                             (unsigned)i, (unsigned)hdr.bootCounter,
                             (unsigned)hdr.eventCount,
                             (hdr.sealMagic == NVLOG_SLOT_SEALED) ? "SEALED" : "ACTIVE");

                if (hdr.bootCounter >= highestBoot)
                {
                    highestBoot = hdr.bootCounter;
                    bestSlot = i;
                    foundValid = TRUE;
                }
            }
            else
            {
                Debug_Printf("[NVLOG]   Slot %u: empty/invalid (magic=0x%08X)\r\n",
                             (unsigned)i, (unsigned)hdr.magic);
            }
        }
        else
        {
            Debug_Printf("[NVLOG]   Slot %u: read error\r\n", (unsigned)i);
        }
    }

    if (foundValid)
    {
        /* Check if the best slot was sealed — if so, advance to next */
        prv_ReadSlotHeader(bestSlot, &hdr);
        if (hdr.sealMagic == NVLOG_SLOT_SEALED)
        {
            s_activeSlot = (bestSlot + 1u) % SLOT_COUNT;
            s_bootCounter = highestBoot + 1u;
            Debug_Printf("[NVLOG] Last slot %u was sealed, advancing to %u\r\n",
                         (unsigned)bestSlot, (unsigned)s_activeSlot);
        }
        else
        {
            /* Unsealed slot with the highest boot counter —
             * previous boot didn't shut down cleanly.
             * Seal it and advance. */
            s_activeSlot = (bestSlot + 1u) % SLOT_COUNT;
            s_bootCounter = highestBoot + 1u;
            Debug_Printf("[NVLOG] Slot %u was not sealed (crash?), advancing to %u\r\n",
                         (unsigned)bestSlot, (unsigned)s_activeSlot);
        }
    }
    else
    {
        /* No valid slots — first boot ever */
        s_activeSlot = 0u;
        s_bootCounter = 1u;
        Debug_Print("[NVLOG] No valid slots found, starting fresh at slot 0\r\n");
    }

    /* Erase the new active slot */
    Debug_Printf("[NVLOG] Erasing slot %u...\r\n", (unsigned)s_activeSlot);
    if (prv_EraseSlot(s_activeSlot) != NVLOG_OK)
    {
        Debug_Print("[NVLOG] ERROR: failed to erase active slot\r\n");
        return NVLOG_ERR_DFLASH;
    }

    /* Write the header for the new active slot */
    {
        NvLog_SlotHeader_t newHdr;
        memset(&newHdr, 0, sizeof(newHdr));
        newHdr.magic       = NVLOG_SLOT_MAGIC;
        newHdr.bootCounter = s_bootCounter;
        newHdr.eventCount  = 0u;
        newHdr.sealMagic   = 0u;

        if (prv_WriteSlotHeader(s_activeSlot, &newHdr) != NVLOG_OK)
        {
            Debug_Print("[NVLOG] ERROR: failed to write slot header\r\n");
            return NVLOG_ERR_DFLASH;
        }
    }

    /* Set the write offset past the header */
    s_writeOffset  = EVENTS_OFFSET;
    s_eventsInSlot = 0u;
    s_ramHead      = 0u;
    s_ramCount     = 0u;
    s_seqNum       = 0u;
    s_totalLogged  = 0u;
    s_flushCount   = 0u;
    s_flushErrors  = 0u;
    s_lastFlushMs  = Stm_GetTimeMs();
    s_initialised  = TRUE;

    Debug_Printf("[NVLOG] Active: slot %u, boot #%u\r\n",
                 (unsigned)s_activeSlot, (unsigned)s_bootCounter);

    /* Log the boot event */
    NvLog_WriteU32(NVLOG_EVT_BOOT, NVLOG_SRC_SYSTEM, NVLOG_SEV_INFO,
                   s_bootCounter);

    return NVLOG_OK;
}

NvLog_Status_t NvLog_Write(uint8 eventType, uint8 source, uint8 severity,
                           const uint32 *pData)
{
    NvLog_Event_t evt;

    if (!s_initialised) return NVLOG_ERR_NOT_INIT;

    prv_BuildEvent(&evt, eventType, source, severity, pData);

    if (s_ramCount < NVLOG_RAM_BUF_COUNT)
    {
        s_ramBuf[s_ramHead] = evt;
        s_ramHead = (s_ramHead + 1u) % NVLOG_RAM_BUF_COUNT;
        s_ramCount++;
        s_totalLogged++;
    }
    else
    {
        /* Buffer full — flush immediately, then retry */
        NvLog_Flush();
        if (s_ramCount < NVLOG_RAM_BUF_COUNT)
        {
            s_ramBuf[s_ramHead] = evt;
            s_ramHead = (s_ramHead + 1u) % NVLOG_RAM_BUF_COUNT;
            s_ramCount++;
            s_totalLogged++;
        }
        else
        {
            /* Still full after flush — slot is full */
            return NVLOG_ERR_FULL;
        }
    }

    if (severity >= NVLOG_SEV_ERROR)
    {
        (void)NvLog_Flush();
    }

    return NVLOG_OK;
}

NvLog_Status_t NvLog_WriteU32(uint8 eventType, uint8 source, uint8 severity,
                              uint32 value)
{
    uint32 data[4] = { value, 0u, 0u, 0u };
    return NvLog_Write(eventType, source, severity, data);
}
#ifndef NVLOG_FLUSH_RETRY_MAX
#define NVLOG_FLUSH_RETRY_MAX   3u   /* attempts per event before dropping it */
#endif

NvLog_Status_t NvLog_Flush(void)
{
    uint32 tail;
    uint32 count;
    uint32 i;
    uint32 written = 0u;
    NvLog_Status_t status = NVLOG_OK;

    static uint32 s_retryCount = 0u;   /* consecutive failures on the same
                                        * oldest event across Flush calls  */

    if (!s_initialised) return NVLOG_ERR_NOT_INIT;
    if (s_ramCount == 0u) return NVLOG_OK;

    count = s_ramCount;

    /* Oldest pending event position (head is next-free, so oldest is
     * head - count, modulo buffer size) */
    if (s_ramHead >= count)
        tail = s_ramHead - count;
    else
        tail = NVLOG_RAM_BUF_COUNT - (count - s_ramHead);

    for (i = 0u; i < count; i++)
    {
        uint32 idx = (tail + i) % NVLOG_RAM_BUF_COUNT;

        status = prv_WriteEventToDFlash(&s_ramBuf[idx]);
        if (status != NVLOG_OK)
        {
            s_flushErrors++;
            Debug_Printf("[NVLOG] Flush error at event %u: %u\r\n",
                         (unsigned)i, (unsigned)status);
            break;
        }
        written++;
    }

    /* ---- Book-keeping: keep what didn't make it to flash ---- */

    if (status == NVLOG_ERR_FULL)
    {
        /* Slot exhausted: nothing pending can ever be written this
         * power cycle. Dropping is a decision, made visibly. */
        Debug_Printf("[NVLOG] slot full, dropping %u pending events\r\n",
                     (unsigned)(count - written));
        s_ramCount   = 0u;
        s_ramHead    = 0u;
        s_retryCount = 0u;
    }
    else if (status != NVLOG_OK)
    {
        /* Transient/unknown write error: retain the failed event and
         * everything after it; retry on the next Flush. */
        s_ramCount -= written;              /* survivors stay; head unchanged,
                                             * so tail recomputes correctly
                                             * next call                     */
        if (written == 0u)
        {
            /* No progress at all — same oldest event failing repeatedly */
            s_retryCount++;
            if (s_retryCount >= NVLOG_FLUSH_RETRY_MAX)
            {
                Debug_Printf("[NVLOG] dropping unwritable event after %u tries\r\n",
                             (unsigned)s_retryCount);
                s_ramCount--;               /* skip the poisoned oldest event */
                s_retryCount = 0u;
            }
        }
        else
        {
            s_retryCount = 0u;              /* progress was made — reset */
        }
    }
    else
    {
        /* Clean flush */
        s_ramCount   = 0u;
        s_ramHead    = 0u;
        s_retryCount = 0u;
    }

    /* NOTE: no slot-header rewrite. eventCount in flash is dead — the
     * on-flash count is reconstructed by prv_CountEventsInSlot() at
     * init, and s_eventsInSlot (advanced inside prv_WriteEventToDFlash
     * on success only) is authoritative at runtime. Sealing is an
     * appended NVLOG_EVT_SLOT_SEALED record, not a header field.       */

    s_flushCount++;
    s_lastFlushMs = Stm_GetTimeMs();

    return status;
}

void NvLog_Run(void)
{
    if (!s_initialised) return;

    if ((Stm_GetTimeMs() - s_lastFlushMs) >= NVLOG_FLUSH_INTERVAL_MS)
    {
        if (s_ramCount > 0u)
        {
            NvLog_Flush();
        }
        else
        {
            s_lastFlushMs = Stm_GetTimeMs();
        }
    }
}

NvLog_Status_t NvLog_SealSlot(uint8 shutdownType)
{
    NvLog_SlotHeader_t hdr;
    uint8 nextSlot;

    if (!s_initialised) return NVLOG_ERR_NOT_INIT;

    /* Flush any remaining RAM events */
    NvLog_Flush();

    /* Write the seal event as the last entry */
    {
        uint32 data[4] = { (uint32)shutdownType, s_eventsInSlot, 0u, 0u };
        NvLog_Event_t sealEvt;
        prv_BuildEvent(&sealEvt, NVLOG_EVT_SLOT_SEALED, NVLOG_SRC_SYSTEM,
                       NVLOG_SEV_INFO, data);
        prv_WriteEventToDFlash(&sealEvt);
    }

    /* Update the slot header with seal info */
    if (prv_ReadSlotHeader(s_activeSlot, &hdr) == NVLOG_OK)
    {
        hdr.eventCount    = s_eventsInSlot;
        hdr.sealMagic     = NVLOG_SLOT_SEALED;
        hdr.sealTimestamp  = Stm_GetTimeMs();
        hdr.shutdownType   = (uint32)shutdownType;
        prv_WriteSlotHeader(s_activeSlot, &hdr);
    }

    Debug_Printf("[NVLOG] Slot %u sealed (%u events)\r\n",
                 (unsigned)s_activeSlot, (unsigned)s_eventsInSlot);

    /* Pre-erase the next slot so it's ready on next boot */
    nextSlot = (s_activeSlot + 1u) % SLOT_COUNT;
    prv_EraseSlot(nextSlot);

    return NVLOG_OK;
}

NvLog_Status_t NvLog_ReadSlot(uint8 slotIdx, NvLog_Event_t *pEvents,
                              uint32 maxEvents, uint32 *pCount)
{
    NvLog_SlotHeader_t hdr;
    uint32 addr;
    uint32 toRead;
    uint32 i;

    if (slotIdx >= SLOT_COUNT) return NVLOG_ERR_PARAM;
    if (pEvents == NULL_PTR || pCount == NULL_PTR) return NVLOG_ERR_PARAM;

    *pCount = 0u;

    if (prv_ReadSlotHeader(slotIdx, &hdr) != NVLOG_OK)
        return NVLOG_ERR_DFLASH;

    if (hdr.magic != NVLOG_SLOT_MAGIC)
        return NVLOG_OK;  /* Empty slot, count=0 */

    toRead = (hdr.eventCount < maxEvents) ? hdr.eventCount : maxEvents;
    addr = prv_SlotAddr(slotIdx) + EVENTS_OFFSET;

    for (i = 0u; i < toRead; i++)
    {
        DFlash_Status_t ds = DFlash_Read(addr + (i * EVENT_SIZE),
                                         &pEvents[i], EVENT_SIZE);
        if (ds != DFLASH_OK)
            return NVLOG_ERR_DFLASH;

        /* Verify checksum */
        if (pEvents[i].checksum != prv_Checksum(&pEvents[i]))
        {
            /* Corrupted event — stop here */
            *pCount = i;
            return NVLOG_OK;
        }
    }

    *pCount = toRead;
    return NVLOG_OK;
}

void NvLog_GetStats(NvLog_Stats_t *pStats)
{
    if (pStats == NULL_PTR) return;

    pStats->activeSlot       = s_activeSlot;
    pStats->bootCounter      = s_bootCounter;
    pStats->eventsInSlot     = s_eventsInSlot;
    pStats->eventsBuffered   = s_ramCount;
    pStats->totalEventsLogged = s_totalLogged;
    pStats->flushCount       = s_flushCount;
    pStats->flushErrors      = s_flushErrors;
}

void NvLog_DumpSlotInfo(void)
{
    NvLog_SlotHeader_t hdr;
    uint8 i;

    Debug_Print("[NVLOG] === Slot Summary ===\r\n");

    for (i = 0u; i < SLOT_COUNT; i++)
    {
        if (prv_ReadSlotHeader(i, &hdr) == NVLOG_OK && hdr.magic == NVLOG_SLOT_MAGIC)
        {
            uint32 evCount = (i == s_activeSlot)
                           ? s_eventsInSlot
                           : prv_CountEventsInSlot(i);

            Debug_Printf("[NVLOG] Slot %u: boot=%u events=%u %s%s\r\n",
                         (unsigned)i,
                         (unsigned)hdr.bootCounter,
                         (unsigned)evCount,
                         (hdr.sealMagic == NVLOG_SLOT_SEALED) ? "SEALED" : "OPEN",
                         (i == s_activeSlot) ? " <-- ACTIVE" : "");
        }
        else
        {
            Debug_Printf("[NVLOG] Slot %u: empty\r\n", (unsigned)i);
        }
    }

    Debug_Printf("[NVLOG] RAM buffer: %u/%u events pending\r\n",
                 (unsigned)s_ramCount, (unsigned)NVLOG_RAM_BUF_COUNT);
    Debug_Printf("[NVLOG] Session: %u logged, %u flushed, %u errors\r\n",
                 (unsigned)s_totalLogged, (unsigned)s_flushCount,
                 (unsigned)s_flushErrors);
}

void NvLog_DumpRecent(uint8 slot, uint32 count)
{
    if (!s_initialised) { Debug_Print("[NVLOG] not init\r\n"); return; }

    if (slot == NVLOG_SLOT_ACTIVE)
        slot = s_activeSlot;
    if (slot >= SLOT_COUNT)
        { Debug_Print("[NVLOG] bad slot\r\n"); return; }

    uint32 total = (slot == s_activeSlot) ? s_eventsInSlot
                                          : prv_CountEventsInSlot(slot);
    if (total == 0u)
    {
        Debug_Printf("[NVLOG] Slot %u: no events\r\n", (unsigned)slot);
        return;
    }
    if (count > total) count = total;
    uint32 start = total - count;

    {
        NvLog_SlotHeader_t hdr;
        (void)prv_ReadSlotHeader(slot, &hdr);
        Debug_Printf("[NVLOG] Last %u events (slot %u, boot %u):\r\n",
                     (unsigned)count, (unsigned)slot, (unsigned)hdr.bootCounter);
    }

    for (uint32 i = start; i < total; i++)
    {
        NvLog_Event_t evt;
        prv_ReadEvent(slot, i, &evt);

        if (evt.checksum != prv_Checksum(&evt))
        {
            Debug_Printf("[NVLOG]   #%u: CORRUPTED\r\n", (unsigned)i);
            continue;
        }

        Debug_Printf("[NVLOG]   #%u t=%ums %-12s sev=%u src=%u d0=%08X d1=%08X d2=%08X d3=%08X\r\n",
                     (unsigned)i, (unsigned)evt.timestamp,
                     prv_EventName(evt.eventType),
                     (unsigned)evt.severity, (unsigned)evt.source,
                     (unsigned)evt.data[0], (unsigned)evt.data[1],
                     (unsigned)evt.data[2], (unsigned)evt.data[3]);
    }
}