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

/* ================================================================== */
/*  Public API                                                        */
/* ================================================================== */

uint8 UsbPd_Hpi_ReadPortState(uint8 i2cAddr, uint16 portBase,
                              UsbPd_HpiState_t *pState)
{
    uint8 err = 0u;
    uint8 altMode8 = 0u;
    uint8 typeC8   = 0u;

    /* TYPE_C_STATUS is a 1-byte register; read 1 byte (was wrongly read as 4,
     * which pulled in BUS_VOLTAGE / CURRENT_PDO bytes). */
    err |= prv_ReadReg8(i2cAddr, portBase + HPI_PORT_TYPE_C_STATUS, &typeC8);
    pState->typeCStatus = (uint32)typeC8;

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

uint8 UsbPd_Hpi_ReadDevIntr(uint8 i2cAddr, uint8 *pIntr)
{
    return prv_ReadReg8(i2cAddr, HPI_INTR_REG, pIntr);
}