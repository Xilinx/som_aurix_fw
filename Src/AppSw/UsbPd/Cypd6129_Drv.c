/**
 * @file    Cypd6129_Drv.c
 * @brief   CYPD6129 HPI driver implementation.
 */

#include "Cypd6129_Drv.h"
#include "AppPin.h"
#include "I2c_Master.h"
#include "Stm_Timer.h"
#include "Uart_Debug.h"
#include "IfxPort.h"
#include "UsbPd_Cfg.h"


#if (USBPD_FEATURE_ENABLE == 1u)
static boolean s_devInitOk[CYPD_DEVICE_COUNT];
/* ---- Private helpers ----------------------------------------------------- */

static uint8 prv_Addr(uint8 devIdx)
{
    return CYPD_DEVICES[devIdx].i2cAddr;
}

static Cypd_Status_t prv_MapI2c(I2c_Status_t st)
{
    return (st == I2C_OK) ? CYPD_OK : CYPD_ERR_I2C;
}

static Cypd_Status_t prv_Read16(uint8 devIdx, uint16 reg, uint16 *pOut)
{
    uint8 buf[2];
    I2c_Status_t st = I2cMaster_ReadReg16_Bus(
        CYPD_DEVICES[devIdx].i2cBus,
        prv_Addr(devIdx), reg, buf, 2u);
    if (st != I2C_OK)
        return CYPD_ERR_I2C;
    *pOut = (uint16)(((uint16)buf[1] << 8u) | (uint16)buf[0]);
    return CYPD_OK;
}


static Cypd_Status_t prv_Read8(uint8 devIdx, uint16 reg, uint8 *pOut)
{
    return prv_MapI2c(I2cMaster_ReadReg16_Bus(
        CYPD_DEVICES[devIdx].i2cBus,
        prv_Addr(devIdx), reg, pOut, 1u));
}

static Cypd_Status_t prv_Read32(uint8 devIdx, uint16 reg, uint32 *pOut)
{
    uint8 buf[4];
    I2c_Status_t st = I2cMaster_ReadReg16_Bus(
        CYPD_DEVICES[devIdx].i2cBus,
        prv_Addr(devIdx), reg, buf, 4u);
    if (st != I2C_OK)
        return CYPD_ERR_I2C;
    *pOut = ((uint32)buf[3] << 24u) | ((uint32)buf[2] << 16u) |
            ((uint32)buf[1] << 8u)  | (uint32)buf[0];
    return CYPD_OK;
}

static Cypd_Status_t prv_Write8(uint8 devIdx, uint16 reg, uint8 val)
{
    return prv_MapI2c(I2cMaster_WriteReg16_Bus(
        CYPD_DEVICES[devIdx].i2cBus,
        prv_Addr(devIdx), reg, &val, 1u));
}
/* ---- Public API ---------------------------------------------------------- */

Cypd_FwImage_t Cypd_DecodeImage(uint16 mode)
{
    return (Cypd_FwImage_t)(mode & CYPD_DEVICE_MODE_IMAGE_MASK);
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

    /* Assert RESET_L (drive low) */
    IfxPort_setPinLow(AppPin_GetPort(dev->resetPin.portIdx), dev->resetPin.pinIdx);
    Stm_DelayMs(CYPD_RESET_PULSE_MS);

    /* Deassert RESET_L (drive high) */
    IfxPort_setPinHigh(AppPin_GetPort(dev->resetPin.portIdx), dev->resetPin.pinIdx);

    /* Wait for device to boot */
    Stm_DelayMs(CYPD_BOOT_DELAY_MS);

    st = Cypd_ReadDeviceMode(devIdx, &mode);
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
     * bit 0 alone wrongly rejects FW2 (e.g. MODE=0x0092 → bits[1:0]=10=FW2).
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

    /* img == CYPD_IMG_BOOTLOADER: device stuck in bootloader — retry once. */
    Debug_Printf("[CYPD %s] Reset: device in bootloader — retrying\r\n",
                 dev->name);

    /* Second reset attempt */
    IfxPort_setPinLow(AppPin_GetPort(dev->resetPin.portIdx),
                      dev->resetPin.pinIdx);
    Stm_DelayMs(CYPD_RESET_PULSE_MS);
    IfxPort_setPinHigh(AppPin_GetPort(dev->resetPin.portIdx),
                       dev->resetPin.pinIdx);
    Stm_DelayMs(CYPD_BOOT_DELAY_MS);

    st = Cypd_ReadDeviceMode(devIdx, &mode);
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

Cypd_Status_t Cypd_ReadDeviceMode(uint8 devIdx, uint16 *pMode)
{
    if ((devIdx >= CYPD_DEVICE_COUNT) || (pMode == NULL_PTR))
    {
        return CYPD_ERR_INVALID;
    }
    return prv_Read16(devIdx, CYPD_REG_DEVICE_MODE, pMode);
}

Cypd_Status_t Cypd_ReadPortStatus(uint8 devIdx, Cypd_PortStatus_t *pStatus)
{
    if ((devIdx >= CYPD_DEVICE_COUNT) || (pStatus == NULL_PTR))
    {
        return CYPD_ERR_INVALID;
    }

    uint16 tcStatus = 0u;
    uint16 busVolt  = 0u;
    uint32 events   = 0u;

    Cypd_Status_t st;

    if ((devIdx >= CYPD_DEVICE_COUNT) || (pStatus == NULL_PTR))
    {
        return CYPD_ERR_INVALID;
    }

    st = prv_Read16(devIdx, CYPD_REG_TYPE_C_STATUS, &tcStatus);
    if (st != CYPD_OK) return st;

    st = prv_Read16(devIdx, CYPD_REG_BUS_VOLTAGE, &busVolt);
    if (st != CYPD_OK) return st;

    st = prv_Read32(devIdx, CYPD_REG_PORT_EVENT, &events);
    if (st != CYPD_OK) return st;

    pStatus->attached         = (boolean)((tcStatus & CYPD_TC_STATUS_ATTACHED) != 0u);
    pStatus->cc2Polarity      = (boolean)((tcStatus & CYPD_TC_STATUS_CC_POLARITY) != 0u);
    pStatus->isDfp            = (boolean)((tcStatus & CYPD_TC_STATUS_DFP) != 0u);
    pStatus->vbusVoltage_100mV = busVolt;
    pStatus->portEvents        = events;

    return CYPD_OK;
}

Cypd_Status_t Cypd_ReadIntrReg(uint8 devIdx, uint8 *pIntr)
{
    if ((devIdx >= CYPD_DEVICE_COUNT) || (pIntr == NULL_PTR))
    {
        return CYPD_ERR_INVALID;
    }
    return prv_Read8(devIdx, CYPD_REG_INTR_REG, pIntr);
}

Cypd_Status_t Cypd_ClearIntr(uint8 devIdx, uint8 mask)
{
    if (devIdx >= CYPD_DEVICE_COUNT)
    {
        return CYPD_ERR_INVALID;
    }
    /* INTR_REG is write-1-to-clear */
    return prv_Write8(devIdx, CYPD_REG_INTR_REG, mask);
}

Cypd_Status_t Cypd_ReadPortEvent(uint8 devIdx, uint32 *pEvent)
{
    if ((devIdx >= CYPD_DEVICE_COUNT) || (pEvent == NULL_PTR))
    {
        return CYPD_ERR_INVALID;
    }
    return prv_Read32(devIdx, CYPD_REG_PORT_EVENT, pEvent);
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

#endif