/**
 * @file    Uart_Debug.h
 * @brief   Polling UART debug output via ASCLIN0 (P14.0 TX / P14.1 RX).
 *
 * Provides printf-style logging. TX is fully polling — no DMA, no interrupts.
 * Call Debug_Init() once after clock and port initialisation.
 */

#ifndef UART_DEBUG_H
#define UART_DEBUG_H

#include "Ifx_Types.h"

/** Baud rate for the debug UART. */
#define UART_DEBUG_BAUD     115200u

/**
 * @brief Initialise ASCLIN0 for 115200 8N1 on P14.0/P14.1.
 */
void Debug_Init(void);

/**
 * @brief Transmit a null-terminated string (blocking).
 */
void Debug_Print(const char *str);

/**
 * @brief printf-style formatted output (blocking). Limited to 256 chars/call.
 */
void Debug_Printf(const char *fmt, ...);

#endif /* UART_DEBUG_H */
