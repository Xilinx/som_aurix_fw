/**
 * @file    UsbPd_Manager.h
 * @brief   Dual-port USB PD management interface.
 */

#ifndef USBPD_MANAGER_H
#define USBPD_MANAGER_H

#include "Ifx_Types.h"

/** Per-port connection state tracked by the manager. */
typedef enum
{
    USBPD_PORT_DETACHED     = 0,
    USBPD_PORT_ATTACHED,
    USBPD_PORT_CONTRACT,        /* PD contract negotiated */
} UsbPd_PortState_t;

/**
 * @brief Initialise USB PD manager.
 *        Hard-resets both CYPD6129 devices and verifies HPI link.
 *        Call after I2cMaster_Init() and Port_Init().
 */
void UsbPdManager_Init(void);

/**
 * @brief Run one USB PD manager iteration. Call from main loop.
 *        Polls INT_L for each device; on assertion reads and dispatches events.
 */
void UsbPdManager_Run(void);

/**
 * @brief Return the current connection state of a port (0 or 1).
 */
UsbPd_PortState_t UsbPdManager_GetPortState(uint8 portIdx);

#endif /* USBPD_MANAGER_H */
