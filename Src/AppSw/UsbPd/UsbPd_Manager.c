/**
 * @file    UsbPd_Manager.c
 * @brief   Dual-port CYPD6129 USB PD management loop.
 */

#include "UsbPd_Manager.h"
#include "Cypd6129_Drv.h"
#include "UsbPd_Cfg.h"
#include "Stm_Timer.h"
#include "Uart_Debug.h"

static UsbPd_PortState_t s_portState[CYPD_DEVICE_COUNT];

/* ---- Private helpers ----------------------------------------------------- */

static void prv_HandleEvent(uint8 devIdx, uint32 events)
{
    const char *name = CYPD_DEVICES[devIdx].name;

    if (events & CYPD_EVT_TYPEC_ATTACH)
    {
        s_portState[devIdx] = USBPD_PORT_ATTACHED;
        Debug_Printf("[USBPD %s] Type-C ATTACH\r\n", name);
    }

    if (events & CYPD_EVT_TYPEC_DETACH)
    {
        s_portState[devIdx] = USBPD_PORT_DETACHED;
        Debug_Printf("[USBPD %s] Type-C DETACH\r\n", name);
    }

    if (events & CYPD_EVT_PD_CONTRACT)
    {
        s_portState[devIdx] = USBPD_PORT_CONTRACT;

        /* Read port status to log negotiated voltage */
        Cypd_PortStatus_t status;
        if (Cypd_ReadPortStatus(devIdx, &status) == CYPD_OK)
        {
            uint32 vbus_mV = (uint32)status.vbusVoltage_100mV * 100u;
            Debug_Printf("[USBPD %s] PD CONTRACT: VBUS=%u mV, CC%u, %s\r\n",
                         name, vbus_mV,
                         status.cc2Polarity ? 2u : 1u,
                         status.isDfp ? "DFP" : "UFP");
        }
    }

    if (events & CYPD_EVT_VBUS_OVP)
    {
        Debug_Printf("[USBPD %s] WARNING: VBUS Over-Voltage!\r\n", name);
    }

    if (events & CYPD_EVT_VBUS_OCP)
    {
        Debug_Printf("[USBPD %s] WARNING: VBUS Over-Current!\r\n", name);
    }
}

static void prv_ServiceDevice(uint8 devIdx)
{
    /* Read interrupt register to determine source */
    uint8 intr = 0u;
    if (Cypd_ReadIntrReg(devIdx, &intr) != CYPD_OK)
    {
        Debug_Printf("[USBPD %s] ERR: failed to read INTR_REG\r\n",
                     CYPD_DEVICES[devIdx].name);
        return;
    }

    if (intr & CYPD_INTR_PORT_EVENT)
    {
        uint32 events = 0u;
        if (Cypd_ReadPortEvent(devIdx, &events) == CYPD_OK)
        {
            prv_HandleEvent(devIdx, events);
        }
    }

    /* Clear serviced interrupt bits */
    Cypd_ClearIntr(devIdx, intr);
}

/* ---- Public API ---------------------------------------------------------- */

void UsbPdManager_Init(void)
{
    /* Populate config table before any driver access (Tasking E306 workaround). */
    UsbPd_CfgInit();

    for (uint8 i = 0u; i < CYPD_DEVICE_COUNT; i++)
    {
        s_portState[i] = USBPD_PORT_DETACHED;

        Cypd_Status_t st = Cypd_HardReset(i);
        if (st != CYPD_OK)
        {
            Debug_Printf("[USBPD %s] INIT FAILED (err=%d)\r\n",
                         CYPD_DEVICES[i].name, (int)st);
        }
        else
        {
            Debug_Printf("[USBPD %s] Init OK\r\n", CYPD_DEVICES[i].name);
        }
    }
}

void UsbPdManager_Run(void)
{
    static uint32 s_lastPoll = 0u;

    if (!Stm_IsElapsedMs(&s_lastPoll, USBPD_MGR_POLL_INTERVAL_MS))
    {
        return;
    }

    for (uint8 i = 0u; i < CYPD_DEVICE_COUNT; i++)
    {
        if (Cypd_IsIntAsserted(i))
        {
            prv_ServiceDevice(i);
        }
    }
}

UsbPd_PortState_t UsbPdManager_GetPortState(uint8 portIdx)
{
    if (portIdx >= CYPD_DEVICE_COUNT)
    {
        return USBPD_PORT_DETACHED;
    }
    return s_portState[portIdx];
}
