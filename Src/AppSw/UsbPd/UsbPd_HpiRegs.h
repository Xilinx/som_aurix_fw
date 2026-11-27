/**
 * @file    UsbPd_HpiRegs.h
 * @brief   Single, authoritative HPIv2 register map for the CYPD6129.
 *
 * WHY THIS FILE EXISTS
 * --------------------
 * Before Phase 1 the register addresses and bit masks were spread across
 * two headers (Cypd6129_Drv.h and UsbPd_Hpi.h) and did not match the
 * CYPD6129 HPIv2 interface. For example the old code read a "PORT_EVENT"
 * register at 0x0030 which does not exist in HPIv2, and it had the INTR_REG
 * device/port bits swapped. That meant the EC could never see real PD events.
 *
 * This header is the ONE place where the HPIv2 register layout is defined,
 * taken directly from the Infineon "EZ-PD CCG Host Processor Interface"
 * specification (doc 001-97863 Rev *V, 2025-05-09). Every USB-PD source file
 * includes this header so there is only one source of truth.
 *
 * HOW HPIv2 WORKS, IN ONE PARAGRAPH
 * ---------------------------------
 * The PD controller (CYPD6129) is an I2C slave. The EC (this AURIX) talks to
 * it by reading and writing 16-bit register addresses. Addresses below
 * 0x1000 are "device" registers (shared by the whole chip). Addresses from
 * 0x1000 belong to USB-PD port 0, and 0x2000 to port 1. The CYPD6129 on this
 * board is a single-port part, so we only use the port-0 window (0x10xx).
 * When something happens (cable plugged, contract made, a command finished),
 * the chip pulls its INTR# pin low, puts a 1-byte code into a RESPONSE
 * register, and waits for the EC to read it and acknowledge by writing to
 * INTR_REG. That read-decode-acknowledge handshake is the heart of Phase 1.
 */

#ifndef USBPD_HPIREGS_H
#define USBPD_HPIREGS_H

#include "Ifx_Types.h"

/* ====================================================================== */
/*  Port address windows                                                  */
/* ---------------------------------------------------------------------- */
/*  Device registers live below 0x1000. Each USB-PD port has its own      */
/*  0x1000-wide window. Port 0 starts at 0x1000, port 1 at 0x2000.        */
/* ====================================================================== */
#define HPI_PORT0_BASE              0x1000u
#define HPI_PORT1_BASE              0x2000u

/* ====================================================================== */
/*  Device registers (no port offset)                                     */
/* ====================================================================== */
#define HPI_REG_DEVICE_MODE         0x0000u  /* R  : image + HPI version   */
#define HPI_REG_BOOT_MODE_REASON    0x0001u  /* R  : 1 byte, why in boot   */
#define HPI_REG_SILICON_ID          0x0002u  /* R  : 2-byte silicon ID     */
#define HPI_REG_INTR_REG            0x0006u  /* RW : interrupt status, W1C */
#define HPI_REG_JUMP_TO_BOOT        0x0007u  /* W  : jump to bootloader    */
#define HPI_REG_RESET               0x0008u  /* W  : device/I2C reset      */
#define HPI_REG_RESPONSE            0x007Eu  /* R  : DEVICE response/event */

/* ====================================================================== */
/*  Port registers (shown for port 0; add 0x1000 for port 1)              */
/* ====================================================================== */
#define HPI_REG_PD_CONTROL          0x1006u  /* W  : 1-byte PD command     */
#define HPI_REG_PD_STATUS           0x1008u  /* R  : 4-byte PD status      */
#define HPI_REG_TYPE_C_STATUS       0x100Cu  /* R  : 1-byte Type-C status  */
#define HPI_REG_BUS_VOLTAGE         0x100Du  /* R  : VBUS in 100 mV units  */
#define HPI_REG_CURRENT_PDO         0x1010u  /* R  : active PDO (4 bytes)  */
#define HPI_REG_CURRENT_RDO         0x1014u  /* R  : active RDO (4 bytes)  */
#define HPI_REG_EVENT_MASK          0x1024u  /* RW : 4-byte event enable   */
#define HPI_REG_ALT_MODE_STATUS     0x102Bu  /* R  : 1-byte alt-mode status */
#define HPI_REG_PORT_INTR_STATUS    0x1034u  /* R  : live port status bits */
#define HPI_REG_PORT_USER_REGS      0x1038u  /* RW : AMD handoff area       */
#define HPI_REG_PD_RESPONSE         0x1400u  /* R  : PORT response/event   */
#define HPI_REG_PD_RESPONSE_DATA    0x1404u  /* R  : response payload      */

/*
 * AMD APU handoff sub-fields inside PORT_USER_REGS (port 0 base 0x1038).
 * Bytes 0-4 carry the 5-byte Type-1 payload; byte 5 is the MUX-busy flag.
 * Byte 6 (0x103E) appears only in a draft table and is NOT agreed, so we do
 * not use it. Bytes 6-23 remain reserved.
 */
#define HPI_REG_APU_DATA            0x1038u  /* 5-byte Type-1 payload       */
#define HPI_REG_APU_MUX_BUSY        0x103Du  /* 1 = request pending         */

/* ---------------------------------------------------------------------- */
/*  Helper: turn a port-0 address into the address for a given port.      */
/*  port 0 -> unchanged, port 1 -> + 0x1000.                              */
/* ---------------------------------------------------------------------- */
#define HPI_PORT_REG(reg, port)     ((uint16)((reg) + ((uint16)(port) * 0x1000u)))

/* ====================================================================== */
/*  INTR_REG (0x0006) bit map                                             */
/* ---------------------------------------------------------------------- */
/*  The spec lists the bits in this order: DEV_INTR, PORT0_INTR,          */
/*  PORT1_INTR. The pre-Phase-1 driver had DEV and PORT0 swapped.         */
/*  Write a 1 to a bit to acknowledge (clear) that source.                */
/* ====================================================================== */
#define HPI_INTR_DEV                (1u << 0)   /* device RESPONSE pending  */
#define HPI_INTR_PORT0              (1u << 1)   /* port-0 PD_RESPONSE pend. */
#define HPI_INTR_PORT1              (1u << 2)   /* port-1 PD_RESPONSE pend. */

/* ====================================================================== */
/*  RESPONSE / PD_RESPONSE byte-0 format                                  */
/* ---------------------------------------------------------------------- */
/*  Byte[0]: b7 = message type, b6:b0 = message code.                     */
/*           b7 = 0 -> response to a command the EC issued                */
/*           b7 = 1 -> asynchronous event / notification                  */
/*  Byte[1]: number of data bytes that follow.                            */
/* ====================================================================== */
#define HPI_RESP_IS_EVENT           (1u << 7)   /* b7: 1 = event, 0 = resp  */
#define HPI_RESP_CODE_MASK          0x7Fu       /* b6:b0 message code       */

/* ---- Command responses (byte0 with b7 = 0) --------------------------- */
#define HPI_RESP_NO_RESPONSE        0x00u   /* queue empty / nothing to read */
#define HPI_RESP_SUCCESS            0x02u   /* command handled OK            */
#define HPI_RESP_FLASH_DATA_AVAIL   0x03u
#define HPI_RESP_INVALID_CMD        0x05u   /* bad/partial register write    */
#define HPI_RESP_INVALID_ARG        0x09u
#define HPI_RESP_NOT_SUPPORTED      0x0Au

/* ---- Asynchronous events (byte0 with b7 = 1, shown here WITHOUT b7) --- */
/*  Compare the masked code (byte0 & HPI_RESP_CODE_MASK) against these.    */
#define HPI_EVT_RESET_COMPLETE      0x80u   /* chip booted, HPI ready        */
#define HPI_EVT_QUEUE_OVERFLOW      0x81u   /* an event was dropped          */
#define HPI_EVT_OVER_CURRENT        0x82u
#define HPI_EVT_OVER_VOLTAGE        0x83u
#define HPI_EVT_TYPEC_CONNECT       0x84u   /* cable attached                */
#define HPI_EVT_TYPEC_DISCONNECT    0x85u   /* cable removed                 */
#define HPI_EVT_PD_CONTRACT         0x86u   /* PD contract negotiated        */
#define HPI_EVT_DP_MODE_ENTERED     0x93u
#define HPI_EVT_DP_STATUS_UPDATE    0x94u

/* ---- AMD project-specific embedded events (b7 = 1) ------------------- */
/*  Defined in the AMD embedded spec, delivered on the port queue. These   */
/*  are zero-length and are not gated by any EVENT_MASK bit.               */
#define HPI_EVT_AMD_APU_REQUEST     0xE0u   /* APU alt-mode request          */
#define HPI_EVT_AMD_HPD_LOW_HIGH    0xE1u   /* virtual HPD Low -> High       */
#define HPI_EVT_AMD_HPD_HIGH_LOW    0xE2u   /* virtual HPD High -> Low       */
#define HPI_EVT_AMD_HPD_IRQ         0xE3u   /* HPD High + IRQ                */
#define HPI_EVT_AMD_APU_TIMEOUT     0xC0u   /* APU handoff wait timed out    */

/* ====================================================================== */
/*  PD_CONTROL (0x1006) command opcodes                                   */
/* ---------------------------------------------------------------------- */
/*  EC_INIT_COMPLETE tells the chip the EC has finished its own setup      */
/*  (e.g. EVENT_MASK). The chip waits up to 100 ms for it after boot, so   */
/*  EVENT_MASK must be written BEFORE this command.                        */
/* ====================================================================== */
#define HPI_PDCMD_EC_INIT_COMPLETE  0x10u

/* ====================================================================== */
/*  EVENT_MASK value (spec Table 87, 4 bytes, port-0 address 0x1024)      */
/* ---------------------------------------------------------------------- */
/*  0x00000C3E sets these bits (1 = the EC is notified):                  */
/*    b1  Over-current detected          (event 0x82)                     */
/*    b2  Over/under-voltage detected    (event 0x83)                     */
/*    b3  Type-C port connect            (event 0x84)                     */
/*    b4  Type-C port disconnect         (event 0x85)                     */
/*    b5  PD contract negotiation done   (event 0x86)                     */
/*    b10 DP alternate-mode events       (events 0x93, 0x94, ...)         */
/*    b11 Error and timeout events       (hard/soft reset sent, timers)   */
/*  NOT enabled (0 in this value): b0 reserved, b6 PD control message,     */
/*  b7 VDM, b8 source capabilities, b9 sink capabilities.                  */
/*  AMD embedded events 0xE0-0xE3 and 0xC0 have no mask bit, so this mask  */
/*  does not control them.                                                 */
/* ====================================================================== */
#define HPI_EVENT_MASK_VALUE        0x00000C3Eu

/* ====================================================================== */
/*  DEVICE_MODE[1:0] : which firmware image is running                    */
/* ====================================================================== */
#define HPI_DEVICE_MODE_IMAGE_MASK  0x0003u
#define HPI_IMG_BOOTLOADER          0x00u
#define HPI_IMG_FW1                 0x01u
#define HPI_IMG_FW2                 0x02u
#define HPI_IMG_INVALID             0x03u

/* ====================================================================== */
/*  PD_STATUS (0x1008) bit positions                                      */
/* ---------------------------------------------------------------------- */
/*  Fixed in Phase 1: the current data role is bit 6 and the current power */
/*  role is bit 8 (the old header used bits 6 and 7, which was wrong).     */
/* ====================================================================== */
#define HPI_PD_DATA_ROLE_DFP        (1u << 6)   /* 1 = DFP, 0 = UFP          */
#define HPI_PD_POWER_ROLE_SOURCE    (1u << 8)   /* 1 = source, 0 = sink      */

/* ====================================================================== */
/*  TYPE_C_STATUS (0x100C) bit positions                                  */
/* ====================================================================== */
#define HPI_TC_ATTACHED             (1u << 0)
#define HPI_TC_CC_POLARITY_CC2      (1u << 1)   /* 1 = CC2 is the reference  */

#endif /* USBPD_HPIREGS_H */
