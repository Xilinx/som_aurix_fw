/**
 * @file    Uart_Xfer.h
 * @brief   Dedicated UART for firmware update data transfer
 *
 * Interrupt-driven UART with a receive ring buffer, used by the
 * FwUpdate module to receive firmware images.
 *
 * Pin selection:
 *   TARGET_EVAL_BOARD : ASCLIN1 (P15.0 TX / P15.1 RX)
 *                       Typically available on the eval board
 *                       header; connect a USB-UART adapter here.
 *   SoM target        : ASCLIN4 (dedicated AURIX ↔ Ryzen UART)
 *                       Pins defined by the SoM ARD.
 *
 * The debug UART (ASCLIN0 / Uart_Debug) is completely separate
 * and continues to function for diagnostic prints.
 */

#ifndef UART_XFER_H
#define UART_XFER_H

#include "Ifx_Types.h"
#include "IfxAsclin_Asc.h"

/* ------------------------------------------------------------------ */
/*  Configuration                                                     */
/* ------------------------------------------------------------------ */

/** RX ring buffer size — must be a power of 2 for efficient masking.
 *  4 KB gives room for multiple 256-byte PFlash pages in flight.    */
#ifndef UART_XFER_RX_BUF_SIZE
#define UART_XFER_RX_BUF_SIZE      4096u
#endif

/** Baud rate — 115200 for initial bring-up.  Increase to 921600
 *  once basic transfers are validated.                              */
#ifndef UART_XFER_BAUD
#define UART_XFER_BAUD              115200u
#endif

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

/**
 * @brief  Initialise the transfer UART peripheral and ring buffer.
 *
 * Configures the ASCLIN module, sets up the RX interrupt, and
 * enables reception.  TX is blocking (only used for ACK/NAK
 * responses, which are short).
 */
void UartXfer_Init(void);

/**
 * @brief  Get the number of bytes available in the RX buffer.
 * @return Number of unread bytes.
 */
uint32 UartXfer_Available(void);

/**
 * @brief  Read bytes from the RX ring buffer.
 *
 * Non-blocking.  Returns the number of bytes actually read,
 * which may be less than requested if the buffer doesn't hold
 * enough data.
 *
 * @param  pDest  Destination buffer.
 * @param  maxLen Maximum bytes to read.
 * @return Number of bytes actually copied to pDest.
 */
uint32 UartXfer_Read(uint8 *pDest, uint32 maxLen);

/**
 * @brief  Read exactly `len` bytes, blocking until available.
 *
 * Spins until enough data has been received.  Use with a
 * caller-side timeout to avoid hanging forever.
 *
 * @param  pDest  Destination buffer.
 * @param  len    Exact number of bytes to read.
 * @param  timeoutMs  Maximum wait time in ms (0 = infinite).
 * @return Number of bytes read (== len on success, < len on timeout).
 */
uint32 UartXfer_ReadBlocking(uint8 *pDest, uint32 len, uint32 timeoutMs);

/**
 * @brief  Transmit bytes (blocking).
 *
 * Used for sending ACK/NAK responses and status frames back to
 * the host.  Blocks until all bytes are transmitted.
 *
 * @param  pSrc  Source buffer.
 * @param  len   Number of bytes to send.
 */
void UartXfer_Write(const uint8 *pSrc, uint32 len);

/**
 * @brief  Flush (discard) all data in the RX ring buffer.
 */
void UartXfer_FlushRx(void);

/**
 * @brief  Override the UART handle (for CLI-triggered update via debug UART).
 * @param  asc  Pointer to an IfxAsclin_Asc handle, or NULL to restore default.
 */
void UartXfer_SetHandle(IfxAsclin_Asc *asc);

#endif /* UART_XFER_H */