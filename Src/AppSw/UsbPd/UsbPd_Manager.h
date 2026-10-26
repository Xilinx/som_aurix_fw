/**
 * @file    UsbPd_Manager.h
 * @brief   Dual-port USB PD management interface.
 */

#ifndef USBPD_MANAGER_H
#define USBPD_MANAGER_H

#include "Ifx_Types.h"
#include "UsbPd_Hpi.h"

#define USBPD_RUNTIME_FAIL_LIMIT   10u
#define USBPD_SPURIOUS_INT_LIMIT   100u

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

/* ---- Port status word (UsbPdManager_GetPortStatusWord) ---------------- */
#define USBPD_STAT_STATE_MASK       0x03u        /* UsbPd_PortState_t       */
#define USBPD_STAT_DATA_DFP         (1u << 2)    /* 1 = DFP, 0 = UFP        */
#define USBPD_STAT_PWR_SOURCE       (1u << 3)    /* 1 = source, 0 = sink    */
#define USBPD_STAT_ALT_DP           (1u << 4)
#define USBPD_STAT_ALT_TBT3         (1u << 5)
#define USBPD_STAT_ALT_USB4         (1u << 6)
#define USBPD_STAT_DEV_PRESENT      (1u << 7)    /* CYPD alive and polled   */

/* ---- Fault flags: one bit per CYPD device index (bit n = device n) ------ */
#define USBPD_FAULT_DEV_ABSENT      (1u << 0)    /* bits 1:0 init/poll lost */
#define USBPD_FAULT_RUNTIME_FAIL    (1u << 2)    /* bits 3:2 HPI I2C errors */
#define USBPD_FAULT_SPURIOUS_INT    (1u << 4)    /* bits 5:4 INT_L spurious */

/**
 * @brief Packed per-port status: connection state, roles, alt modes and
 *        device presence.  Role/alt-mode bits are 0 while detached.
 */
uint32 UsbPdManager_GetPortStatusWord(uint8 portIdx);

/**
 * @brief Packed fault summary across both CYPD devices (see USBPD_FAULT_*).
 */
uint32 UsbPdManager_GetFaultFlags(void);

#endif /* USBPD_MANAGER_H */
