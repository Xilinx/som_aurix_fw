/**
 * @file    UsbPd_Hpi.c
 * @brief   HPI register I2C reads for CYPD6129/6229
 */

#include "UsbPd_Hpi.h"
#include "I2c_Master.h"
#include "Stm_Timer.h"

#define HPI_I2C_BUS     0u

/* ================================================================== */
/*  Private helpers                                                   */
/* ================================================================== */

static uint8 prv_ReadReg8(uint8 i2cAddr, uint16 regAddr, uint8 *pVal)
{
    I2c_Status_t st = I2cMaster_ReadReg16_Bus(HPI_I2C_BUS, i2cAddr,
                                               regAddr, pVal, 1u);
    return (st == I2C_OK) ? 0u : 1u;
}

static uint8 prv_ReadReg32(uint8 i2cAddr, uint16 regAddr, uint32 *pVal)
{
    uint8 buf[4] = {0};
    I2c_Status_t st = I2cMaster_ReadReg16_Bus(HPI_I2C_BUS, i2cAddr,
                                               regAddr, buf, 4u);
    if (st != I2C_OK) return 1u;

    *pVal = (uint32)buf[0]
          | ((uint32)buf[1] << 8u)
          | ((uint32)buf[2] << 16u)
          | ((uint32)buf[3] << 24u);
    return 0u;
}

static uint8 prv_WriteReg8(uint8 i2cAddr, uint16 regAddr, uint8 val)
{
    I2c_Status_t st = I2cMaster_WriteReg16_Bus(HPI_I2C_BUS, i2cAddr,
                                                regAddr, &val, 1u);
    return (st == I2C_OK) ? 0u : 1u;
}

/* ================================================================== */
/*  Public API                                                        */
/* ================================================================== */

uint8 UsbPd_Hpi_ReadPortState(uint8 i2cAddr, uint16 portBase,
                              UsbPd_HpiState_t *pState)
{
    uint8 err = 0u;
    uint8 altMode8 = 0u;

    err |= prv_ReadReg32(i2cAddr, portBase + HPI_PORT_TYPE_C_STATUS,
                         &pState->typeCStatus);
    err |= prv_ReadReg32(i2cAddr, portBase + HPI_PORT_PD_STATUS,
                         &pState->pdStatus);
    err |= prv_ReadReg8(i2cAddr, portBase + HPI_PORT_ALT_MODE_STATUS,
                        &altMode8);
    pState->altModeStatus = altMode8;

    err |= prv_ReadReg32(i2cAddr, portBase + HPI_PORT_CURRENT_CABLE_VDO,
                         &pState->currentCableVdo);
    err |= prv_ReadReg32(i2cAddr, portBase + HPI_PORT_ACT_CBL_VDO_2,
                         &pState->actCblVdo2);

    if (err == 0u)
    {
        pState->connected    = (boolean)((pState->typeCStatus & HPI_TC_CONNECTED) != 0u);
        pState->contractValid = (boolean)((pState->pdStatus & HPI_PD_CONTRACT_EXISTS) != 0u);
        pState->lastUpdateMs = Stm_GetTimeMs();
    }

    return err;
}

uint8 UsbPd_Hpi_ReadClearEvents(uint8 i2cAddr, uint16 portBase,
                                uint32 *pEvents)
{
    uint32 events = 0u;
    uint8 err = prv_ReadReg32(i2cAddr, portBase + HPI_PORT_EVENT_MASK,
                              &events);
    if (err != 0u) return err;

    *pEvents = events;

    /* Clear by writing back the event bits (write-1-to-clear) */
    if (events != 0u)
    {
        uint8 clearBuf[6];
        uint16 regAddr = portBase + HPI_PORT_EVENT_MASK;
        clearBuf[0] = (uint8)(regAddr >> 8u);
        clearBuf[1] = (uint8)(regAddr & 0xFFu);
        clearBuf[2] = (uint8)(events);
        clearBuf[3] = (uint8)(events >> 8u);
        clearBuf[4] = (uint8)(events >> 16u);
        clearBuf[5] = (uint8)(events >> 24u);
        I2cMaster_Write(i2cAddr, clearBuf, 6u);
    }

    return 0u;
}

uint8 UsbPd_Hpi_ReadDevIntr(uint8 i2cAddr, uint8 *pIntr)
{
    return prv_ReadReg8(i2cAddr, HPI_INTR_REG, pIntr);
}

uint8 UsbPd_Hpi_WriteHpdCtrl(uint8 i2cAddr, uint16 portBase, uint8 hpdCmd)
{
    return prv_WriteReg8(i2cAddr, portBase + HPI_PORT_DP_HPD_CTRL, hpdCmd);
}