/**
 * @file    UsbPd_Hpd.h
 * @brief   Virtual HPD for DisplayPort Alt Mode
 *
 * Implements PMC-USBC-003/004: Reads HPD events from PD controller
 * HPI interrupt registers, translates to GPIO-driven DP_HPD
 * assertions (level or IRQ pulse) toward the APU.
 *
 * HPD flow:
 *   1. PD controller asserts USBC_PD_ALERT# (P10.7, wired-OR)
 *   2. UsbPd_Manager polls ALERT and calls UsbPd_Hpd_ProcessEvent()
 *   3. HPD handler reads the HPI HPD status register
 *   4. Drives DP2_HPD (P13.0) or DP3_HPD (P13.3) accordingly:
 *      - HPD HIGH: level assert
 *      - HPD LOW: level deassert
 *      - HPD IRQ: 2ms low pulse (per DP spec, min 0.25ms, max 2ms)
 */

#ifndef USBPD_HPD_H
#define USBPD_HPD_H

#include "Ifx_Types.h"

/* ================================================================== */
/*  HPD event types                                                   */
/* ================================================================== */

typedef enum
{
    HPD_EVENT_NONE      = 0u,
    HPD_EVENT_HIGH      = 1u,   /* DP connected — assert HPD level */
    HPD_EVENT_LOW       = 2u,   /* DP disconnected — deassert HPD */
    HPD_EVENT_IRQ       = 3u,   /* DP IRQ — 2ms low pulse */
} UsbPd_HpdEvent_t;

/* ================================================================== */
/*  Per-port HPD state                                                */
/* ================================================================== */

typedef struct
{
    boolean           hpdLevel;        /**< Current HPD output level */
    boolean           irqPending;      /**< IRQ pulse in progress */
    uint32            irqStartMs;      /**< Timestamp of IRQ pulse start */
    uint32            lastEventMs;     /**< Timestamp of last HPD event */
    UsbPd_HpdEvent_t  lastEvent;       /**< Most recent event type */
} UsbPd_HpdState_t;

/* ================================================================== */
/*  Public API                                                        */
/* ================================================================== */

/**
 * @brief  Initialise HPD outputs for both ports.
 *
 * Sets DP2_HPD and DP3_HPD LOW (deasserted).
 */
void UsbPd_Hpd_Init(void);

/**
 * @brief  Process an HPD event for a specific port.
 *
 * Called by UsbPd_Manager when the PD controller reports an
 * HPD-related event via the ALERT# interrupt.
 *
 * @param  portIdx   0 (maps to DP2_HPD) or 1 (maps to DP3_HPD)
 * @param  event     HPD event type
 */
void UsbPd_Hpd_ProcessEvent(uint8 portIdx, UsbPd_HpdEvent_t event);

/**
 * @brief  Periodic service — handles IRQ pulse timing.
 *
 * Must be called from the main loop to complete IRQ pulses
 * (deassert HPD after the 2ms low period).
 */
void UsbPd_Hpd_Run(void);

/**
 * @brief  Force deassert all HPD outputs (e.g. on detach or shutdown).
 */
void UsbPd_Hpd_DeassertAll(void);

/**
 * @brief  Get the current HPD level for a port.
 */
boolean UsbPd_Hpd_GetLevel(uint8 portIdx);

#endif /* USBPD_HPD_H */