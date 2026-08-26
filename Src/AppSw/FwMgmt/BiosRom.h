/**
 * @file    BiosRom.h
 * @brief   BIOS ROM SPI bus access control
 *
 * The Ryzen APU's BIOS ROM is on QSPI0 (P22.7/9/10/11).
 * A mux select (APU_ROM_SPI_SEL, P22.8) gates whether the AURIX
 * or the Ryzen owns the SPI bus.
 *
 * v0.2 first implementation: ensure the AURIX does NOT gate
 * Ryzen BIOS ROM access.  QSPI0 pins are held in high-impedance
 * and APU_ROM_SPI_SEL is set to pass the bus to the Ryzen.
 *
 * Future: AURIX can temporarily take ownership to read/verify
 * the BIOS image (integrity check), then release.
 */

#ifndef BIOSROM_H
#define BIOSROM_H

#include "Ifx_Types.h"

/**
 * @brief  Initialise BIOS ROM bus control.
 *
 * Sets QSPI0 pins to high-impedance input and asserts
 * APU_ROM_SPI_SEL to give the Ryzen unimpeded access.
 *
 * Must be called early in init, before PowerManager starts
 * the Ryzen boot sequence.
 */
void BiosRom_Init(void);

/**
 * @brief  Release the BIOS ROM SPI bus to the Ryzen.
 *
 * Tri-states all QSPI0 pins and sets APU_ROM_SPI_SEL
 * to the Ryzen-owns-bus state.  Called after any AURIX
 * access to the ROM (future integrity check).
 */
void BiosRom_ReleaseBus(void);

/**
 * @brief  Check whether the bus is currently released to the Ryzen.
 */
boolean BiosRom_IsReleased(void);

#endif /* BIOSROM_H */