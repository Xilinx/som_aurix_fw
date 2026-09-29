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

static void prv_HandleEvent(uint8 devIdx, uint32 events)
{
    const char *name = CYPD_DEVICES[devIdx].name;

    if (events & CYPD_EVT_TYPEC_ATTACH)
    {
        s_portState[devIdx] = USBPD_PORT_ATTACHED;
        Debug_Printf("[USBPD %s] Type-C ATTACH\r\n", name);

        uint32 nvData[4] = { devIdx, USBPD_PORT_ATTACHED, 0u, 0u };
        NvLog_Write(NVLOG_EVT_USBPD_ATTACH, NVLOG_SRC_USBPD,
                    NVLOG_SEV_INFO, nvData);
    }

    if (events & CYPD_EVT_TYPEC_DETACH)
    {
        s_portState[devIdx] = USBPD_PORT_DETACHED;
        Debug_Printf("[USBPD %s] Type-C DETACH\r\n", name);

        /* Deassert HPD on detach */
        UsbPd_Hpd_ProcessEvent(devIdx, HPD_EVENT_LOW);

        uint32 nvData[4] = { devIdx, USBPD_PORT_DETACHED, 0u, 0u };
        NvLog_Write(NVLOG_EVT_USBPD_DETACH, NVLOG_SRC_USBPD,
                    NVLOG_SEV_INFO, nvData);
    }

    if (events & CYPD_EVT_PD_CONTRACT)
    {
        s_portState[devIdx] = USBPD_PORT_CONTRACT;

        Cypd_PortStatus_t status;
        if (Cypd_ReadPortStatus(devIdx, &status) == CYPD_OK)
        {
            uint32 vbus_mV = (uint32)status.vbusVoltage_100mV * 100u;
            Debug_Printf("[USBPD %s] PD CONTRACT: VBUS=%u mV, CC%u, %s\r\n",
                         name, (unsigned)vbus_mV,
                         status.cc2Polarity ? 2u : 1u,
                         status.isDfp ? "DFP" : "UFP");

            uint32 nvData[4] = { devIdx, vbus_mV,
                                 (uint32)status.cc2Polarity,
                                 (uint32)status.isDfp };
            NvLog_Write(NVLOG_EVT_USBPD_CONTRACT, NVLOG_SRC_USBPD,
                        NVLOG_SEV_INFO, nvData);
        }
    }

    /* v0.2: DP HPD events from PD controller */
    if (events & HPI_EVT_DP_HPD)
    {
        /* Read HPD state from HPI to determine HIGH/LOW/IRQ */
        /* For now, map alt-mode active → HPD HIGH, inactive → HPD LOW */
        UsbPd_HpiState_t *hpi = &s_hpiState[devIdx];
        if (hpi->altModeStatus & (HPI_ALT_DP_ACTIVE | HPI_ALT_USB4_ACTIVE))
        {
            UsbPd_Hpd_ProcessEvent(devIdx, HPD_EVENT_HIGH);
        }
        else
        {
            UsbPd_Hpd_ProcessEvent(devIdx, HPD_EVENT_LOW);
        }
    }

    if (events & CYPD_EVT_VBUS_OVP)
    {
        Debug_Printf("[USBPD %s] WARNING: VBUS Over-Voltage!\r\n", name);
        NvLog_WriteU32(NVLOG_EVT_USBPD_FAULT, NVLOG_SRC_USBPD,
                       NVLOG_SEV_ERROR,
                       ((uint32)devIdx << 8u) | 0x01u);
    }

    if (events & CYPD_EVT_VBUS_OCP)
    {
        Debug_Printf("[USBPD %s] WARNING: VBUS Over-Current!\r\n", name);
        NvLog_WriteU32(NVLOG_EVT_USBPD_FAULT, NVLOG_SRC_USBPD,
                       NVLOG_SEV_ERROR,
                       ((uint32)devIdx << 8u) | 0x02u);
    }

    /* Refresh full HPI state after any event */
    prv_RefreshHpiState(devIdx);
}

static void prv_ServiceDevice(uint8 devIdx, uint8 intr)
{
    uint8 clearMask = 0u;

    if (intr & CYPD_INTR_PORT_EVENT)
    {
        uint32 events = 0u;
        if (Cypd_ReadPortEvent(devIdx, &events) == CYPD_OK)
        {
            prv_HandleEvent(devIdx, events);
            clearMask |= CYPD_INTR_PORT_EVENT;
        }
    }

    if (clearMask != 0u)
    {
        Cypd_ClearIntr(devIdx, clearMask);
    }
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
    Cypd_Status_t st;

    for (i = 0u; i < CYPD_DEVICE_COUNT; i++)
    {
        s_portState[i]        = USBPD_PORT_DETACHED;
        s_devPresent[i]       = FALSE;
        s_devRuntimeFails[i]  = 0u;
        s_devSpuriousCount[i] = 0u;

        memset(&s_hpiState[i], 0, sizeof(UsbPd_HpiState_t));

        st = Cypd_HardReset(i);
        if (st != CYPD_OK)
        {
            Debug_Printf("[USBPD %s] INIT FAILED (err=%d) — polling disabled\r\n",
                         CYPD_DEVICES[i].name, (int)st);
        }
        else
        {
            s_devPresent[i] = TRUE;
            Debug_Printf("[USBPD %s] Init OK\r\n", CYPD_DEVICES[i].name);

            /* Initial HPI state read */
            prv_RefreshHpiState(i);
        }
    }
}

void UsbPdManager_Run(void)
{
    static uint32 s_lastPoll = 0u;
    static uint32 s_lastRefresh = 0u;
    uint8 i;

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
            uint8 intr = 0u;
            if (Cypd_ReadIntrReg(i, &intr) != CYPD_OK)
            {
                s_devRuntimeFails[i]++;
                if (s_devRuntimeFails[i] >= USBPD_RUNTIME_FAIL_LIMIT)
                {
                    s_devPresent[i] = FALSE;
                    Debug_Printf("[USBPD %s] I2C failed %u times — disabling\r\n",
                                 CYPD_DEVICES[i].name,
                                 (unsigned)s_devRuntimeFails[i]);
                    Cypd_RecoverBus(i);
                }
                continue;
            }

            s_devRuntimeFails[i] = 0u;

            if (intr == 0u)
            {
                s_devSpuriousCount[i]++;
                if (s_devSpuriousCount[i] >= USBPD_SPURIOUS_INT_LIMIT)
                {
                    s_devPresent[i] = FALSE;
                    Debug_Printf("[USBPD %s] INT_L stuck low (%u spurious) "
                                 "— disabling\r\n",
                                 CYPD_DEVICES[i].name,
                                 (unsigned)s_devSpuriousCount[i]);
                }
                continue;
            }

            s_devSpuriousCount[i] = 0u;
            prv_ServiceDevice(i, intr);
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

#endif