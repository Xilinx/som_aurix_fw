/**
 * @file    UsbPd_Manager.c
 * @brief   USB PD manager — dual-port event dispatch with APU proxy + HPD
 *
 * v0.2 expansion:
 *   - Reads full HPI register set (TYPE_C_STATUS, PD_STATUS, etc.)
 *   - Packs into AMD APU register format via UsbPd_ApuProxy
 *   - Dispatches HPD events to UsbPd_Hpd for GPIO pulse generation
 *   - Logs all events to NvLog
 */

#include "UsbPd_Manager.h"
#include "Cypd6129_Drv.h"
#include "UsbPd_Cfg.h"
#include "UsbPd_Hpi.h"
#include "UsbPd_ApuProxy.h"
#include "UsbPd_Hpd.h"
#include "I2c_Slave.h"
#include "NvLog.h"
#include "Stm_Timer.h"
#include "Uart_Debug.h"
#include <string.h>


#if (USBPD_FEATURE_ENABLE == 1u)

/* ================================================================== */
/*  State                                                             */
/* ================================================================== */

static UsbPd_PortState_t  s_portState[CYPD_DEVICE_COUNT];
static UsbPd_HpiState_t  s_hpiState[CYPD_DEVICE_COUNT];  /* v0.2: full HPI state */
static boolean            s_devPresent[CYPD_DEVICE_COUNT];
static uint8              s_devRuntimeFails[CYPD_DEVICE_COUNT];
static uint16             s_devSpuriousCount[CYPD_DEVICE_COUNT];

/* ================================================================== */
/*  Private helpers                                                   */
/* ================================================================== */

/**
 * Read full HPI state and update the APU proxy for this port.
 * Called after every event and periodically for state refresh.
 */
static void prv_RefreshHpiState(uint8 devIdx)
{
    const UsbPd_SysCfg_t *sysCfg = UsbPd_Cfg_GetSysCfg();
    uint8 i2cAddr;
    uint16 portBase;

    if (sysCfg->topology == USBPD_TOPO_DUAL_SINGLE_PORT)
    {
        /* Each CYPD6129 has one port at base 0x1000 */
        i2cAddr  = CYPD_DEVICES[devIdx].i2cAddr;
        portBase = 0x1000u;
    }
    else
    {
        /* CYPD6229: single controller, port base varies */
        i2cAddr  = CYPD_DEVICES[0].i2cAddr;
        portBase = (devIdx == 0u) ? 0x1000u : 0x2000u;
    }

    UsbPd_HpiState_t hpiState;
    if (UsbPd_Hpi_ReadPortState(i2cAddr, portBase, &hpiState) == 0u)
    {
        s_hpiState[devIdx] = hpiState;

        /* Pack into AMD APU register format */
        UsbPd_ApuProxy_Pack(devIdx, &hpiState, NULL_PTR);
    }
}

/* Most events we can read in one service call before the loop re-checks. */
#define USBPD_MAX_EVENTS_PER_SVC    8u

/**
 * Act on a single decoded HPI message (one RESPONSE / PD_RESPONSE entry).
 * In Phase 1 we recognise and log every message code so attach / detach /
 * contract and the AMD embedded events are visible in the log. The full
 * state-machine reactions to the AMD events (0xE0-0xE3, 0xC0) are built in
 * Phases 3, 5 and 6; here they are decoded and traced only.
 */
static void prv_HandleMessage(uint8 devIdx, const Cypd_Event_t *ev)
{
    const char *name = CYPD_DEVICES[devIdx].name;

    /* Command responses (Success/error) are consumed inside the driver; by
     * the time a message reaches us it should be an asynchronous event. */
    if (!ev->isEvent)
    {
        Debug_Printf("[USBPD %s] response code 0x%02X (%s)\r\n",
                     name, ev->code, Cypd_EventName(ev->code));
        return;
    }

    switch (ev->code)
    {
        case HPI_EVT_RESET_COMPLETE:
            /* The controller rebooted on its own; its HPI setup is gone. */
            Debug_Printf("[USBPD %s] RESET_COMPLETE - controller rebooted, "
                         "re-running HPI init\r\n", name);
            (void)Cypd_HpiInit(devIdx);
            break;

        case HPI_EVT_QUEUE_OVERFLOW:
            /* An event was dropped. A full status re-sync belongs to Phase 5;
             * for now we flag it so lost events are never silent. */
            Debug_Printf("[USBPD %s] WARNING: event queue overflow - an event "
                         "was dropped\r\n", name);
            NvLog_WriteU32(NVLOG_EVT_USBPD_FAULT, NVLOG_SRC_USBPD,
                           NVLOG_SEV_WARNING, ((uint32)devIdx << 8u) | 0x03u);
            break;

        case HPI_EVT_TYPEC_CONNECT:
            s_portState[devIdx] = USBPD_PORT_ATTACHED;
            Debug_Printf("[USBPD %s] Type-C CONNECT\r\n", name);
            {
                uint32 nvData[4] = { devIdx, USBPD_PORT_ATTACHED, 0u, 0u };
                NvLog_Write(NVLOG_EVT_USBPD_ATTACH, NVLOG_SRC_USBPD,
                            NVLOG_SEV_INFO, nvData);
            }
            break;

        case HPI_EVT_TYPEC_DISCONNECT:
            s_portState[devIdx] = USBPD_PORT_DETACHED;
            Debug_Printf("[USBPD %s] Type-C DISCONNECT\r\n", name);
            UsbPd_Hpd_ProcessEvent(devIdx, HPD_EVENT_LOW);  /* drop HPD */
            {
                uint32 nvData[4] = { devIdx, USBPD_PORT_DETACHED, 0u, 0u };
                NvLog_Write(NVLOG_EVT_USBPD_DETACH, NVLOG_SRC_USBPD,
                            NVLOG_SEV_INFO, nvData);
            }
            break;

        case HPI_EVT_PD_CONTRACT:
            s_portState[devIdx] = USBPD_PORT_CONTRACT;
            {
                Cypd_PortStatus_t status;
                if (Cypd_ReadPortStatus(devIdx, &status) == CYPD_OK)
                {
                    uint32 vbus_mV = (uint32)status.vbusVoltage_100mV * 100u;
                    Debug_Printf("[USBPD %s] PD CONTRACT: VBUS=%u mV, CC%u, "
                                 "%s, %s\r\n", name, (unsigned)vbus_mV,
                                 status.cc2Polarity ? 2u : 1u,
                                 status.isDfp ? "DFP" : "UFP",
                                 status.isSource ? "source" : "sink");

                    uint32 nvData[4] = { devIdx, vbus_mV,
                                         (uint32)status.cc2Polarity,
                                         (uint32)status.isDfp };
                    NvLog_Write(NVLOG_EVT_USBPD_CONTRACT, NVLOG_SRC_USBPD,
                                NVLOG_SEV_INFO, nvData);
                }
            }
            break;

        case HPI_EVT_OVER_CURRENT:
            Debug_Printf("[USBPD %s] WARNING: VBUS Over-Current!\r\n", name);
            NvLog_WriteU32(NVLOG_EVT_USBPD_FAULT, NVLOG_SRC_USBPD,
                           NVLOG_SEV_ERROR, ((uint32)devIdx << 8u) | 0x02u);
            break;

        case HPI_EVT_OVER_VOLTAGE:
            Debug_Printf("[USBPD %s] WARNING: VBUS Over-Voltage!\r\n", name);
            NvLog_WriteU32(NVLOG_EVT_USBPD_FAULT, NVLOG_SRC_USBPD,
                           NVLOG_SEV_ERROR, ((uint32)devIdx << 8u) | 0x01u);
            break;

        case HPI_EVT_DP_MODE_ENTERED:
        case HPI_EVT_DP_STATUS_UPDATE:
            Debug_Printf("[USBPD %s] DP event %s (0x%02X)\r\n",
                         name, Cypd_EventName(ev->code), ev->code);
            break;

        /* AMD embedded events: decoded and logged in Phase 1. The handshake
         * reactions arrive in later phases (0xE0 -> Phase 3, 0xC0 -> Phase 5,
         * 0xE1-0xE3 -> Phase 6). */
        case HPI_EVT_AMD_APU_REQUEST:
        case HPI_EVT_AMD_APU_TIMEOUT:
        case HPI_EVT_AMD_HPD_LOW_HIGH:
        case HPI_EVT_AMD_HPD_HIGH_LOW:
        case HPI_EVT_AMD_HPD_IRQ:
            Debug_Printf("[USBPD %s] AMD event %s (0x%02X), %u data byte(s)\r\n",
                         name, Cypd_EventName(ev->code), ev->code,
                         (unsigned)ev->dataLen);
            break;

        default:
            Debug_Printf("[USBPD %s] unhandled event 0x%02X\r\n",
                         name, ev->code);
            break;
    }

    /* Refresh the cached HPI/APU state after any event. */
    prv_RefreshHpiState(devIdx);
}

/**
 * Drain and dispatch every pending HPI message for one controller.
 * @param pServiced Output: number of messages handled (0 = spurious INT).
 * @return CYPD_OK if the service completed, or an I2C error.
 */
static Cypd_Status_t prv_ServiceDevice(uint8 devIdx, uint8 *pServiced)
{
    Cypd_Event_t evts[USBPD_MAX_EVENTS_PER_SVC];
    uint8 count = 0u;
    uint8 i;
    Cypd_Status_t st;

    *pServiced = 0u;
    st = Cypd_ServiceInterrupts(devIdx, evts, USBPD_MAX_EVENTS_PER_SVC, &count);
    if (st != CYPD_OK)
    {
        return st;
    }
    for (i = 0u; i < count; i++)
    {
        prv_HandleMessage(devIdx, &evts[i]);
    }
    *pServiced = count;
    return CYPD_OK;
}

/**
 * Attempt to bring one controller up via a hard reset, and on success mark it
 * present and prime its HPI/APU state. Clears the per-device fault counters so
 * a device recovered at runtime starts from a clean slate.
 * @return TRUE if the controller is present after the attempt.
 */
static boolean prv_BringUpDevice(uint8 devIdx)
{
    Cypd_Status_t st = Cypd_HardReset(devIdx);
    if (st != CYPD_OK)
    {
        s_devPresent[devIdx] = FALSE;
        Debug_Printf("[USBPD %s] INIT FAILED (err=%d) - polling disabled\r\n",
                     CYPD_DEVICES[devIdx].name, (int)st);
        return FALSE;
    }

    /* Run the HPIv2 bring-up handshake (drain Reset-Complete, program
     * EVENT_MASK, send EC-Init-Complete). Treat a failure here as a failed
     * bring-up so the periodic recovery path retries the whole sequence. */
    st = Cypd_HpiInit(devIdx);
    if (st != CYPD_OK)
    {
        s_devPresent[devIdx] = FALSE;
        Debug_Printf("[USBPD %s] HPI INIT FAILED (err=%d) - polling disabled\r\n",
                     CYPD_DEVICES[devIdx].name, (int)st);
        return FALSE;
    }

    s_devPresent[devIdx]       = TRUE;
    s_devRuntimeFails[devIdx]  = 0u;
    s_devSpuriousCount[devIdx] = 0u;
    Debug_Printf("[USBPD %s] Init OK\r\n", CYPD_DEVICES[devIdx].name);

    /* Initial HPI state read */
    prv_RefreshHpiState(devIdx);
    return TRUE;
}

/* ================================================================== */
/*  Public API                                                        */
/* ================================================================== */

void UsbPdManager_Init(void)
{
    UsbPd_CfgInit();

    /* v0.2: init HPD and APU proxy */
    UsbPd_Hpd_Init();
    UsbPd_ApuProxy_Init();

    uint8 i;

    for (i = 0u; i < CYPD_DEVICE_COUNT; i++)
    {
        s_portState[i]        = USBPD_PORT_DETACHED;
        s_devPresent[i]       = FALSE;
        s_devRuntimeFails[i]  = 0u;
        s_devSpuriousCount[i] = 0u;

        memset(&s_hpiState[i], 0, sizeof(UsbPd_HpiState_t));

        (void)prv_BringUpDevice(i);
    }
}

uint8 UsbPdManager_Reinit(void)
{
    uint8 i;
    uint8 present = 0u;

    for (i = 0u; i < CYPD_DEVICE_COUNT; i++)
    {
        if (!s_devPresent[i])
        {
            (void)prv_BringUpDevice(i);
        }
        if (s_devPresent[i])
        {
            present++;
        }
    }
    return present;
}

void UsbPdManager_Run(void)
{
    static uint32 s_lastPoll = 0u;
    static uint32 s_lastRefresh = 0u;
    static uint32 s_lastReinit = 0u;
    uint8 i;

    /* Recovery: periodically retry any controller that is not present, so a
     * device that comes up late or a transient I2C error no longer disables
     * USB PD until the next AURIX reset. */
    if (Stm_IsElapsedMs(&s_lastReinit, USBPD_REINIT_INTERVAL_MS))
    {
        for (i = 0u; i < CYPD_DEVICE_COUNT; i++)
        {
            if (!s_devPresent[i])
            {
                (void)prv_BringUpDevice(i);
            }
        }
    }

    if (!Stm_IsElapsedMs(&s_lastPoll, USBPD_MGR_POLL_INTERVAL_MS))
    {
        return;
    }

    for (i = 0u; i < CYPD_DEVICE_COUNT; i++)
    {
        if (!s_devPresent[i])
        {
            continue;
        }

        if (Cypd_IsIntAsserted(i))
        {
            uint8 serviced = 0u;

            /* Drain and dispatch every queued HPI message. */
            if (prv_ServiceDevice(i, &serviced) != CYPD_OK)
            {
                s_devRuntimeFails[i]++;
                if (s_devRuntimeFails[i] >= USBPD_RUNTIME_FAIL_LIMIT)
                {
                    s_devPresent[i] = FALSE;
                    Debug_Printf("[USBPD %s] I2C failed %u times - disabling\r\n",
                                 CYPD_DEVICES[i].name,
                                 (unsigned)s_devRuntimeFails[i]);
                    Cypd_RecoverBus(i);
                }
                continue;
            }

            s_devRuntimeFails[i] = 0u;

            /* INT_L asserted but nothing in the queue: count as spurious and
             * disable the port if it stays stuck low. */
            if (serviced == 0u)
            {
                s_devSpuriousCount[i]++;
                if (s_devSpuriousCount[i] >= USBPD_SPURIOUS_INT_LIMIT)
                {
                    s_devPresent[i] = FALSE;
                    Debug_Printf("[USBPD %s] INT_L stuck low (%u spurious) "
                                 "- disabling\r\n",
                                 CYPD_DEVICES[i].name,
                                 (unsigned)s_devSpuriousCount[i]);
                }
                continue;
            }

            s_devSpuriousCount[i] = 0u;
        }
        else
        {
            s_devSpuriousCount[i] = 0u;
        }
    }

    /* v0.2: periodic HPI state refresh (even without events, for APU proxy) */
    if (Stm_IsElapsedMs(&s_lastRefresh, 1000u))
    {
        for (i = 0u; i < CYPD_DEVICE_COUNT; i++)
        {
            if (s_devPresent[i])
            {
                prv_RefreshHpiState(i);
            }
        }
    }

    /* v0.2: HPD pulse timing */
    UsbPd_Hpd_Run();
}

UsbPd_PortState_t UsbPdManager_GetPortState(uint8 portIdx)
{
    if (portIdx >= CYPD_DEVICE_COUNT)
    {
        return USBPD_PORT_DETACHED;
    }
    return s_portState[portIdx];
}

uint32 UsbPdManager_GetPortStatusWord(uint8 portIdx)
{
    uint32 w;

    if (portIdx >= CYPD_DEVICE_COUNT)
    {
        return 0u;
    }

    w = (uint32)s_portState[portIdx] & USBPD_STAT_STATE_MASK;
    if (s_devPresent[portIdx])
    {
        w |= USBPD_STAT_DEV_PRESENT;
    }
    /* Per HPI spec Table 74, the current data/power role fields are valid
     * only while a PD contract exists (PD_STATUS b10). Gate the role bits on
     * the contract, not just on the Type-C connection. Alt modes stay gated on
     * the connection. */
    if (s_hpiState[portIdx].contractValid)
    {
        if ((s_hpiState[portIdx].pdStatus & HPI_PD_DATA_ROLE) != 0u)
            w |= USBPD_STAT_DATA_DFP;
        if ((s_hpiState[portIdx].pdStatus & HPI_PD_PORT_ROLE) != 0u)
            w |= USBPD_STAT_PWR_SOURCE;
    }
    if (s_hpiState[portIdx].connected)
    {
        if ((s_hpiState[portIdx].altModeStatus & HPI_ALT_DP_ACTIVE) != 0u)
            w |= USBPD_STAT_ALT_DP;
        if ((s_hpiState[portIdx].altModeStatus & HPI_ALT_TBT3_ACTIVE) != 0u)
            w |= USBPD_STAT_ALT_TBT3;
        if ((s_hpiState[portIdx].altModeStatus & HPI_ALT_USB4_ACTIVE) != 0u)
            w |= USBPD_STAT_ALT_USB4;
    }
    return w;
}

uint32 UsbPdManager_GetFaultFlags(void)
{
    uint32 f = 0u;
    uint8  i;

    for (i = 0u; i < CYPD_DEVICE_COUNT; i++)
    {
        if (!s_devPresent[i])           { f |= (USBPD_FAULT_DEV_ABSENT   << i); }
        if (s_devRuntimeFails[i] != 0u) { f |= (USBPD_FAULT_RUNTIME_FAIL << i); }
        if (s_devSpuriousCount[i] != 0u){ f |= (USBPD_FAULT_SPURIOUS_INT<< i); }
    }
    return f;
}

#else /* USBPD_FEATURE_ENABLE == 0 */

void UsbPdManager_Init(void)
{
    Debug_Print("[USBPD] Feature disabled at build time\r\n");
}

void UsbPdManager_Run(void) { }

UsbPd_PortState_t UsbPdManager_GetPortState(uint8 portIdx)
{
    (void)portIdx;
    return USBPD_PORT_DETACHED;
}

uint32 UsbPdManager_GetPortStatusWord(uint8 portIdx) { (void)portIdx; return 0u; }

uint32 UsbPdManager_GetFaultFlags(void) { return 0u; }

#endif