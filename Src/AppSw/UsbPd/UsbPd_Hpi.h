/**
 * @file    UsbPd_Hpi.h
 * @brief   Infineon CCGx HPI register interface for PD controllers
 *
 * Expanded register set for v0.2: reads TYPE_C_STATUS, PD_STATUS,
 * ALT_MODE_STATUS, CURRENT_CABLE_VDO, ACT_CBL_VDO_2 via I2C0.
 *
 * Device-abstracted: port register base addresses are configured
 * per-device (CYPD6129 dual single-port vs CYPD6229 single dual-port).
 */

#ifndef USBPD_HPI_H
#define USBPD_HPI_H

#include "Ifx_Types.h"

/* ================================================================== */
/*  HPI register offsets (per-port, add to port base)                 */
/* ================================================================== */

/* Device-level registers (common, no port offset) */
#define HPI_DEV_MODE            0x0000u
#define HPI_INTR_REG            0x0006u
#define HPI_RESPONSE            0x007Eu
#define HPI_FW_VERSION          0x0020u

/* Port-level registers — offset from port base (0x1000 or 0x2000) */
#define HPI_PORT_TYPE_C_STATUS      0x000Cu
#define HPI_PORT_PD_STATUS          0x0008u
#define HPI_PORT_CURRENT_PDO        0x0010u
#define HPI_PORT_ALT_MODE_STATUS    0x002Bu
#define HPI_PORT_CURRENT_CABLE_VDO  0x0048u
#define HPI_PORT_ACT_CBL_VDO_2     0x0050u
#define HPI_PORT_EVENT_MASK         0x0024u
#define HPI_PORT_PD_CTRL            0x0026u
#define HPI_PORT_DP_HPD_CTRL        0x0060u  /* Vendor-specific HPD control */

/* Interrupt/event register bit positions */
#define HPI_EVT_TYPE_C_ATTACH   (1u << 0)
#define HPI_EVT_TYPE_C_DETACH   (1u << 1)
#define HPI_EVT_PD_CONTRACT     (1u << 2)
#define HPI_EVT_ALT_MODE        (1u << 4)
#define HPI_EVT_DP_HPD          (1u << 8)
#define HPI_EVT_VBUS_OV         (1u << 10)
#define HPI_EVT_VBUS_OC         (1u << 11)
#define HPI_EVT_ERROR           (1u << 15)

/* TYPE_C_STATUS bit positions */
#define HPI_TC_CONNECTED        (1u << 0)
#define HPI_TC_CC_POLARITY      (1u << 1)
#define HPI_TC_ATTACHED_DEV     (0x07u << 2)
#define HPI_TC_CURRENT_LEVEL    (0x03u << 6)

/* PD_STATUS bit positions */
#define HPI_PD_CONTRACT_EXISTS  (1u << 0)
#define HPI_PD_PORT_ROLE        (1u << 6)   /* 0=sink, 1=source */
#define HPI_PD_DATA_ROLE        (1u << 7)   /* 0=UFP, 1=DFP */
#define HPI_PD_CABLE_TYPE       (1u << 22)
#define HPI_PD_EMCA_PRESENT     (1u << 24)

/* ALT_MODE_STATUS bit positions */
#define HPI_ALT_TBT3_ACTIVE    (1u << 0)
#define HPI_ALT_DP_ACTIVE      (1u << 1)
#define HPI_ALT_USB4_ACTIVE    (1u << 6)

/* ================================================================== */
/*  Per-port cached state                                             */
/* ================================================================== */

typedef struct
{
    uint32 typeCStatus;         /**< Raw TYPE_C_STATUS register */
    uint32 pdStatus;            /**< Raw PD_STATUS register */
    uint8  altModeStatus;       /**< Raw ALT_MODE_STATUS (8-bit) */
    uint32 currentCableVdo;     /**< Raw CURRENT_CABLE_VDO register */
    uint32 actCblVdo2;          /**< Raw ACT_CBL_VDO_2 register */
    uint32 eventFlags;          /**< Pending event bitmask */
    boolean connected;          /**< Port partner connected */
    boolean contractValid;      /**< PD contract negotiated */
    uint32 lastUpdateMs;        /**< Timestamp of last successful read */
} UsbPd_HpiState_t;

/* ================================================================== */
/*  Public API                                                        */
/* ================================================================== */

/**
 * @brief  Read all status registers for a port from the PD controller.
 *
 * @param  i2cAddr   7-bit I2C address of the PD controller
 * @param  portBase  HPI port base (0x1000 for port 0, 0x2000 for port 1)
 * @param  pState    Output: populated port state structure
 * @return 0 on success, non-zero on I2C error
 */
uint8 UsbPd_Hpi_ReadPortState(uint8 i2cAddr, uint16 portBase,
                              UsbPd_HpiState_t *pState);

/**
 * @brief  Read and clear the interrupt/event register.
 *
 * @param  i2cAddr   7-bit I2C address
 * @param  portBase  HPI port base
 * @param  pEvents   Output: event flags that were pending
 * @return 0 on success
 */
uint8 UsbPd_Hpi_ReadClearEvents(uint8 i2cAddr, uint16 portBase,
                                uint32 *pEvents);

/**
 * @brief  Read the device-level interrupt register.
 *
 * The CYPD6129 uses a single ALERT# line for both ports.
 * This reads INTR_REG to determine which port(s) have events.
 *
 * @param  i2cAddr   7-bit I2C address
 * @param  pIntr     Output: interrupt status (bit0=port0, bit1=port1)
 * @return 0 on success
 */
uint8 UsbPd_Hpi_ReadDevIntr(uint8 i2cAddr, uint8 *pIntr);

/**
 * @brief  Write the HPD control register (for virtual HPD).
 *
 * @param  i2cAddr   7-bit I2C address
 * @param  portBase  HPI port base
 * @param  hpdCmd    HPD command value
 * @return 0 on success
 */
uint8 UsbPd_Hpi_WriteHpdCtrl(uint8 i2cAddr, uint16 portBase,
                              uint8 hpdCmd);

#endif /* USBPD_HPI_H */