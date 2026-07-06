/**
 * @file    I2c_Master.h
 * @brief   Polling I2C master wrapper over iLLD IfxI2c.
 *
 * Configured for I2C0, 400 kHz Fast Mode, P13.1 (SCL) / P13.2 (SDA).
 * All transactions are blocking with a STM-based timeout guard.
 * The CYPD6129 HPI register access protocol uses 2-byte little-endian
 * register addresses, so both raw byte-array and addressed-read helpers
 * are provided.
 */

#ifndef I2C_MASTER_H
#define I2C_MASTER_H

#include "Ifx_Types.h"

/** I2C bus frequency — 400 kHz Fast Mode. */
#define I2C_MASTER_FREQ_HZ      400000u

/* Note: iLLD 1.20.0 write2/read2 are blocking internally.
 * I2C_MASTER_TIMEOUT_MS is retained for potential future use. */
#define I2C_MASTER_TIMEOUT_MS   10u

typedef enum
{
    I2C_OK      = 0,
    I2C_ERR_NAK,
    I2C_ERR_ARB_LOST,
    I2C_ERR_TIMEOUT,
    I2C_ERR_BUS_BUSY
} I2c_Status_t;

/**
 * @brief Initialise I2C0 master. Call after Stm_Init().
 */
void I2cMaster_Init(void);

/**
 * @brief Write len bytes from pData to the device at addr (7-bit).
 */
I2c_Status_t I2cMaster_Write(uint8 addr7bit, const uint8 *pData, uint16 len);

/**
 * @brief Read len bytes from the device at addr into pBuf.
 *        Performs a combined write (register address) then repeated-START read.
 *        regAddr is sent as a 2-byte little-endian value to match the
 *        CYPD6129 HPI addressing scheme.
 */
I2c_Status_t I2cMaster_ReadReg16(uint8 addr7bit, uint16 regAddr,
                                  uint8 *pBuf,    uint16 len);

/**
 * @brief Write len bytes to a 16-bit register address.
 */
I2c_Status_t I2cMaster_WriteReg16(uint8 addr7bit, uint16 regAddr,
                                   const uint8 *pData, uint16 len);

#endif /* I2C_MASTER_H */
