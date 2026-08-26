/**
 * @file    I2c_Slave.h
 * @brief   I2C1 slave driver for APU USB-C sideband proxy
 *
 * The TC3xx I2C peripheral supports simultaneous master and slave
 * operation.  This driver configures I2C1 to respond to slave
 * addresses 0x54 and 0x58 while the existing I2c_Master driver
 * continues to use I2C1 for APML reads.
 *
 * The iLLD does not provide an I2C slave wrapper, so this driver
 * accesses the I2C1 registers directly.
 *
 * When the APU reads from 0x54, the slave serves the port-0
 * APU proxy register map.  When it reads 0x58, port-1.
 *
 * The slave response is interrupt-driven: the Protocol Interrupt
 * (I2C_PIRQSM) fires on address match, and the slave loads the
 * TX FIFO with the proxy register bytes.
 *
 * IMPORTANT: On the eval board (TARGET_EVAL_BOARD), this is
 * compiled but I2C1 has no APU connected.  The slave address
 * match will simply never fire.
 */

#ifndef I2C_SLAVE_H
#define I2C_SLAVE_H

#include "Ifx_Types.h"

/* ================================================================== */
/*  Configuration                                                     */
/* ================================================================== */

#ifndef I2C_SLAVE_FEATURE_ENABLE
#define I2C_SLAVE_FEATURE_ENABLE    1u
#endif

/* APU slave addresses (7-bit) */
#define I2C_SLV_USBC0_ADDR     0x54u
#define I2C_SLV_USBC1_ADDR     0x58u

/* ISR priority — must not conflict with I2C master or other ISRs */
#define I2C1_SLAVE_ISR_PRIO    50u

/* ================================================================== */
/*  Callback type for providing response data                         */
/* ================================================================== */

/**
 * Callback invoked from the slave ISR when the APU reads a register.
 *
 * @param  portIdx    Which port was addressed (0 for 0x54, 1 for 0x58)
 * @param  regAddr    Register offset the APU is reading
 * @param  pData      Output: byte to send back
 * @return TRUE if data is valid, FALSE to NACK
 */
typedef boolean (*I2cSlave_ReadCb_t)(uint8 portIdx, uint8 regAddr,
                                     uint8 *pData);

/* ================================================================== */
/*  Public API                                                        */
/* ================================================================== */

/**
 * @brief  Initialise I2C1 slave mode alongside existing master mode.
 *
 * Configures the I2C1 address register for slave addresses
 * 0x54 and 0x58.  Enables the protocol interrupt for address
 * match and read-request handling.
 *
 * Must be called AFTER I2cMaster_Init() (which sets up I2C1
 * for APML master transactions).
 *
 * @param  readCb  Callback function for serving APU read requests
 */
void I2cSlave_Init(I2cSlave_ReadCb_t readCb);

/**
 * @brief  Check if the slave is currently handling a transaction.
 *
 * The APML master should check this before starting a master
 * transaction to avoid bus conflicts.
 */
boolean I2cSlave_IsBusy(void);

/**
 * @brief  Temporarily disable slave address matching.
 *
 * Call before an APML master transaction if the hardware
 * cannot do simultaneous master+slave on the same bus.
 */
void I2cSlave_Suspend(void);

/**
 * @brief  Re-enable slave address matching.
 *
 * Call after an APML master transaction completes.
 */
void I2cSlave_Resume(void);

#endif /* I2C_SLAVE_H */