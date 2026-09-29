/**
 * @file    AppPin.c
 * @brief   Port index to Ifx_P* lookup for AppPin_t GPIO references.
 *
 * Tasking iLLD uses zero-padded two-digit module names for single-digit ports:
 *   MODULE_P00 (not MODULE_P0), MODULE_P01 (not MODULE_P1), MODULE_P02 (not MODULE_P2).
 * Double-digit ports (P10, P11 ... P40) use their natural names unchanged.
 */

#include "AppPin.h"
#include "IfxPort.h"

Ifx_P* AppPin_GetPort(uint8 portIdx)
{
    switch (portIdx)
    {
        case 0u:  return &MODULE_P00;
        case 1u:  return &MODULE_P01;
        case 2u:  return &MODULE_P02;
        case 10u: return &MODULE_P10;
        case 11u: return &MODULE_P11;
        case 12u: return &MODULE_P12;
        case 13u: return &MODULE_P13;
        case 14u: return &MODULE_P14;
        case 15u: return &MODULE_P15;
        case 20u: return &MODULE_P20;
        case 21u: return &MODULE_P21;
        case 22u: return &MODULE_P22;
        case 23u: return &MODULE_P23;
        case 32u: return &MODULE_P32;
        case 33u: return &MODULE_P33;
        case 34u: return &MODULE_P34;
        case 40u: return &MODULE_P40;
        default:  return NULL_PTR;
    }
}
