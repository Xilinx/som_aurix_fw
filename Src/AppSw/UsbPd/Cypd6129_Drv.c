/**
 * @file    Cypd6129_Drv.c
 * @brief   CYPD6129 HPIv2 driver implementation.
 *
 * See Cypd6129_Drv.h for the four-layer overview. On this board each
 * CYPD6129 is a single-port part, so only the device window (0x00xx) and the
 * port-0 window (0x10xx) are used; port 1 handling is kept for completeness.
 */

#include "Cypd6129_Drv.h"
#include "AppPin.h"
#include "I2c_Master.h"
#include "Stm_Timer.h"
#include "Uart_Debug.h"
#include "IfxPort.h"
#include "UsbPd_Cfg.h"

#if (USBPD_FEATURE_ENABLE == 1u)

/* How long to wait for the Success/error response that follows a write. */
#define CYPD_RESP_TIMEOUT_MS        20u
/* Spec: INTR# may re-assert up to ~50 us after the EC clears INTR_REG. */
#define CYPD_INTR_RECHECK_US        60u
/* Minimum settle time after releasing RESET_L before the first I2C poll. */
#define CYPD_RESET_SETTLE_MS        5u
/* Interval between boot-readiness polls (see prv_PulseResetAndReadMode). */
#define CYPD_BOOT_POLL_MS           2u

static boolean s_devInitOk[CYPD_DEVICE_COUNT];

/* ======================================================================== */
/*  Private I2C helpers                                                     */
/* ======================================================================== */

static uint8 prv_Addr(uint8 devIdx)
{
    return CYPD_DEVICES[devIdx].i2cAddr;
}

static uint8 prv_Bus(uint8 devIdx)
{
    return CYPD_DEVICES[devIdx].i2cBus;
}

/* Which RESPONSE register and INTR_REG bit a write to `reg` reports through. */
static void prv_RespChannel(uint16 reg, uint16 *pRespReg, uint8 *pIntrBit)
{
    if (reg < HPI_PORT0_BASE)
    {
        *pRespReg = HPI_REG_RESPONSE;      /* 0x007E device response        */
        *pIntrBit = HPI_INTR_DEV;
    }
    else if (reg < HPI_PORT1_BASE)
    {
        *pRespReg = HPI_REG_PD_RESPONSE;   /* 0x1400 port-0 response        */
        *pIntrBit = HPI_INTR_PORT0;
    }
    else
    {
        *pRespReg = (uint16)(HPI_REG_PD_RESPONSE + 0x1000u); /* 0x2400      */
        *pIntrBit = HPI_INTR_PORT1;
    }
}

/* ======================================================================== */
/*  Layer 1 : raw register access                                           */
/* ======================================================================== */

Cypd_Status_t Cypd_HpiRead(uint8 devIdx, uint16 reg16, uint8 *pBuf, uint16 len)
{
    if ((devIdx >= CYPD_DEVICE_COUNT) || (pBuf == NULL_PTR) || (len == 0u))
    {
        return CYPD_ERR_INVALID;
    }
    return (I2cMaster_ReadReg16_Bus(prv_Bus(devIdx), prv_Addr(devIdx),
                                    reg16, pBuf, len) == I2C_OK)
           ? CYPD_OK : CYPD_ERR_I2C;
}

/* Read one INTR_REG byte. */
static Cypd_Status_t prv_ReadIntr(uint8 devIdx, uint8 *pIntr)
{
    return Cypd_HpiRead(devIdx, HPI_REG_INTR_REG, pIntr, 1u);
}

/* Acknowledge (clear) the given INTR_REG source bits (write-1-to-clear). */
static Cypd_Status_t prv_ClearIntr(uint8 devIdx, uint8 mask)
{
    return (I2cMaster_WriteReg16_Bus(prv_Bus(devIdx), prv_Addr(devIdx),
                                     HPI_REG_INTR_REG, &mask, 1u) == I2C_OK)
           ? CYPD_OK : CYPD_ERR_I2C;
}

/* ======================================================================== */
/*  Layer 2 : write-with-response                                           */
/* ======================================================================== */

Cypd_Status_t Cypd_HpiWrite(uint8 devIdx, uint16 reg16, const uint8 *pBuf,
                            uint16 len)
{
    uint16 respReg;
    uint8  intrBit;
    uint8  intr = 0u;
    uint8  hdr[2];
    uint32 tStart;

    if ((devIdx >= CYPD_DEVICE_COUNT) || (pBuf == NULL_PTR) || (len == 0u))
    {
        return CYPD_ERR_INVALID;
    }

    /* 1. Issue the raw write. */
    if (I2cMaster_WriteReg16_Bus(prv_Bus(devIdx), prv_Addr(devIdx),
                                 reg16, pBuf, len) != I2C_OK)
    {
        return CYPD_ERR_I2C;
    }

    /* 2. Every valid register write produces a response + INTR#. Wait for
     *    the matching INTR_REG bit so we can read and consume that response. */
    prv_RespChannel(reg16, &respReg, &intrBit);
    tStart = Stm_GetTimeMs();
    do
    {
        if (prv_ReadIntr(devIdx, &intr) != CYPD_OK)
        {
            return CYPD_ERR_I2C;
        }
        if ((intr & intrBit) != 0u)
        {
            break;
        }
        if ((Stm_GetTimeMs() - tStart) > CYPD_RESP_TIMEOUT_MS)
        {
            return CYPD_ERR_TIMEOUT;
        }
    } while (TRUE);

    /* 3. Read the response code (byte0 = code, byte1 = length). */
    if (Cypd_HpiRead(devIdx, respReg, hdr, 2u) != CYPD_OK)
    {
        return CYPD_ERR_I2C;
    }

    /* 4. Acknowledge the interrupt so the chip frees the response register. */
    (void)prv_ClearIntr(devIdx, intrBit);

    /* 5. A command response of Success means the write was accepted. If an
     *    async event preempted our response slot (b7 set), we accept the
     *    write and move on. This only matters during the brief bring-up
     *    window, when no cable is normally attached; steady-state events all
     *    flow through Cypd_ServiceInterrupts instead. */
    if ((hdr[0] & HPI_RESP_IS_EVENT) != 0u)
    {
        return CYPD_OK;
    }
    return (hdr[0] == HPI_RESP_SUCCESS) ? CYPD_OK : CYPD_ERR_RESPONSE;
}

/* ======================================================================== */
/*  Layer 3 : interrupt service                                             */
/* ======================================================================== */

/* Read one message (code + optional payload) from a RESPONSE register. */
static Cypd_Status_t prv_ReadMessage(uint8 devIdx, uint16 respReg,
                                     uint16 dataReg, uint8 port,
                                     Cypd_Event_t *pEv)
{
    uint8 hdr[2];

    if (Cypd_HpiRead(devIdx, respReg, hdr, 2u) != CYPD_OK)
    {
        return CYPD_ERR_I2C;
    }

    /* Keep the FULL byte as the code. The spec's event codes already include
     * the b7 "event" flag (0x80, 0x84, 0x86, ...), and the command-response
     * codes have b7 = 0 (0x00, 0x02, 0x05, ...), so the two sets never
     * overlap. Masking b7 off here would be wrong: 0x84 would become 0x04 and
     * never match the 0x84 the manager compares against, and 0x80 -> 0x00 /
     * 0x82 -> 0x02 would collide with the NO_RESPONSE / SUCCESS codes. */
    pEv->code    = hdr[0];
    pEv->isEvent = (boolean)((hdr[0] & HPI_RESP_IS_EVENT) != 0u);
    pEv->port    = port;
    pEv->dataLen = (hdr[1] <= CYPD_EVENT_DATA_MAX) ? hdr[1] : CYPD_EVENT_DATA_MAX;

    if ((pEv->dataLen > 0u) && (dataReg != 0u))
    {
        if (Cypd_HpiRead(devIdx, dataReg, pEv->data, pEv->dataLen) != CYPD_OK)
        {
            return CYPD_ERR_I2C;
        }
    }
    return CYPD_OK;
}

Cypd_Status_t Cypd_ServiceInterrupts(uint8 devIdx, Cypd_Event_t *pEvents,
                                     uint8 maxEvents, uint8 *pCount)
{
    uint8 count = 0u;
    uint8 guard;

    if ((devIdx >= CYPD_DEVICE_COUNT) || (pEvents == NULL_PTR) ||
        (pCount == NULL_PTR) || (maxEvents == 0u))
    {
        return CYPD_ERR_INVALID;
    }
    *pCount = 0u;

    /* Drain the queue: the chip keeps INTR# asserted and re-sets the INTR_REG
     * bits while more messages are waiting, so we loop until it reads zero or
     * the caller's buffer is full. `guard` stops a runaway loop on a fault. */
    for (guard = 0u; (guard < (uint8)(maxEvents + 4u)) && (count < maxEvents);
         guard++)
    {
        uint8 intr = 0u;
        uint8 clearMask = 0u;

        if (prv_ReadIntr(devIdx, &intr) != CYPD_OK)
        {
            return CYPD_ERR_I2C;
        }
        if (intr == 0u)
        {
            break;      /* queue empty, nothing more to read */
        }

        /* Device-level message (RESPONSE register 0x007E). */
        if (((intr & HPI_INTR_DEV) != 0u) && (count < maxEvents))
        {
            if (prv_ReadMessage(devIdx, HPI_REG_RESPONSE, 0u,
                                CYPD_PORT_DEVICE, &pEvents[count]) == CYPD_OK)
            {
                if (pEvents[count].code != HPI_RESP_NO_RESPONSE)
                {
                    count++;
                }
            }
            clearMask |= HPI_INTR_DEV;
        }

        /* Port-0 message (PD_RESPONSE 0x1400, payload at 0x1404). */
        if (((intr & HPI_INTR_PORT0) != 0u) && (count < maxEvents))
        {
            if (prv_ReadMessage(devIdx, HPI_REG_PD_RESPONSE,
                                HPI_REG_PD_RESPONSE_DATA, 0u,
                                &pEvents[count]) == CYPD_OK)
            {
                if (pEvents[count].code != HPI_RESP_NO_RESPONSE)
                {
                    count++;
                }
            }
            clearMask |= HPI_INTR_PORT0;
        }

        /* Port-1 message (not used on the single-port CYPD6129). */
        if (((intr & HPI_INTR_PORT1) != 0u) && (count < maxEvents))
        {
            if (prv_ReadMessage(devIdx,
                                (uint16)(HPI_REG_PD_RESPONSE + 0x1000u),
                                (uint16)(HPI_REG_PD_RESPONSE_DATA + 0x1000u),
                                1u, &pEvents[count]) == CYPD_OK)
            {
                if (pEvents[count].code != HPI_RESP_NO_RESPONSE)
                {
                    count++;
                }
            }
            clearMask |= HPI_INTR_PORT1;
        }

        if (clearMask == 0u)
        {
            break;      /* unknown bit set; avoid spinning */
        }

        /* Acknowledge the serviced sources, then give the chip the ~50 us it
         * needs before it may re-assert INTR# for the next queued message. */
        (void)prv_ClearIntr(devIdx, clearMask);
        Stm_DelayUs(CYPD_INTR_RECHECK_US);
    }

    *pCount = count;
    return CYPD_OK;
}

/* ======================================================================== */
/*  Decode helpers                                                          */
/* ======================================================================== */

Cypd_FwImage_t Cypd_DecodeImage(uint16 mode)
{
    return (Cypd_FwImage_t)(mode & HPI_DEVICE_MODE_IMAGE_MASK);
}

const char *Cypd_ImageName(Cypd_FwImage_t img)
{
    switch (img)
    {
        case CYPD_IMG_BOOTLOADER: return "bootloader";
        case CYPD_IMG_FW1:        return "FW1";
        case CYPD_IMG_FW2:        return "FW2";
        default:                  return "invalid";
    }
}

const char *Cypd_EventName(uint8 code)
{
    switch (code)
    {
        case HPI_EVT_RESET_COMPLETE:   return "RESET_COMPLETE";
        case HPI_EVT_QUEUE_OVERFLOW:   return "QUEUE_OVERFLOW";
        case HPI_EVT_OVER_CURRENT:     return "OVER_CURRENT";
        case HPI_EVT_OVER_VOLTAGE:     return "OVER_VOLTAGE";
        case HPI_EVT_TYPEC_CONNECT:    return "TYPEC_CONNECT";
        case HPI_EVT_TYPEC_DISCONNECT: return "TYPEC_DISCONNECT";
        case HPI_EVT_PD_CONTRACT:      return "PD_CONTRACT";
        case HPI_EVT_DP_MODE_ENTERED:  return "DP_MODE_ENTERED";
        case HPI_EVT_DP_STATUS_UPDATE: return "DP_STATUS_UPDATE";
        case HPI_EVT_AMD_APU_REQUEST:  return "AMD_APU_REQUEST(0xE0)";
        case HPI_EVT_AMD_HPD_LOW_HIGH: return "AMD_HPD_LOW_HIGH(0xE1)";
        case HPI_EVT_AMD_HPD_HIGH_LOW: return "AMD_HPD_HIGH_LOW(0xE2)";
        case HPI_EVT_AMD_HPD_IRQ:      return "AMD_HPD_IRQ(0xE3)";
        case HPI_EVT_AMD_APU_TIMEOUT:  return "AMD_APU_TIMEOUT(0xC0)";
        case HPI_RESP_SUCCESS:         return "SUCCESS";
        case HPI_RESP_NO_RESPONSE:     return "NO_RESPONSE";
        default:                       return "UNKNOWN";
    }
}

/* ======================================================================== */
/*  Layer 4 : bring-up                                                      */
/* ======================================================================== */

Cypd_Status_t Cypd_ReadDeviceMode(uint8 devIdx, uint16 *pMode)
{
    uint8 buf[2];
    Cypd_Status_t st;

    if ((devIdx >= CYPD_DEVICE_COUNT) || (pMode == NULL_PTR))
    {
        return CYPD_ERR_INVALID;
    }
    st = Cypd_HpiRead(devIdx, HPI_REG_DEVICE_MODE, buf, 2u);
    if (st != CYPD_OK)
    {
        return st;
    }
    *pMode = (uint16)(((uint16)buf[1] << 8u) | (uint16)buf[0]);
    return CYPD_OK;
}

/*
 * Toggle RESET_L and wait for the controller to finish booting, then return
 * the DEVICE_MODE it reports.
 *
 * We POLL for readiness instead of using one fixed delay. The reason is the
 * EC-Init-Complete deadline: the chip gives the EC at most 100 ms AFTER its
 * "Reset Complete" notification to send EC-Init-Complete, and it raises Reset
 * Complete only once it has booted. A fixed blind wait (reset-release + 100 ms)
 * pushes our EVENT_MASK / EC-Init-Complete writes to a point whose margin
 * inside that window depends on the unknown boot time. By polling DEVICE_MODE
 * and returning the instant the chip answers, the caller sends EC-Init-Complete
 * promptly after Reset Complete, with comfortable margin. CYPD_BOOT_DELAY_MS is
 * reused only as the upper bound on how long we wait before giving up.
 */
static Cypd_Status_t prv_PulseResetAndReadMode(uint8 devIdx, uint16 *pMode)
{
    const Cypd_DevCfg_t *dev = &CYPD_DEVICES[devIdx];
    uint32 tStart;
    Cypd_Status_t st;

    IfxPort_setPinLow(AppPin_GetPort(dev->resetPin.portIdx), dev->resetPin.pinIdx);
    Stm_DelayMs(CYPD_RESET_PULSE_MS);
    IfxPort_setPinHigh(AppPin_GetPort(dev->resetPin.portIdx), dev->resetPin.pinIdx);

    Stm_DelayMs(CYPD_RESET_SETTLE_MS);
    tStart = Stm_GetTimeMs();
    do
    {
        st = Cypd_ReadDeviceMode(devIdx, pMode);
        if (st == CYPD_OK)
        {
            return CYPD_OK;         /* chip is up: proceed immediately */
        }
        Stm_DelayMs(CYPD_BOOT_POLL_MS);
    } while ((Stm_GetTimeMs() - tStart) < CYPD_BOOT_DELAY_MS);

    return st;                      /* timed out: return the last I2C error */
}

Cypd_Status_t Cypd_HardReset(uint8 devIdx)
{
    const Cypd_DevCfg_t *dev;
    uint16 mode = 0u;
    Cypd_FwImage_t img;
    Cypd_Status_t st;

    if (devIdx >= CYPD_DEVICE_COUNT)
    {
        return CYPD_ERR_INVALID;
    }
    dev = &CYPD_DEVICES[devIdx];
    s_devInitOk[devIdx] = FALSE;

    st = prv_PulseResetAndReadMode(devIdx, &mode);
    if (st != CYPD_OK)
    {
        Debug_Printf("[CYPD %s] Reset: I2C error reading DEVICE_MODE\r\n",
                     dev->name);
        I2cMaster_ReinitBus(dev->i2cBus);
        return CYPD_ERR_I2C;
    }

    /*
     * DEVICE_MODE[1:0] selects the running image: 0 = bootloader, 1 = FW1,
     * 2 = FW2, 3 = invalid. A controller running either firmware image is
     * operational; only the bootloader (0) warrants a reset/retry. Testing
     * bit 0 alone wrongly rejects FW2 (e.g. MODE=0x0092 -> bits[1:0]=10=FW2).
     */
    img = Cypd_DecodeImage(mode);
    if ((img == CYPD_IMG_FW1) || (img == CYPD_IMG_FW2))
    {
        Debug_Printf("[CYPD %s] Reset OK, running %s (MODE=0x%04X)\r\n",
                     dev->name, Cypd_ImageName(img), mode);
        s_devInitOk[devIdx] = TRUE;
        return CYPD_OK;
    }
    if (img == CYPD_IMG_INVALID)
    {
        Debug_Printf("[CYPD %s] Reset: invalid image (MODE=0x%04X)\r\n",
                     dev->name, mode);
        return CYPD_ERR_BAD_IMAGE;
    }

    /* Still in bootloader: one more reset attempt. */
    Debug_Printf("[CYPD %s] Reset: device in bootloader - retrying\r\n",
                 dev->name);
    st = prv_PulseResetAndReadMode(devIdx, &mode);
    if (st != CYPD_OK)
    {
        Debug_Printf("[CYPD %s] Reset: I2C error reading DEVICE_MODE\r\n",
                     dev->name);
        I2cMaster_ReinitBus(dev->i2cBus);
        return CYPD_ERR_I2C;
    }

    img = Cypd_DecodeImage(mode);
    if ((img != CYPD_IMG_FW1) && (img != CYPD_IMG_FW2))
    {
        Debug_Printf("[CYPD %s] Reset: still in boot mode after retry "
                     "(MODE=0x%04X)\r\n", dev->name, mode);
        return CYPD_ERR_BOOT_MODE;
    }

    Debug_Printf("[CYPD %s] Reset OK, running %s (MODE=0x%04X)\r\n",
                 dev->name, Cypd_ImageName(img), mode);
    s_devInitOk[devIdx] = TRUE;
    return CYPD_OK;
}

Cypd_Status_t Cypd_HpiInit(uint8 devIdx)
{
    const char *name;
    uint32 eventMask = HPI_EVENT_MASK_VALUE;
    uint8  maskBuf[4];
    uint8  cmd;
    uint8  evtCount = 0u;
    Cypd_Event_t evts[4];
    Cypd_Status_t st;
    uint8 i;

    if (devIdx >= CYPD_DEVICE_COUNT)
    {
        return CYPD_ERR_INVALID;
    }
    name = CYPD_DEVICES[devIdx].name;

    /* 1. The chip raised a Reset-Complete (0x80) event when it booted. Drain
     *    the queue so INTR# starts clean and log what we saw. */
    st = Cypd_ServiceInterrupts(devIdx, evts, 4u, &evtCount);
    if (st != CYPD_OK)
    {
        Debug_Printf("[CYPD %s] Init: cannot drain reset events (err=%d)\r\n",
                     name, (int)st);
        return st;
    }
    for (i = 0u; i < evtCount; i++)
    {
        Debug_Printf("[CYPD %s] Init: boot message %s (0x%02X)\r\n",
                     name, Cypd_EventName(evts[i].code), evts[i].code);
    }

    /* 2. Program EVENT_MASK. This MUST happen before EC-Init-Complete, and
     *    the chip only waits ~100 ms for that command after boot. */
    maskBuf[0] = (uint8)(eventMask & 0xFFu);
    maskBuf[1] = (uint8)((eventMask >> 8u) & 0xFFu);
    maskBuf[2] = (uint8)((eventMask >> 16u) & 0xFFu);
    maskBuf[3] = (uint8)((eventMask >> 24u) & 0xFFu);
    st = Cypd_HpiWrite(devIdx, HPI_REG_EVENT_MASK, maskBuf, 4u);
    if (st != CYPD_OK)
    {
        Debug_Printf("[CYPD %s] Init: EVENT_MASK write failed (err=%d)\r\n",
                     name, (int)st);
        return st;
    }

    /* 3. Tell the chip the EC has finished its own init. */
    cmd = HPI_PDCMD_EC_INIT_COMPLETE;
    st = Cypd_HpiWrite(devIdx, HPI_REG_PD_CONTROL, &cmd, 1u);
    if (st != CYPD_OK)
    {
        Debug_Printf("[CYPD %s] Init: EC-Init-Complete failed (err=%d)\r\n",
                     name, (int)st);
        return st;
    }

    Debug_Printf("[CYPD %s] HPI init complete (EVENT_MASK=0x%08X)\r\n",
                 name, (unsigned)eventMask);
    return CYPD_OK;
}

/* ======================================================================== */
/*  Port status                                                             */
/* ======================================================================== */

Cypd_Status_t Cypd_ReadPortStatus(uint8 devIdx, Cypd_PortStatus_t *pStatus)
{
    uint8  tc = 0u;
    uint8  pd[4] = {0u};
    uint8  vbus[2] = {0u};
    Cypd_Status_t st;

    if ((devIdx >= CYPD_DEVICE_COUNT) || (pStatus == NULL_PTR))
    {
        return CYPD_ERR_INVALID;
    }

    st = Cypd_HpiRead(devIdx, HPI_REG_TYPE_C_STATUS, &tc, 1u);
    if (st != CYPD_OK) { return st; }

    st = Cypd_HpiRead(devIdx, HPI_REG_PD_STATUS, pd, 4u);
    if (st != CYPD_OK) { return st; }

    st = Cypd_HpiRead(devIdx, HPI_REG_BUS_VOLTAGE, vbus, 2u);
    if (st != CYPD_OK) { return st; }

    {
        uint32 pdWord = ((uint32)pd[3] << 24u) | ((uint32)pd[2] << 16u) |
                        ((uint32)pd[1] << 8u)  | (uint32)pd[0];

        pStatus->attached    = (boolean)((tc & HPI_TC_ATTACHED) != 0u);
        pStatus->cc2Polarity = (boolean)((tc & HPI_TC_CC_POLARITY_CC2) != 0u);
        pStatus->isDfp       = (boolean)((pdWord & HPI_PD_DATA_ROLE_DFP) != 0u);
        pStatus->isSource    = (boolean)((pdWord & HPI_PD_POWER_ROLE_SOURCE) != 0u);
        pStatus->vbusVoltage_100mV = (uint16)(((uint16)vbus[1] << 8u) | vbus[0]);
    }
    return CYPD_OK;
}

boolean Cypd_IsIntAsserted(uint8 devIdx)
{
    if ((devIdx >= CYPD_DEVICE_COUNT) || !s_devInitOk[devIdx])
    {
        return FALSE;
    }
    return (IfxPort_getPinState(
        AppPin_GetPort(CYPD_DEVICES[devIdx].intPin.portIdx),
        CYPD_DEVICES[devIdx].intPin.pinIdx) == 0u) ? TRUE : FALSE;
}

void Cypd_RecoverBus(uint8 devIdx)
{
    if (devIdx >= CYPD_DEVICE_COUNT)
    {
        return;
    }
    I2cMaster_ReinitBus(CYPD_DEVICES[devIdx].i2cBus);
}

#endif /* USBPD_FEATURE_ENABLE */
