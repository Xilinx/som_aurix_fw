/**
 * @file    Cypd6129_Drv.h
 * @brief   CYPD6129 HPIv2 driver: low-level register access, interrupt
 *          service and bring-up for the PD controller.
 *
 * The CYPD6129 is an I2C slave that exposes its state through the HPIv2
 * register map (see UsbPd_HpiRegs.h). This driver provides four layers:
 *
 *   1. Raw register access      : Cypd_HpiRead  / Cypd_HpiWrite
 *   2. Command-with-response     : Cypd_HpiWrite waits for and consumes the
 *                                  Success/error response every write produces
 *   3. Interrupt service         : Cypd_ServiceInterrupts drains INTR_REG,
 *                                  decodes every queued event, acknowledges it
 *   4. Bring-up                  : Cypd_HardReset (pin + boot) and
 *                                  Cypd_HpiInit (reset-complete, EVENT_MASK,
 *                                  EC-init-complete)
 *
 * Reference: Infineon EZ-PD CCG Host Processor Interface specification,
 * document 001-97863 Rev *V (2025-05-09).
 */

#ifndef CYPD6129_DRV_H
#define CYPD6129_DRV_H

#include "UsbPd_Cfg.h"
#include "UsbPd_HpiRegs.h"
#include "Ifx_Types.h"

/* ---- Firmware image decode ---------------------------------------------- */
typedef enum
{
    CYPD_IMG_BOOTLOADER = HPI_IMG_BOOTLOADER,
    CYPD_IMG_FW1        = HPI_IMG_FW1,
    CYPD_IMG_FW2        = HPI_IMG_FW2,
    CYPD_IMG_INVALID    = HPI_IMG_INVALID,
} Cypd_FwImage_t;

/* ---- Return status ------------------------------------------------------- */
typedef enum
{
    CYPD_OK             = 0,
    CYPD_ERR_I2C,           /* I2C transfer failed                           */
    CYPD_ERR_TIMEOUT,       /* expected response never arrived               */
    CYPD_ERR_BOOT_MODE,     /* device stuck in bootloader                    */
    CYPD_ERR_BAD_IMAGE,     /* DEVICE_MODE[1:0] = 3 (invalid image)          */
    CYPD_ERR_RESPONSE,      /* command returned a non-Success response code  */
    CYPD_ERR_INVALID,       /* bad argument                                  */
} Cypd_Status_t;

/* ---- One decoded message from a RESPONSE / PD_RESPONSE register ---------- */
#define CYPD_EVENT_DATA_MAX     16u     /* max response payload we capture   */
#define CYPD_PORT_DEVICE        0xFFu   /* ev.port value for device messages */

typedef struct
{
    uint8   code;       /* message code, b6:b0 only (b7 stripped)            */
    boolean isEvent;    /* TRUE = async event, FALSE = command response      */
    uint8   port;       /* 0 or 1 for a port message, CYPD_PORT_DEVICE else  */
    uint8   dataLen;    /* number of valid bytes in data[]                   */
    uint8   data[CYPD_EVENT_DATA_MAX];
} Cypd_Event_t;

/* ---- Decoded port status (lightweight, used for contract logging) -------- */
typedef struct
{
    boolean attached;
    boolean cc2Polarity;        /* TRUE = CC2 is the orientation reference   */
    boolean isDfp;
    boolean isSource;
    uint16  vbusVoltage_100mV;
} Cypd_PortStatus_t;

/* ======================================================================== */
/*  Layer 1 + 2 : register access                                           */
/* ======================================================================== */

/**
 * @brief Raw HPI register read (no handshake). Reads are side-effect free.
 * @param devIdx Index into CYPD_DEVICES[].
 * @param reg16  16-bit HPI register address.
 * @param pBuf   Destination buffer.
 * @param len    Number of bytes to read.
 */
Cypd_Status_t Cypd_HpiRead(uint8 devIdx, uint16 reg16, uint8 *pBuf, uint16 len);

/**
 * @brief HPI register write that waits for and consumes the response.
 *
 * Per the HPI spec, every register write (other than flash/data memory)
 * produces a Success (0x02) or error response and asserts INTR#. This helper
 * performs the write, waits for that response in the matching RESPONSE /
 * PD_RESPONSE register, checks it is Success, and clears the interrupt. That
 * keeps the response queue and INTR# line clean for the next transaction.
 *
 * @param devIdx Index into CYPD_DEVICES[].
 * @param reg16  16-bit HPI register address (selects device vs port queue).
 * @param pBuf   Source bytes.
 * @param len    Number of bytes to write.
 */
Cypd_Status_t Cypd_HpiWrite(uint8 devIdx, uint16 reg16, const uint8 *pBuf,
                            uint16 len);

/* ======================================================================== */
/*  Layer 3 : interrupt service                                             */
/* ======================================================================== */

/**
 * @brief Drain all pending HPI messages for one controller.
 *
 * Reads INTR_REG, and for each asserted source (device, port 0, port 1)
 * reads the matching RESPONSE register, decodes the message into pEvents[],
 * then acknowledges that source by writing back to INTR_REG. It loops until
 * INTR_REG reads zero or pEvents[] is full, so a burst of queued events is
 * fully consumed in one call.
 *
 * @param devIdx    Index into CYPD_DEVICES[].
 * @param pEvents   Caller buffer for decoded messages.
 * @param maxEvents Capacity of pEvents[].
 * @param pCount    Output: number of messages written to pEvents[].
 */
Cypd_Status_t Cypd_ServiceInterrupts(uint8 devIdx, Cypd_Event_t *pEvents,
                                     uint8 maxEvents, uint8 *pCount);

/* ======================================================================== */
/*  Layer 4 : bring-up                                                      */
/* ======================================================================== */

/**
 * @brief Hard-reset a controller by toggling RESET_L, wait for it to boot
 *        and confirm it is running a valid firmware image.
 */
Cypd_Status_t Cypd_HardReset(uint8 devIdx);

/**
 * @brief Complete the HPIv2 bring-up handshake after a reset:
 *        consume the Reset-Complete event, read DEVICE_MODE, program
 *        EVENT_MASK and send EC-Initialization-Complete.
 *
 * EVENT_MASK must be written before EC-Init-Complete, and the controller
 * only waits ~100 ms for EC-Init-Complete after boot, so call this promptly
 * after Cypd_HardReset succeeds.
 */
Cypd_Status_t Cypd_HpiInit(uint8 devIdx);

/* ======================================================================== */
/*  Small helpers                                                           */
/* ======================================================================== */

Cypd_Status_t  Cypd_ReadDeviceMode(uint8 devIdx, uint16 *pMode);
Cypd_FwImage_t Cypd_DecodeImage(uint16 mode);
const char    *Cypd_ImageName(Cypd_FwImage_t img);
const char    *Cypd_EventName(uint8 code);   /* human name for a message code */

/** Read and decode the port status (Type-C + roles + VBUS). */
Cypd_Status_t Cypd_ReadPortStatus(uint8 devIdx, Cypd_PortStatus_t *pStatus);

/** TRUE if INT_L is asserted (active low) for this controller. */
boolean Cypd_IsIntAsserted(uint8 devIdx);

/** Reinitialise the I2C bus to recover from a stuck-bus condition. */
void Cypd_RecoverBus(uint8 devIdx);

#endif /* CYPD6129_DRV_H */
