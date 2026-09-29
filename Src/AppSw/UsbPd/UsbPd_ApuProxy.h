/**
 * @file    UsbPd_ApuProxy.h
 * @brief   APU I2C Slave Proxy — AMD USB-C sideband register format
 *
 * Implements PMC-USBC-002: Packs PD controller HPI register fields
 * into the AMD 40-bit APU register format that the Ryzen reads
 * via I2C slave at 0x54 (port 0) and 0x58 (port 1).
 *
 * AMD APU Register Bit Map (per-port, 40 bits = 5 bytes):
 *   Bit 39     DP_UHBR13_5       (custom — from retimer/PD FW)
 *   Bit 38:37  DP_UHBR_Cap       (custom)
 *   Bit 36:33  Reserved
 *   Bit 32     TypeC              TYPE_C_STATUS[0]
 *   Bit 31:24  Cable_ver          (custom — from cable VDO)
 *   Bit 23     Active_Cable       PD_STATUS[22]
 *   Bit 22     Reserved
 *   Bit 21     Cable_Gen3         CABLE_VDO[2:0] if EMCA_PRESENT
 *   Bit 20     Bidir_Retimer      (custom)
 *   Bit 19     Retimed_Active     ACT_CBL_VDO_2[9]
 *   Bit 18     Cable_CLx          (custom)
 *   Bit 17     TBT3               ALT_MODE_STATUS[0]
 *   Bit 16     USB4               ALT_MODE_STATUS[6]
 *   Bit 15     Port_Role          PD_STATUS[6]
 *   Bit 14     Data_Reset         (custom)
 *   Bit 13     Reserved
 *   Bit 12     Orientation        TYPE_C_STATUS[1]
 *   Bit 11:8   Mode               ALT_MODE_STATUS[6,1,0] decoded
 *   Bit 7:0    Index              Port index (0 or 1)
 */

#ifndef USBPD_APUPROXY_H
#define USBPD_APUPROXY_H

#include "Ifx_Types.h"
#include "UsbPd_Hpi.h"

/* ================================================================== */
/*  APU slave addresses                                               */
/* ================================================================== */
#define APU_SLV_USBC0_ADDR      0x54u
#define APU_SLV_USBC1_ADDR      0x58u

/* ================================================================== */
/*  APU proxy register offsets (what the APU reads via I2C)           */
/* ================================================================== */
#define APU_PROXY_REG_STATUS_LO     0x00u   /* Bits 7:0 of AMD format */
#define APU_PROXY_REG_STATUS_HI     0x01u   /* Bits 15:8 */
#define APU_PROXY_REG_STATUS_2      0x02u   /* Bits 23:16 */
#define APU_PROXY_REG_STATUS_3      0x03u   /* Bits 31:24 */
#define APU_PROXY_REG_STATUS_4      0x04u   /* Bits 39:32 */
#define APU_PROXY_REG_COUNT         0x08u   /* 8 registers total */

/* ================================================================== */
/*  Custom field sources (populated by UsbPd_Manager or retimer drv)  */
/* ================================================================== */

typedef struct
{
    boolean dpUhbr135;          /**< DP UHBR13.5 capable */
    uint8   dpUhbrCap;          /**< DP UHBR capability (2 bits) */
    uint8   cableVersion;       /**< Cable version (8 bits) */
    boolean bidirRetimer;       /**< Bidirectional retimer present */
    boolean cableClx;           /**< Cable CLx support */
    boolean dataReset;          /**< Data reset in progress */
    uint8   dpLaneMode;         /**< 0=none, 2=2-lane, 4=4-lane DP */
} UsbPd_CustomFields_t;

/* ================================================================== */
/*  Per-port proxy state                                              */
/* ================================================================== */

typedef struct
{
    uint8  regMap[APU_PROXY_REG_COUNT];  /**< Shadow registers the APU reads */
    uint64 amdStatus;                     /**< Packed 40-bit AMD format */
    UsbPd_CustomFields_t custom;          /**< Custom AMD fields */
    boolean valid;                        /**< Set TRUE after first successful pack */
} UsbPd_ApuPort_t;

/* ================================================================== */
/*  Public API                                                        */
/* ================================================================== */

/**
 * @brief  Initialise the APU proxy for both ports.
 */
void UsbPd_ApuProxy_Init(void);

/**
 * @brief  Pack HPI port state + custom fields into AMD format.
 *
 * Call after UsbPd_Hpi_ReadPortState completes for a port.
 *
 * @param  portIdx   0 or 1
 * @param  pHpiState Current HPI register state for this port
 * @param  pCustom   Custom AMD-specific fields (NULL for defaults)
 */
void UsbPd_ApuProxy_Pack(uint8 portIdx,
                         const UsbPd_HpiState_t *pHpiState,
                         const UsbPd_CustomFields_t *pCustom);

/**
 * @brief  Get the shadow register map for a port.
 *
 * The I2C slave ISR reads from this when the APU queries.
 *
 * @param  portIdx  0 or 1
 * @return Pointer to APU_PROXY_REG_COUNT bytes, or NULL if invalid
 */
const uint8 *UsbPd_ApuProxy_GetRegMap(uint8 portIdx);

/**
 * @brief  Set a custom field for a port.
 *
 * Used by the retimer driver or PD manager to inject fields
 * that aren't in the standard HPI registers.
 */
void UsbPd_ApuProxy_SetCustom(uint8 portIdx,
                              const UsbPd_CustomFields_t *pCustom);

/**
 * @brief  Print the packed AMD status for debug.
 */
void UsbPd_ApuProxy_DumpPort(uint8 portIdx);

#endif /* USBPD_APUPROXY_H */