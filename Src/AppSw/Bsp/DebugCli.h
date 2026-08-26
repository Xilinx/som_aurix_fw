/**
 * @file    DebugCli.h
 * @brief   Interactive debug command interface over UART
 *
 * Implements PMC-IFC-001–004:
 *   001: Accept commands: power on/off, reset, clear fault, read state
 *   002: Reject commands invalid for current state
 *   003: Arbitrate request sources (CLI vs button vs watchdog vs fault)
 *   004: Support ASCLIN0 (debug UART) and ASCLIN4 (APU sideband UART)
 *
 * Command format: single-line text terminated by CR or LF.
 * Response: printed to the same UART.
 *
 * Commands:
 *   status        Full system status dump
 *   poweron       Request power on (PM_STATE_OFF → PM_STATE_ON)
 *   poweroff      Request graceful shutdown
 *   forceoff      Forced immediate power off
 *   warmreset     Warm reset (CF9-style)
 *   coldreset     Cold reboot
 *   clearfault    Clear fault latch, return to OFF
 *   tlf           TLF35585 register dump
 *   vmon          VoltMon channel report
 *   nvlog         NvLog slot summary
 *   nvlog recent [N]  Print last N events
 *   fusa          FUSA_SPI register map dump
 *   bist          POST/BIST status
 *   usbpd         USB PD port states
 *   temp          APU temperature (APML readback)
 *   uptime        Seconds since boot
 *   version       Firmware version string
 *   help          List available commands
 */

#ifndef DEBUGCLI_H
#define DEBUGCLI_H

#include "Ifx_Types.h"

/* ================================================================== */
/*  Configuration                                                     */
/* ================================================================== */

/** Maximum command line length */
#define CLI_MAX_CMD_LEN     80u

/** Command prompt string */
#define CLI_PROMPT          "aurix> "

/* ================================================================== */
/*  Public API                                                        */
/* ================================================================== */

/**
 * @brief  Initialise the debug CLI.
 *
 * Prints the welcome banner and initial prompt.
 */
void DebugCli_Init(void);

/**
 * @brief  Process incoming characters — call from main loop.
 *
 * Non-blocking.  Reads available characters from the debug
 * UART RX buffer, assembles a command line, and dispatches
 * on CR/LF.
 */
void DebugCli_Run(void);

#endif /* DEBUGCLI_H */