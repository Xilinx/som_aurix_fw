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
static boolean s_devPresent[CYPD_DEVICE_COUNT];
static uint8 s_devRuntimeFails[CYPD_DEVICE_COUNT];
static uint16 s_devSpuriousCount[CYPD_DEVICE_COUNT];

/* ---- Private helpers ----------------------------------------------------- */
#if (USBPD_FEATURE_ENABLE == 1u)

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


/*

Poll cycle:
1. Cypd_IsIntAsserted(i)          
2. UsbPdManager_Run reads intr   
3. prv_ServiceDevice(i, intr)    
4. Process events
5. Cypd_ClearIntr(i, intr)    
*/

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

/* ---- Public API ---------------------------------------------------------- */

void UsbPdManager_Init(void)
{
    UsbPd_CfgInit();
    uint8 i;
    Cypd_Status_t st;

    for (i = 0u; i < CYPD_DEVICE_COUNT; i++)
    {
        s_portState[i] = USBPD_PORT_DETACHED;
        s_devPresent[i] = FALSE;
        s_devRuntimeFails[i]  = 0u;
        s_devSpuriousCount[i] = 0u;
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
        }
    }
}

void UsbPdManager_Run(void)
{
    static uint32 s_lastPoll = 0u;
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
                /* Mode A: I2C failure — device gone or bus stuck */
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
                /* Mode B: INT_L asserted but no interrupt bits set.
                 * GPIO is stuck low — count spurious reads. */
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

            /* Real interrupt — reset spurious counter, service it */
            s_devSpuriousCount[i] = 0u;
            prv_ServiceDevice(i, intr);
        }
        else
        {
            /* INT_L is high (not asserted) — clear spurious counter */
            s_devSpuriousCount[i] = 0u;
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
#else

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
