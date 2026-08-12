/**
 * @file    Cypd6129_Drv.h
 * @brief   CYPD6129 HPI (Host Processor Interface) register driver.
 *
 * The CYPD6129 exposes its state via a register-mapped I2C interface called
 * HPI (Host Processor Interface). Registers are addressed using 2-byte
 * little-endian addresses matching the I2cMaster_ReadReg16 / WriteReg16 API.
 *
 * Reference: Infineon CYPD6129 HPI Specification (Document 002-24049).
 */

#ifndef CYPD6129_DRV_H
#define CYPD6129_DRV_H

#include "UsbPd_Cfg.h"
#include "Ifx_Types.h"

#define USBPD_FEATURE_ENABLE   1u


/* ---- HPI Register Map ---------------------------------------------------- */
#define CYPD_REG_DEVICE_MODE        0x0000u  /* R  — device mode/fw status    */
#define CYPD_REG_BOOT_MODE_REASON   0x0002u  /* R  — reason for boot mode     */
#define CYPD_REG_SILICON_ID         0x0004u  /* R  — silicon ID               */
#define CYPD_REG_INTR_REG           0x0006u  /* RW — interrupt status (W1C)   */
#define CYPD_REG_JUMP_TO_BOOT       0x0007u  /* W  — command: jump to boot    */
#define CYPD_REG_RESET_REQ          0x0008u  /* W  — soft reset               */
#define CYPD_REG_PD_STATUS          0x001Cu  /* R  — PD contract status       */
#define CYPD_REG_TYPE_C_STATUS      0x001Eu  /* R  — Type-C attachment status */
#define CYPD_REG_BUS_VOLTAGE        0x0020u  /* R  — VBUS voltage (100 mV)    */
#define CYPD_REG_CURR_LIMIT         0x0022u  /* RW — current limit            */
#define CYPD_REG_SWAP_RESPONSE      0x0028u  /* RW — PR/DR swap response cfg  */
#define CYPD_REG_EVENT_MASK         0x002Au  /* RW — event notification mask  */
#define CYPD_REG_PORT_ENABLE        0x002Cu  /* RW — port enable              */
#define CYPD_REG_PDSS_STATUS        0x002Eu  /* R  — PDSS block status        */
#define CYPD_REG_PORT_EVENT         0x0030u  /* R  — port event register      */
#define CYPD_REG_HPI_CMD            0x0100u  /* W  — HPI command register     */
#define CYPD_REG_DATA_MEM           0x0400u  /* RW — HPI data memory window   */

/* ---- INTR_REG bits ------------------------------------------------------- */
#define CYPD_INTR_PORT_EVENT        (1u << 0)   /* port event pending */
#define CYPD_INTR_DEV_EVENT         (1u << 1)   /* device event pending */

/* ---- PORT_EVENT bits ----------------------------------------------------- */
#define CYPD_EVT_TYPEC_ATTACH       (1u << 0)
#define CYPD_EVT_TYPEC_DETACH       (1u << 1)
#define CYPD_EVT_PD_CONTRACT        (1u << 2)
#define CYPD_EVT_PR_SWAP_DONE       (1u << 3)
#define CYPD_EVT_DR_SWAP_DONE       (1u << 4)
#define CYPD_EVT_VBUS_OVP           (1u << 7)
#define CYPD_EVT_VBUS_OCP           (1u << 8)

/* ---- TYPE_C_STATUS bits -------------------------------------------------- */
#define CYPD_TC_STATUS_ATTACHED     (1u << 0)
#define CYPD_TC_STATUS_CC_POLARITY  (1u << 1)   /* 0=CC1, 1=CC2 */
#define CYPD_TC_STATUS_DFP          (1u << 2)   /* device is DFP */

/* ---- Return status ------------------------------------------------------- */
typedef enum
{
    CYPD_OK             = 0,
    CYPD_ERR_I2C,
    CYPD_ERR_TIMEOUT,
    CYPD_ERR_BOOT_MODE, /* device still in bootloader */
    CYPD_ERR_INVALID,
} Cypd_Status_t;

/* ---- Decoded port status ------------------------------------------------- */
typedef struct
{
    boolean  attached;
    boolean  cc2Polarity;       /* TRUE = CC2 is orientation reference */
    boolean  isDfp;
    uint16   vbusVoltage_100mV;
    uint32   portEvents;        /* raw PORT_EVENT register */
} Cypd_PortStatus_t;

/* ---- Driver API ---------------------------------------------------------- */

/**
 * @brief Hard-reset a CYPD6129 by toggling its RESET_L pin, then wait for
 *        the device to boot and confirm HPI is accessible.
 * @param devIdx  Index into CYPD_DEVICES[] (0 or 1).
 */
Cypd_Status_t Cypd_HardReset(uint8 devIdx);

/**
 * @brief Read the DEVICE_MODE register to verify the device is operational.
 * @param devIdx   Index into CYPD_DEVICES[].
 * @param pMode    Output: raw DEVICE_MODE word.
 */
Cypd_Status_t Cypd_ReadDeviceMode(uint8 devIdx, uint16 *pMode);

/**
 * @brief Read and decode the port status into a Cypd_PortStatus_t struct.
 */
Cypd_Status_t Cypd_ReadPortStatus(uint8 devIdx, Cypd_PortStatus_t *pStatus);

/**
 * @brief Read the interrupt register and return the raw interrupt flags.
 */
Cypd_Status_t Cypd_ReadIntrReg(uint8 devIdx, uint8 *pIntr);

/**
 * @brief Clear the interrupt register (write-1-to-clear the given bits).
 */
Cypd_Status_t Cypd_ClearIntr(uint8 devIdx, uint8 mask);

/**
 * @brief Read and clear the PORT_EVENT register in one transaction.
 *        The event register self-clears on read per HPI spec.
 */
Cypd_Status_t Cypd_ReadPortEvent(uint8 devIdx, uint32 *pEvent);

/**
 * @brief Check if INT_L is asserted (active low) for the given device.
 */
boolean Cypd_IsIntAsserted(uint8 devIdx);

#endif /* CYPD6129_DRV_H */
