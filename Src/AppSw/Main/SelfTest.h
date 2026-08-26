/* ================================================================== */
/*  SelfTest.h — eval-board-only destructive self-tests               */
/*                                                                    */
/*  DFlash/PFlash self-tests extracted from Cpu0_Main.c.  These are   */
/*  destructive (they erase real sectors) and know the SOTA metadata  */
/*  layout, so they are compiled only for TARGET_EVAL_BOARD and must  */
/*  only be called from CPU0 during Phase 2 init, after Ipc_Init()    */
/*  (they read g_wdtOwner) and before the core-release barrier.       */
/* ================================================================== */
#ifndef SELFTEST_H
#define SELFTEST_H


#include "Ifx_Types.h"
#include "Ipc.h"          /* g_wdtOwner */
#include "DFlash.h"
#include "PFlash.h"
#include "Tlf35585.h"
#include "Uart_Debug.h"
#include "Ipc.h"
#include "NvLog.h"
#include "PowerManager.h"
#include "Stm_Timer.h"
#include "BootValid.h"

#if defined(TARGET_EVAL_BOARD)

/* Write/read/erase cycle on the SOTA metadata sector in DFlash.
 * Call after NvLog_Init(). Prints [DFLASH] PASSED/FAILED. */
void SelfTest_DFlash(void);

/* Erase/write/verify one page in the inactive PFlash bank.
 * Call after PFlash_Init() + POST. Prints [PFLASH] PASSED/FAIL. */
void SelfTest_PFlash(void);

#endif /* TARGET_EVAL_BOARD */

void prv_PaceLoop(uint32 loopStartMs);

void prv_ForwardTlfEvents(void);

void prv_ForwardVoltageFaults(void);

void prv_ForwardVoltageWarnings(void);

void prv_CommitSotaOnce(void);

void prv_HandoverTlfWdt(void);

boolean prv_WaitForCores(uint32 timeoutMs);

#endif /* SELFTEST_H */