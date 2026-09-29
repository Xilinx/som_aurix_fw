/**
 * @file    AppPin.h
 * @brief   Toolchain-agnostic GPIO pin reference type.
 *
 * Replaces IfxPort_Pin / IfxPort_Pxx_y throughout the application layer.
 * Tasking iLLD defines IfxPort_Pxx_y as extern variables (not macros), which
 * cannot appear in aggregate initialisers (Tasking E306/E272/E333).
 *
 * AppPin_t stores only uint8 integers — always valid constant expressions.
 * AppPin_GetPort() maps portIdx to the Ifx_P* register pointer at runtime.
 */

#ifndef APPPIN_H
#define APPPIN_H

#include "Ifx_Types.h"
#include "IfxPort.h"

/**
 * @brief Board-level GPIO pin reference.
 *        portIdx: TC387 port number (e.g. 0 for P0, 33 for P33).
 *        pinIdx:  pin index within that port (0–15).
 */
typedef struct
{
    uint8 portIdx;
    uint8 pinIdx;
} AppPin_t;

/**
 * @brief Return the Ifx_P* register base for the given port index.
 *        Returns NULL_PTR for unmapped indices.
 */
Ifx_P* AppPin_GetPort(uint8 portIdx);

#endif /* APPPIN_H */
