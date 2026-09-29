/**
 * @file    UsbPd_ApuProxy.c
 * @brief   Packs HPI register fields into AMD 40-bit APU format
 */

#include "UsbPd_ApuProxy.h"
#include "Uart_Debug.h"
#include "UsbPd_Hpi.h"
#include <string.h>

/* ================================================================== */
/*  State — two ports                                                 */
/* ================================================================== */

static UsbPd_ApuPort_t s_port[2];

/* ================================================================== */
/*  Mode encoding (bits 11:8)                                         */
/*  ALT_MODE_STATUS: bit6=USB4, bit1=DP, bit0=TBT3                   */
/*  If none set → USB3.                                               */
/* ================================================================== */

#define AMD_MODE_USB3       0x00u
#define AMD_MODE_DP         0x01u   /* DP alt mode (2 or 4 lane) */
#define AMD_MODE_TBT3       0x02u
#define AMD_MODE_USB4       0x04u
#define AMD_MODE_DP_TBT3    0x03u   /* DP + TBT3 */
#define AMD_MODE_DP_USB4    0x05u   /* DP + USB4 */

static uint8 prv_EncodeMode(uint8 altModeStatus, uint8 dpLaneMode)
{
    uint8 mode = 0u;

    if (altModeStatus & HPI_ALT_USB4_ACTIVE)
        mode |= AMD_MODE_USB4;
    if (altModeStatus & HPI_ALT_TBT3_ACTIVE)
        mode |= AMD_MODE_TBT3;
    if (altModeStatus & HPI_ALT_DP_ACTIVE)
        mode |= AMD_MODE_DP;

    /* If none → USB3 */
    (void)dpLaneMode;  /* Could use for DP 2-lane vs 4-lane disambiguation */
    return mode;
}

/* ================================================================== */
/*  Public API                                                        */
/* ================================================================== */

void UsbPd_ApuProxy_Init(void)
{
    memset(s_port, 0, sizeof(s_port));
    Debug_Print("[USBPD] APU proxy init\r\n");
}

void UsbPd_ApuProxy_Pack(uint8 portIdx,
                         const UsbPd_HpiState_t *pHpiState,
                         const UsbPd_CustomFields_t *pCustom)
{
    uint64 status = 0u;
    UsbPd_CustomFields_t cust;

    if (portIdx > 1u) return;

    /* Use provided custom fields or the stored ones */
    if (pCustom != NULL_PTR)
    {
        cust = *pCustom;
        s_port[portIdx].custom = cust;
    }
    else
    {
        cust = s_port[portIdx].custom;
    }

    /* Bit 7:0 — Index */
    status |= (uint64)portIdx;

    /* Bit 11:8 — Mode */
    status |= ((uint64)prv_EncodeMode(pHpiState->altModeStatus,
                                       cust.dpLaneMode)) << 8u;

    /* Bit 12 — Orientation (CC polarity) */
    if (pHpiState->typeCStatus & HPI_TC_CC_POLARITY)
        status |= (1ULL << 12u);

    /* Bit 14 — Data_Reset (custom) */
    if (cust.dataReset)
        status |= (1ULL << 14u);

    /* Bit 15 — Port_Role */
    if (pHpiState->pdStatus & HPI_PD_PORT_ROLE)
        status |= (1ULL << 15u);

    /* Bit 16 — USB4 */
    if (pHpiState->altModeStatus & HPI_ALT_USB4_ACTIVE)
        status |= (1ULL << 16u);

    /* Bit 17 — TBT3 */
    if (pHpiState->altModeStatus & HPI_ALT_TBT3_ACTIVE)
        status |= (1ULL << 17u);

    /* Bit 18 — Cable_CLx (custom) */
    if (cust.cableClx)
        status |= (1ULL << 18u);

    /* Bit 19 — Retimed_Active_Cable: ACT_CBL_VDO_2 bit 9 */
    if (pHpiState->actCblVdo2 & (1u << 9u))
        status |= (1ULL << 19u);

    /* Bit 20 — Bidir_Retimer (custom) */
    if (cust.bidirRetimer)
        status |= (1ULL << 20u);

    /* Bit 21 — Cable_Gen3: CABLE_VDO bits[2:0] if EMCA_PRESENT */
    if (pHpiState->pdStatus & HPI_PD_EMCA_PRESENT)
    {
        uint8 cableSpeed = (uint8)(pHpiState->currentCableVdo & 0x07u);
        if (cableSpeed >= 2u)  /* Gen3 = speed code >= 2 */
            status |= (1ULL << 21u);
    }

    /* Bit 23 — Active_Cable: PD_STATUS bit 22 */
    if (pHpiState->pdStatus & HPI_PD_CABLE_TYPE)
        status |= (1ULL << 23u);

    /* Bit 31:24 — Cable_ver (custom) */
    status |= ((uint64)cust.cableVersion) << 24u;

    /* Bit 32 — TypeC connected */
    if (pHpiState->typeCStatus & HPI_TC_CONNECTED)
        status |= (1ULL << 32u);

    /* Bit 38:37 — DP_UHBR_Cap (custom, 2 bits) */
    status |= ((uint64)(cust.dpUhbrCap & 0x03u)) << 37u;

    /* Bit 39 — DP_UHBR13_5 (custom) */
    if (cust.dpUhbr135)
        status |= (1ULL << 39u);

    /* Store packed status */
    s_port[portIdx].amdStatus = status;

    /* Unpack into byte register map for I2C slave */
    s_port[portIdx].regMap[0] = (uint8)(status);
    s_port[portIdx].regMap[1] = (uint8)(status >> 8u);
    s_port[portIdx].regMap[2] = (uint8)(status >> 16u);
    s_port[portIdx].regMap[3] = (uint8)(status >> 24u);
    s_port[portIdx].regMap[4] = (uint8)(status >> 32u);
    s_port[portIdx].regMap[5] = 0u;  /* Reserved */
    s_port[portIdx].regMap[6] = 0u;
    s_port[portIdx].regMap[7] = 0u;

    s_port[portIdx].valid = TRUE;
}

const uint8 *UsbPd_ApuProxy_GetRegMap(uint8 portIdx)
{
    if (portIdx > 1u || !s_port[portIdx].valid)
        return NULL_PTR;
    return s_port[portIdx].regMap;
}

void UsbPd_ApuProxy_SetCustom(uint8 portIdx,
                              const UsbPd_CustomFields_t *pCustom)
{
    if (portIdx > 1u || pCustom == NULL_PTR) return;
    s_port[portIdx].custom = *pCustom;
}

void UsbPd_ApuProxy_DumpPort(uint8 portIdx)
{
    if (portIdx > 1u) return;

    uint64 s = s_port[portIdx].amdStatus;
    Debug_Printf("[USBPD] APU proxy port %u: 0x%02X%08X\r\n",
                 (unsigned)portIdx,
                 (unsigned)(uint32)(s >> 32u),
                 (unsigned)(uint32)(s));
    Debug_Printf("[USBPD]   TypeC=%u Role=%u USB4=%u TBT3=%u DP=%u Orient=%u\r\n",
                 (unsigned)((s >> 32u) & 1u),
                 (unsigned)((s >> 15u) & 1u),
                 (unsigned)((s >> 16u) & 1u),
                 (unsigned)((s >> 17u) & 1u),
                 (unsigned)((s >> 8u) & 0x01u),
                 (unsigned)((s >> 12u) & 1u));
}