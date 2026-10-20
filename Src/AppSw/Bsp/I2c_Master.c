/**
 * @file    I2c_Master.c
 * @brief   Polling I2C master — iLLD 1.20.0 protocol with bounded waits.
 *
 * iLLD IfxI2c_I2c_write2 / read2 contain unbounded
 *     while ((i2c->RIS.U) == 0) {}
 * loops.  A clock-stretching or dying SB-TSI freezes CPU1 forever
 * (Sapphire PM-state-6 hang).  We run the same 7-bit FIFO state
 * machine as write2/read2, but every RIS / TX_END wait has an STM
 * deadline; on expiry we stop the kernel and ReinitBus.
 *
 * enableRepeatedStart is applied to ADDRCFG.SOPE (iLLD stores the
 * flag on the device handle but write2 never writes SOPE).  APML
 * SB-TSI requires STOP between Send Byte and Receive Byte.
 */

#include "I2c_Master.h"
#include "I2c_Slave.h"
#include "Stm_Timer.h"
#include "IfxI2c_I2c.h"
#include "IfxI2c.h"
#include "IfxI2c_PinMap.h"
#include "Uart_Debug.h"

static IfxI2c_I2c s_i2cHandle;
static IfxI2c_I2c s_i2c1Handle;   /* I2C1 — APML (P11.14/P11.13) */
static IfxI2c_I2c_Device s_deviceHandle;
static IfxI2c_I2c_Device s_apmlDevHandle; /* I2C1 — APML (P11.14/P11.13) */

static uint32 s_lastTimeoutLogMs = 0u;

static boolean prv_BusIsBusy(uint8 busIdx)
{
    Ifx_I2C *mod;
    IfxI2c_BusStatus bs;
    if      (busIdx == 0u) mod = &MODULE_I2C0;
    else if (busIdx == 1u) mod = &MODULE_I2C1;
    else return TRUE;                                /* invalid index fails closed */
    bs = IfxI2c_getBusStatus(mod);
    /* Same acceptance set as IfxI2c_I2c_write/read (IfxI2c_I2c.c:221,565):
     * idle = bus free; busyMaster = WE hold the bus between our own
     * transfers — both are safe entry states.  'started' (another master's
     * start seen) and 'remoteSlave' are the genuinely-busy cases. */
    return (boolean)((bs != IfxI2c_BusStatus_idle) &&
                     (bs != IfxI2c_BusStatus_busyMaster));
}

static Ifx_I2C *prv_Mod(uint8 busIdx)
{
    return (busIdx == 0u) ? &MODULE_I2C0 : &MODULE_I2C1;
}

static uint8 prv_BusIdxFromDev(const IfxI2c_I2c_Device *dev)
{
    if ((dev == NULL_PTR) || (dev->i2c == NULL_PTR))
    {
        return 1u;
    }
    return (dev->i2c->i2c == &MODULE_I2C0) ? 0u : 1u;
}

static void prv_LogTimeout(uint8 busIdx)
{
    if (Stm_IsElapsedMs(&s_lastTimeoutLogMs, 1000u))
    {
        Debug_Printf("[I2C] bus%u transfer timeout — kernel reinit\r\n",
                     (unsigned)busIdx);
    }
}

void I2cMaster_ReinitBus(uint8 busIdx)
{
    IfxI2c_I2c_Config cfg;

    if (busIdx == 0u)
    {
        const IfxI2c_Pins pins = {
            &IfxI2c0_SCL_P13_1_INOUT,
            &IfxI2c0_SDA_P13_2_INOUT,
            IfxPort_PadDriver_cmosAutomotiveSpeed1
        };
        IfxI2c_I2c_initConfig(&cfg, &MODULE_I2C0);
        cfg.baudrate = (float32)I2C_MASTER_FREQ_HZ;
        cfg.mode     = IfxI2c_Mode_StandardAndFast;
        cfg.pins     = &pins;
        IfxI2c_I2c_initModule(&s_i2cHandle, &cfg);
    }
    else if (busIdx == 1u)
    {
        const IfxI2c_Pins pins = {
            &IfxI2c1_SCL_P11_14_INOUT,
            &IfxI2c1_SDA_P11_13_INOUT,
            IfxPort_PadDriver_cmosAutomotiveSpeed1
        };
        IfxI2c_I2c_initConfig(&cfg, &MODULE_I2C1);
        cfg.baudrate = 400000.0f;
        cfg.mode     = IfxI2c_Mode_StandardAndFast;
        cfg.pins     = &pins;
        IfxI2c_I2c_initModule(&s_i2c1Handle, &cfg);
    }
}

void I2cMaster_AbortBus(uint8 busIdx)
{
    Ifx_I2C *mod;

    if ((busIdx != 0u) && (busIdx != 1u))
    {
        return;
    }
    mod = prv_Mod(busIdx);
    /* RUN=0 drops the protocol engine.  Do NOT call IfxI2c_releaseBus()
     * here — it waits forever for TX_END, which is the hang we are in. */
    IfxI2c_stop(mod);
    IfxI2c_clearAllProtocolInterruptSources(mod);
    IfxI2c_clearAllErrorInterruptSources(mod);
    IfxI2c_clearAllDtrInterruptSources(mod);
    IfxI2c_resetFifo(mod);
    I2cMaster_ReinitBus(busIdx);
}

void I2cMaster_OnLeaveS0(void)
{
    /* I2C1 is master-only. Do not park the I2C0 sideband slave from CPU1. */
    I2cMaster_AbortBus(1u);
}

void I2cMaster_OnEnterS0(void)
{
    I2cMaster_ReinitBus(1u);
}

static boolean prv_WaitBusIdle(uint8 busIdx, uint32 timeoutMs)
{
    uint32 start = Stm_GetTimeMs();
    while (prv_BusIsBusy(busIdx))
    {
        if ((Stm_GetTimeMs() - start) >= timeoutMs)
        {
            /* Bus never went idle — likely a latched phantom START from a
             * pin-mux/rail transition (seen at init on bus 1).  One kernel
             * reset, one more bounded wait, then report honestly. */
            I2cMaster_ReinitBus(busIdx);
            start = Stm_GetTimeMs();
            while (prv_BusIsBusy(busIdx))
            {
                if ((Stm_GetTimeMs() - start) >= timeoutMs)
                    return FALSE;               /* genuinely stuck wire */
            }
            return TRUE;                        /* recovered */
        }
    }
    return TRUE;
}

uint32 I2cMaster_GetRawBusStatus(uint8 busIdx)
{
    Ifx_I2C *mod = (busIdx == 0u) ? &MODULE_I2C0 : &MODULE_I2C1;
    return (uint32)IfxI2c_getBusStatus(mod);
}

/* Initialise device handle for a given 7-bit address. */
static void prv_SetDevice(uint8 addr7bit, boolean repeatedStart)
{
    IfxI2c_I2c_deviceConfig devCfg;
    IfxI2c_I2c_initDeviceConfig(&devCfg, &s_i2cHandle);
    devCfg.deviceAddress       = (uint16)((uint16)addr7bit << 1u); /* 8-bit shifted */
    devCfg.enableRepeatedStart = repeatedStart;
    IfxI2c_I2c_initDevice(&s_deviceHandle, &devCfg);
}

static boolean prv_WaitRis(Ifx_I2C *i2c)
{
    uint32 start = Stm_GetTimeMs();
    while (i2c->RIS.U == 0u)
    {
        if ((Stm_GetTimeMs() - start) >= I2C_XFER_TIMEOUT_MS)
        {
            return FALSE;
        }
    }
    return TRUE;
}

static boolean prv_WaitTxEnd(Ifx_I2C *i2c)
{
    uint32 start = Stm_GetTimeMs();
    while (IfxI2c_getProtocolInterruptSourceStatus(
               i2c, IfxI2c_ProtocolInterruptSource_transmissionEnd) == FALSE)
    {
        if ((Stm_GetTimeMs() - start) >= I2C_XFER_TIMEOUT_MS)
        {
            return FALSE;
        }
    }
    return TRUE;
}

static void prv_PrepareMasterXfer(IfxI2c_I2c_Device *dev)
{
    Ifx_I2C *i2c = dev->i2c->i2c;

    /* ADDRCFG / FIFOCFG are config-mode only (RUN=0).
     * Force 1-word byte FIFO so a 1-byte APML access cannot burst-dump
     * 8 FIFO words (I2cSlave_Start sets TXBS=RXBS=3). */
    IfxI2c_stop(i2c);
    i2c->ADDRCFG.B.MNS  = 1u;
    i2c->ADDRCFG.B.SOPE = (dev->enableRepeatedStart != FALSE) ? 0u : 1u;
    i2c->ADDRCFG.B.SONA = 1u;   /* release bus on NACK — do not sit in RESTART */
    i2c->FIFOCFG.B.TXFC = 1u;
    i2c->FIFOCFG.B.RXFC = 1u;
    i2c->FIFOCFG.B.TXBS = 0u;
    i2c->FIFOCFG.B.RXBS = 0u;
    i2c->FIFOCFG.B.TXFA = 0u;
    i2c->FIFOCFG.B.RXFA = 0u;
    IfxI2c_clearAllProtocolInterruptSources(i2c);
    IfxI2c_clearAllErrorInterruptSources(i2c);
    IfxI2c_clearAllDtrInterruptSources(i2c);
    IfxI2c_enableInterruptSources(i2c);
    IfxI2c_run(i2c);
}

static void prv_LockBusForMaster(uint8 busIdx, IfxI2c_I2c_Device *dev)
{
    /* CYPD master and the 0x54 slave share MODULE_I2C0. */
    if (busIdx == 0u)
    {
        I2cSlave_Suspend();
    }
    prv_PrepareMasterXfer(dev);
}

static void prv_UnlockBus(uint8 busIdx)
{
    if (busIdx == 0u)
    {
        I2cSlave_Resume();
    }
}

static I2c_Status_t prv_Timeout(uint8 busIdx)
{
    prv_LogTimeout(busIdx);
    I2cMaster_AbortBus(busIdx);
    return I2C_ERR_TIMEOUT;
}

/*
 * 7-bit master write — same packet flow as IfxI2c_I2c_write2, bounded waits.
 * Caller has already locked the bus and prepared SOPE/FIFO.
 */
static I2c_Status_t prv_Write2Bounded(IfxI2c_I2c_Device *dev,
                                      volatile uint8 *data,
                                      Ifx_SizeT size)
{
    Ifx_I2C *i2c = dev->i2c->i2c;
    uint8 busIdx = prv_BusIdxFromDev(dev);
    IfxI2c_BusStatus busStatus;
    Ifx_SizeT remaining;
    boolean addrPending;
    union
    {
        uint32 packet;
        uint8  b[4];
    } tx;

    if (size < 0)
    {
        return I2C_ERR_BUS_BUSY;
    }

    remaining = size + 1;   /* + 7-bit address byte */
    if (remaining > 16383)
    {
        return I2C_ERR_BUS_BUSY;
    }

    busStatus = IfxI2c_getBusStatus(i2c);
    if ((busStatus != IfxI2c_BusStatus_idle) &&
        (busStatus != IfxI2c_BusStatus_busyMaster))
    {
        return I2C_ERR_BUS_BUSY;
    }

    IfxI2c_setTransmitPacketSize(i2c, remaining);
    addrPending = TRUE;

    while (remaining > 0)
    {
        uint32 i;

        if (prv_WaitRis(i2c) == FALSE)
        {
            return prv_Timeout(busIdx);
        }

        if (IfxI2c_isFifoRequest(i2c) == TRUE)
        {
            tx.packet = 0u;
            for (i = 0u; (i < 4u) && (remaining > 0); i++)
            {
                if (addrPending != FALSE)
                {
                    tx.b[i] = (uint8)(dev->deviceAddress & 0xFEu);
                    addrPending = FALSE;
                }
                else
                {
                    tx.b[i] = *data++;
                }
                remaining--;
            }
            IfxI2c_writeFifo(i2c, tx.packet);
            IfxI2c_clearAllDtrInterruptSources(i2c);
        }
        else
        {
            remaining = 0;
        }
    }

    if (prv_WaitRis(i2c) == FALSE)
    {
        return prv_Timeout(busIdx);
    }

    if (IfxI2c_getProtocolInterruptSourceStatus(
            i2c, IfxI2c_ProtocolInterruptSource_arbitrationLost) == TRUE)
    {
        IfxI2c_clearProtocolInterruptSource(
            i2c, IfxI2c_ProtocolInterruptSource_arbitrationLost);
        return I2C_ERR_ARB_LOST;
    }

    if (IfxI2c_getErrorInterruptSourceStatus(
            i2c, IfxI2c_ErrorInterruptSource_txFifoOverflow) == TRUE)
    {
        IfxI2c_clearErrorInterruptSource(
            i2c, IfxI2c_ErrorInterruptSource_txFifoOverflow);
        if (prv_WaitTxEnd(i2c) == FALSE)
        {
            return prv_Timeout(busIdx);
        }
        IfxI2c_clearProtocolInterruptSource(
            i2c, IfxI2c_ProtocolInterruptSource_transmissionEnd);
        return I2C_ERR_TIMEOUT;
    }

    if (IfxI2c_getProtocolInterruptSourceStatus(
            i2c, IfxI2c_ProtocolInterruptSource_notAcknowledgeReceived) == TRUE)
    {
        IfxI2c_clearProtocolInterruptSource(
            i2c, IfxI2c_ProtocolInterruptSource_notAcknowledgeReceived);
        if (prv_WaitTxEnd(i2c) == FALSE)
        {
            return prv_Timeout(busIdx);
        }
        IfxI2c_clearProtocolInterruptSource(
            i2c, IfxI2c_ProtocolInterruptSource_transmissionEnd);
        return I2C_ERR_NAK;
    }

    if (prv_WaitTxEnd(i2c) == FALSE)
    {
        return prv_Timeout(busIdx);
    }
    IfxI2c_clearProtocolInterruptSource(
        i2c, IfxI2c_ProtocolInterruptSource_transmissionEnd);
    return I2C_OK;
}

static I2c_Status_t prv_Read2Bounded(IfxI2c_I2c_Device *dev,
                                     volatile uint8 *data,
                                     Ifx_SizeT size)
{
    Ifx_I2C *i2c = dev->i2c->i2c;
    uint8 busIdx = prv_BusIdxFromDev(dev);
    IfxI2c_BusStatus busStatus;
    union
    {
        uint32 packet;
        uint8  b[4];
    } rx;

    if ((size < 0) || (size > 16383))
    {
        return I2C_ERR_BUS_BUSY;
    }

    busStatus = IfxI2c_getBusStatus(i2c);
    if ((busStatus != IfxI2c_BusStatus_idle) &&
        (busStatus != IfxI2c_BusStatus_busyMaster))
    {
        return I2C_ERR_BUS_BUSY;
    }

    IfxI2c_setTransmitPacketSize(i2c, 1);
    IfxI2c_setReceivePacketSize(i2c, size);

    if (prv_WaitRis(i2c) == FALSE)
    {
        return prv_Timeout(busIdx);
    }

    if (IfxI2c_isFifoRequest(i2c) == TRUE)
    {
        IfxI2c_writeFifo(i2c, (uint32)(dev->deviceAddress | 0x1u));
        IfxI2c_clearAllDtrInterruptSources(i2c);
    }

    if (prv_WaitRis(i2c) == FALSE)
    {
        return prv_Timeout(busIdx);
    }

    if (IfxI2c_getProtocolInterruptSourceStatus(
            i2c, IfxI2c_ProtocolInterruptSource_arbitrationLost) == TRUE)
    {
        IfxI2c_clearProtocolInterruptSource(
            i2c, IfxI2c_ProtocolInterruptSource_arbitrationLost);
        return I2C_ERR_ARB_LOST;
    }

    if (IfxI2c_getErrorInterruptSourceStatus(
            i2c, IfxI2c_ErrorInterruptSource_txFifoOverflow) == TRUE)
    {
        IfxI2c_clearErrorInterruptSource(
            i2c, IfxI2c_ErrorInterruptSource_txFifoOverflow);
        if (prv_WaitTxEnd(i2c) == FALSE)
        {
            return prv_Timeout(busIdx);
        }
        IfxI2c_clearProtocolInterruptSource(
            i2c, IfxI2c_ProtocolInterruptSource_transmissionEnd);
        return I2C_ERR_TIMEOUT;
    }

    if (IfxI2c_getProtocolInterruptSourceStatus(
            i2c, IfxI2c_ProtocolInterruptSource_notAcknowledgeReceived) == TRUE)
    {
        IfxI2c_clearProtocolInterruptSource(
            i2c, IfxI2c_ProtocolInterruptSource_notAcknowledgeReceived);
        if (prv_WaitTxEnd(i2c) == FALSE)
        {
            return prv_Timeout(busIdx);
        }
        IfxI2c_clearProtocolInterruptSource(
            i2c, IfxI2c_ProtocolInterruptSource_transmissionEnd);
        return I2C_ERR_NAK;
    }

    if (IfxI2c_getProtocolInterruptSourceStatus(
            i2c, IfxI2c_ProtocolInterruptSource_receiveMode) == FALSE)
    {
        IfxI2c_clearAllProtocolInterruptSources(i2c);
        return I2C_ERR_TIMEOUT;
    }
    IfxI2c_clearProtocolInterruptSource(
        i2c, IfxI2c_ProtocolInterruptSource_receiveMode);

    while (size > 0)
    {
        uint32 i;

        if (prv_WaitRis(i2c) == FALSE)
        {
            return prv_Timeout(busIdx);
        }

        if (IfxI2c_isFifoRequest(i2c) == TRUE)
        {
            rx.packet = i2c->RXD.U;
            for (i = 0u; (i < 4u) && (size > 0); i++)
            {
                *data++ = rx.b[i];
                size--;
            }
            IfxI2c_clearAllDtrInterruptSources(i2c);
        }
        else
        {
            size = 0;
        }
    }

    if (prv_WaitRis(i2c) == FALSE)
    {
        return prv_Timeout(busIdx);
    }

    if (IfxI2c_getErrorInterruptSourceStatus(
            i2c, IfxI2c_ErrorInterruptSource_rxFifoOverflow) == TRUE)
    {
        IfxI2c_clearErrorInterruptSource(
            i2c, IfxI2c_ErrorInterruptSource_rxFifoOverflow);
        if (prv_WaitTxEnd(i2c) == FALSE)
        {
            return prv_Timeout(busIdx);
        }
        IfxI2c_clearProtocolInterruptSource(
            i2c, IfxI2c_ProtocolInterruptSource_transmissionEnd);
        return I2C_ERR_TIMEOUT;
    }

    if (prv_WaitTxEnd(i2c) == FALSE)
    {
        return prv_Timeout(busIdx);
    }
    IfxI2c_clearProtocolInterruptSource(
        i2c, IfxI2c_ProtocolInterruptSource_transmissionEnd);
    return I2C_OK;
}

void I2cMaster_Init(void)
{
    IfxI2c_I2c_Config cfg;
    /* Local const — automatic storage, no E306 restriction on initialiser. */
    const IfxI2c_Pins pins = {
        &IfxI2c0_SCL_P13_1_INOUT,   /* P13.1 SCL */
        &IfxI2c0_SDA_P13_2_INOUT,   /* P13.2 SDA */
        IfxPort_PadDriver_cmosAutomotiveSpeed1
    };

    IfxI2c_I2c_initConfig(&cfg, &MODULE_I2C0);
    cfg.baudrate = (float32)I2C_MASTER_FREQ_HZ;
    cfg.mode     = IfxI2c_Mode_StandardAndFast;
    cfg.pins     = &pins;

    IfxI2c_I2c_initModule(&s_i2cHandle, &cfg);
    /* ---- I2C1: APML SB-TSI (P11.14 SCL / P11.13 SDA) ------------------- */
    const IfxI2c_Pins pins1 = {
        &IfxI2c1_SCL_P11_14_INOUT,
        &IfxI2c1_SDA_P11_13_INOUT,
        IfxPort_PadDriver_cmosAutomotiveSpeed1
    };

    IfxI2c_I2c_initConfig(&cfg, &MODULE_I2C1);
    cfg.baudrate = 400000.0f;   /* 400 kHz fast-mode per PPR §5.3.2 */
    cfg.mode     = IfxI2c_Mode_StandardAndFast;
    cfg.pins     = &pins1;

    IfxI2c_I2c_initModule(&s_i2c1Handle, &cfg);
    Debug_Printf("[I2C] init: bus0 P13.1/2 busy=%u, bus1 P11.14/13 busy=%u\r\n",
                (unsigned)prv_BusIsBusy(0u), (unsigned)prv_BusIsBusy(1u));
    if (prv_BusIsBusy(0u)) I2cMaster_ReinitBus(0u);
    if (prv_BusIsBusy(1u)) I2cMaster_ReinitBus(1u);
}

I2c_Status_t I2cMaster_Write(uint8 addr7bit, const uint8 *pData, uint16 len)
{
    I2c_Status_t st;

    if (!prv_WaitBusIdle(0u, I2C_MASTER_TIMEOUT_MS))
        return I2C_ERR_BUS_BUSY;
    prv_SetDevice(addr7bit, FALSE);
    prv_LockBusForMaster(0u, &s_deviceHandle);
    st = prv_Write2Bounded(&s_deviceHandle, (volatile uint8 *)pData, (Ifx_SizeT)len);
    prv_UnlockBus(0u);
    return st;
}

I2c_Status_t I2cMaster_ReadReg16_Bus(uint8 busIdx, uint8 addr7bit,
                                      uint16 regAddr, uint8 *pBuf,
                                      uint16 len)
{
    uint8 addrBytes[2];
    I2c_Status_t st;
    IfxI2c_I2c *busHandle;
    IfxI2c_I2c_deviceConfig devCfg;
    IfxI2c_I2c_Device localDev;

    if (!prv_WaitBusIdle(busIdx, I2C_MASTER_TIMEOUT_MS))
        return I2C_ERR_BUS_BUSY;

    if (busIdx == 0u)
        busHandle = &s_i2cHandle;
    else if (busIdx == 1u)
        busHandle = &s_i2c1Handle;
    else
        return I2C_ERR_BUS_BUSY;

    IfxI2c_I2c_initDeviceConfig(&devCfg, busHandle);
    devCfg.deviceAddress       = (uint16)((uint16)addr7bit << 1u);
    devCfg.enableRepeatedStart = TRUE;

    IfxI2c_I2c_initDevice(&localDev, &devCfg);

    addrBytes[0] = (uint8)(regAddr & 0x00FFu);
    addrBytes[1] = (uint8)(regAddr >> 8u);

    prv_LockBusForMaster(busIdx, &localDev);
    st = prv_Write2Bounded(&localDev, (volatile uint8 *)addrBytes, (Ifx_SizeT)2);
    if (st != I2C_OK)
    {
        prv_UnlockBus(busIdx);
        return st;
    }

    st = prv_Read2Bounded(&localDev, (volatile uint8 *)pBuf, (Ifx_SizeT)len);
    prv_UnlockBus(busIdx);
    return st;
}

I2c_Status_t I2cMaster_WriteReg16_Bus(uint8 busIdx, uint8 addr7bit,
                                       uint16 regAddr, const uint8 *pData,
                                       uint16 len)
{
    uint8 buf[2u + 32u];
    uint16 i;
    I2c_Status_t st;
    IfxI2c_I2c *busHandle;
    IfxI2c_I2c_deviceConfig devCfg;
    IfxI2c_I2c_Device localDev;

    if (!prv_WaitBusIdle(busIdx, I2C_MASTER_TIMEOUT_MS))
        return I2C_ERR_BUS_BUSY;
    if (len > 32u)
        return I2C_ERR_BUS_BUSY;

    if (busIdx == 0u)
        busHandle = &s_i2cHandle;
    else if (busIdx == 1u)
        busHandle = &s_i2c1Handle;
    else
        return I2C_ERR_BUS_BUSY;

    IfxI2c_I2c_initDeviceConfig(&devCfg, busHandle);
    devCfg.deviceAddress       = (uint16)((uint16)addr7bit << 1u);
    devCfg.enableRepeatedStart = FALSE;

    IfxI2c_I2c_initDevice(&localDev, &devCfg);

    buf[0] = (uint8)(regAddr & 0x00FFu);
    buf[1] = (uint8)(regAddr >> 8u);
    for (i = 0u; i < len; i++)
        buf[2u + i] = pData[i];

    prv_LockBusForMaster(busIdx, &localDev);
    st = prv_Write2Bounded(&localDev, (volatile uint8 *)buf, (Ifx_SizeT)(2u + len));
    prv_UnlockBus(busIdx);
    return st;
}

I2c_Status_t I2cMaster_ApmlReadByte(uint8 addr7bit, uint8 regAddr, uint8 *pData)
{
    /* SB-TSI (PPR §5.2): combined-format repeated start is UNSUPPORTED —
     * undefined behavior.  Use Send Byte (pointer load, STOP) then a
     * separate Receive Byte transaction. */
    IfxI2c_I2c_deviceConfig devCfg;
    I2c_Status_t st;

    if (!prv_WaitBusIdle(1u, I2C_MASTER_TIMEOUT_MS))
        return I2C_ERR_BUS_BUSY;
    IfxI2c_I2c_initDeviceConfig(&devCfg, &s_i2c1Handle);
    devCfg.deviceAddress       = (uint16)((uint16)addr7bit << 1u);
    devCfg.enableRepeatedStart = FALSE;              /* STOP between transfers */
    IfxI2c_I2c_initDevice(&s_apmlDevHandle, &devCfg);

    prv_LockBusForMaster(1u, &s_apmlDevHandle);
    st = prv_Write2Bounded(&s_apmlDevHandle, (volatile uint8 *)&regAddr, 1);
    if (st != I2C_OK)
    {
        prv_UnlockBus(1u);
        return st;
    }
    st = prv_Read2Bounded(&s_apmlDevHandle, (volatile uint8 *)pData, 1);
    prv_UnlockBus(1u);
    return st;
}

I2c_Status_t I2cMaster_ReadReg8_Bus(uint8 busIdx, uint8 addr7bit,
                                    uint8 regAddr, uint8 *pData)
{
    IfxI2c_I2c *bus;
    IfxI2c_I2c_deviceConfig devCfg;
    IfxI2c_I2c_Device dev;
    I2c_Status_t st;

    if (!prv_WaitBusIdle(busIdx, I2C_MASTER_TIMEOUT_MS))
        return I2C_ERR_BUS_BUSY;
    bus = (busIdx == 0u) ? &s_i2cHandle : &s_i2c1Handle;
    IfxI2c_I2c_initDeviceConfig(&devCfg, bus);
    devCfg.deviceAddress       = (uint16)((uint16)addr7bit << 1u);
    devCfg.enableRepeatedStart = TRUE;
    IfxI2c_I2c_initDevice(&dev, &devCfg);

    prv_LockBusForMaster(busIdx, &dev);
    st = prv_Write2Bounded(&dev, (volatile uint8 *)&regAddr, 1);
    if (st != I2C_OK)
    {
        prv_UnlockBus(busIdx);
        return st;
    }
    st = prv_Read2Bounded(&dev, (volatile uint8 *)pData, 1);
    prv_UnlockBus(busIdx);
    return st;
}
