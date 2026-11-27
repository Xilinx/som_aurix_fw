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

/* Port-level register offsets — add to the port base (0x1000 or 0x2000).
 * Checked against HPI spec Table (section 4.3.1) addresses for port 0:
 *   PD_STATUS 0x1008, TYPE_C_STATUS 0x100C, CURRENT_CABLE_VDO 0x1018,
 *   ALT_MODE_STATUS 0x102B, ACT_CBL_VDO_2 0x1050. */
#define HPI_PORT_PD_STATUS          0x0008u  /* 4 bytes */
#define HPI_PORT_TYPE_C_STATUS      0x000Cu  /* 1 byte  */
#define HPI_PORT_CURRENT_CABLE_VDO  0x0018u  /* 4 bytes (was wrongly 0x0048) */
#define HPI_PORT_ALT_MODE_STATUS    0x002Bu  /* 1 byte  */
#define HPI_PORT_ACT_CBL_VDO_2      0x0050u  /* 4 bytes */

/* NOTE: HPI event codes (HPI_EVT_*) live in UsbPd_HpiRegs.h as one-byte
 * message codes (0x84 connect, 0x86 contract, ...). The old bit-mask macros
 * that used to be here described a register that does not exist in HPIv2 and
 * reused the same names with different values, so they were removed. */

/* TYPE_C_STATUS (0x100C, 1 byte) bit positions — HPI spec Table 75. */
#define HPI_TC_CONNECTED        (1u << 0)   /* b0: partner connected         */
#define HPI_TC_CC_POLARITY      (1u << 1)   /* b1: 0 = CC1, 1 = CC2          */
#define HPI_TC_ATTACHED_DEV     (0x07u << 2)/* b4:b2: attached device type   */
#define HPI_TC_CURRENT_LEVEL    (0x03u << 6)

/* PD_STATUS (0x1008, 4 bytes) bit positions — HPI spec Table 74.
 * Fixed here: b10 is ContractState (was read as bit 0), b11 is EMCA Present
 * (was read as bit 24), and the current roles are b6 (data) and b8 (power).
 * Per the spec the current data/power roles are valid only while
 * ContractState (b10) is set. */
#define HPI_PD_CONTRACT_EXISTS  (1u << 10)  /* b10: PD contract established   */
#define HPI_PD_DATA_ROLE        (1u << 6)   /* b6: 1 = DFP, 0 = UFP          */
#define HPI_PD_PORT_ROLE        (1u << 8)   /* b8: 1 = source, 0 = sink      */
#define HPI_PD_EMCA_PRESENT     (1u << 11)  /* b11: EMCA (e-marked cable)    */
#define HPI_PD_CABLE_TYPE       (1u << 22)  /* b22: 1 = active cable         */

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
 * @brief  Read the device-level interrupt register (INTR_REG, 0x0006).
 *
 * @param  i2cAddr   7-bit I2C address
 * @param  pIntr     Output: interrupt status (b0 device, b1 port0, b2 port1)
 * @return 0 on success
 */
uint8 UsbPd_Hpi_ReadDevIntr(uint8 i2cAddr, uint8 *pIntr);

#endif /* USBPD_HPI_H */