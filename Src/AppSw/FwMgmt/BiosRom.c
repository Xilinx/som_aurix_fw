/**
 * @file    BiosRom.c
 * @brief   BIOS ROM SPI bus gating — release to Ryzen
 *
 * QSPI0 pin assignments (from GP_AURIX_Subsystem_PinDefn.xlsx):
 *   P22.7  — QSPI0 CLK
 *   P22.9  — QSPI0 MISO (MRST)
 *   P22.10 — QSPI0 MOSI (MTSR)
 *   P22.11 — QSPI0 CS
 *   P22.8  — APU_ROM_SPI_SEL (mux select, GPIO output)
 *
 * Strategy:
 *   1. Do NOT initialise QSPI0 as a peripheral — leave it unclaimed.
 *   2. Explicitly set all four QSPI0 pins to input (high-impedance)
 *      so the AURIX does not drive the bus.
 *   3. Set APU_ROM_SPI_SEL to the state that connects the Ryzen
 *      to the BIOS ROM SPI flash.
 *
 * APU_ROM_SPI_SEL polarity:
 *   LOW  = Ryzen owns the bus (default / release state)
 *   HIGH = AURIX owns the bus (for future integrity check)
 *   Verify against schematic — invert if your mux is active-high-selects-APU.
 */

#include "BiosRom.h"
#include "Platform_PinCfg.h"
#include "AppPin.h"
#include "IfxPort.h"
#include "Uart_Debug.h"

/* QSPI0 pin port/pin indices — P22.x */
#define BIOS_SPI_PORT       &MODULE_P22
#define BIOS_SPI_CLK_PIN    7u
#define BIOS_SPI_MISO_PIN   9u
#define BIOS_SPI_MOSI_PIN   10u
#define BIOS_SPI_CS_PIN     11u

/* APU_ROM_SPI_SEL polarity:
 * Set to 0 to release bus to Ryzen, 1 for AURIX ownership.
 * Change this if your schematic uses opposite polarity. */
#define BIOS_SEL_RYZEN      0u
#define BIOS_SEL_AURIX      1u

static boolean s_released = FALSE;

/* ================================================================== */
/*  Private helpers                                                   */
/* ================================================================== */

/**
 * Set a port pin to high-impedance input (tri-state).
 */
static void prv_TriStatePin(Ifx_P *port, uint8 pin)
{
    IfxPort_setPinModeInput(port, pin, IfxPort_InputMode_noPullDevice);
}
/* ================================================================== */
/*  Public API                                                        */
/* ================================================================== */

void BiosRom_Init(void)
{
    /* Step 1: Tri-state all QSPI0 pins so the AURIX doesn't
     * drive any signal on the BIOS SPI bus. */
    prv_TriStatePin(BIOS_SPI_PORT, BIOS_SPI_CLK_PIN);
    prv_TriStatePin(BIOS_SPI_PORT, BIOS_SPI_MISO_PIN);
    prv_TriStatePin(BIOS_SPI_PORT, BIOS_SPI_MOSI_PIN);
    prv_TriStatePin(BIOS_SPI_PORT, BIOS_SPI_CS_PIN);

    /* Step 2: Set APU_ROM_SPI_SEL to release bus to Ryzen */
    if (BIOS_SEL_RYZEN == 0u)
    {
        IfxPort_setPinLow(AppPin_GetPort(PIN_APU_ROM_SPI_SEL.portIdx),
                          PIN_APU_ROM_SPI_SEL.pinIdx);
    }
    else
    {
        IfxPort_setPinHigh(AppPin_GetPort(PIN_APU_ROM_SPI_SEL.portIdx),
                           PIN_APU_ROM_SPI_SEL.pinIdx);
    }

    /* Ensure the SEL pin is configured as output */
    IfxPort_setPinModeOutput(AppPin_GetPort(PIN_APU_ROM_SPI_SEL.portIdx),
                             PIN_APU_ROM_SPI_SEL.pinIdx,
                             IfxPort_OutputMode_pushPull,
                             IfxPort_OutputIdx_general);

    s_released = TRUE;

    Debug_Print("[BIOS_ROM] Init: QSPI0 tri-stated, SPI_SEL -> Ryzen\r\n");
}

void BiosRom_ReleaseBus(void)
{
    if (s_released) return;

    prv_TriStatePin(BIOS_SPI_PORT, BIOS_SPI_CLK_PIN);
    prv_TriStatePin(BIOS_SPI_PORT, BIOS_SPI_MISO_PIN);
    prv_TriStatePin(BIOS_SPI_PORT, BIOS_SPI_MOSI_PIN);
    prv_TriStatePin(BIOS_SPI_PORT, BIOS_SPI_CS_PIN);

    if (BIOS_SEL_RYZEN == 0u)
        IfxPort_setPinLow(AppPin_GetPort(PIN_APU_ROM_SPI_SEL.portIdx),
                          PIN_APU_ROM_SPI_SEL.pinIdx);
    else
        IfxPort_setPinHigh(AppPin_GetPort(PIN_APU_ROM_SPI_SEL.portIdx),
                           PIN_APU_ROM_SPI_SEL.pinIdx);

    s_released = TRUE;
    Debug_Print("[BIOS_ROM] Bus released to Ryzen\r\n");
}

boolean BiosRom_IsReleased(void)
{
    return s_released;
}