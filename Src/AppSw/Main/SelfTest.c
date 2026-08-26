/* ================================================================== */
/*  SelfTest.c — eval-board-only destructive self-tests               */
/*                                                                    */
/*  Extracted from Cpu0_Main.c.  Behavior unchanged except:           */
/*    - Tlf35585_ServiceWdt() calls are guarded on g_wdtOwner == 0    */
/*      so these functions cannot double-service the TLF window       */
/*      watchdog if ever invoked after the CPU2 handover (e.g. from   */
/*      a future CLI test command).                                   */
/*                                                                    */
/*  Constraints (see SelfTest.h):                                     */
/*    - CPU0 only (DFlash/PFlash are CPU0-owned peripherals)          */
/*    - After Ipc_Init() (g_wdtOwner must be initialized)             */
/* ================================================================== */
#include "SelfTest.h"

#if defined(TARGET_EVAL_BOARD)

/* ------------------------------------------------------------------ */
/*  TLF keep-alive that respects WDT ownership                        */
/* ------------------------------------------------------------------ */
static void prv_ServiceWdtIfOwner(void)
{
    if (g_wdtOwner == 0u)
    {
        Tlf35585_ServiceWdt();
    }
}

/* ------------------------------------------------------------------ */
/*  DFlash: SOTA metadata write / read-back / erase cycle             */
/* ------------------------------------------------------------------ */
void SelfTest_DFlash(void)
{
    DFlash_SotaMeta_t writeData;
    DFlash_SotaMeta_t readBack;
    DFlash_Status_t   status;
    uint32            errors = 0u;

    writeData.magic         = DFLASH_SOTA_MAGIC;
    writeData.pendingUpdate = 1u;
    writeData.bootCounter   = 42u;
    writeData.reserved0     = 0u;
    writeData.imageCrc      = 0xDEADBEEFu;
    writeData.activeBank    = 0x55u;
    writeData.reserved1     = 0u;
    writeData.reserved2     = 0u;

    status = DFlash_WriteSotaMeta(&writeData);
    if (status != DFLASH_OK) errors++;

    status = DFlash_ReadSotaMeta(&readBack);
    if (status != DFLASH_OK)                    errors++;
    if (readBack.magic != DFLASH_SOTA_MAGIC)    errors++;
    if (readBack.pendingUpdate != 1u)           errors++;
    if (readBack.bootCounter != 42u)            errors++;
    if (readBack.imageCrc != 0xDEADBEEFu)       errors++;
    if (readBack.activeBank != 0x55u)           errors++;

    DFlash_EraseSectors(DFLASH_SOTA_ADDR, 1u);
    DFlash_ReadSotaMeta(&readBack);
    if (readBack.magic != 0u)       errors++;
    if (readBack.bootCounter != 0u) errors++;

    /* Leave clean metadata behind */
    writeData.pendingUpdate = 0u;
    writeData.bootCounter   = 0u;
    writeData.imageCrc      = 0u;
    DFlash_WriteSotaMeta(&writeData);

    Debug_Printf("[DFLASH] %s (%u errors)\r\n",
                 (errors == 0u) ? "PASSED" : "FAILED", (unsigned)errors);
}

/* ------------------------------------------------------------------ */
/*  PFlash: erase / write / verify one page in the inactive bank      */
/* ------------------------------------------------------------------ */
void SelfTest_PFlash(void)
{
    uint8  testBuf[PFLASH_PAGE_SIZE] __attribute__((aligned(4)));
    uint32 i;
    PFlash_Status_t s;

    for (i = 0u; i < PFLASH_PAGE_SIZE; i++)
    {
        testBuf[i] = (uint8)(i & 0xFFu);
    }

    s = PFlash_EraseSector(PFLASH_BANK_B_BASE);
    if (s != PFLASH_OK)
    {
        Debug_Printf("[PFLASH] FAIL: erase (%u)\r\n", (unsigned)s);
        return;
    }
    prv_ServiceWdtIfOwner();

    s = PFlash_WritePage256(PFLASH_BANK_B_BASE, testBuf);
    if (s != PFLASH_OK)
    {
        Debug_Printf("[PFLASH] FAIL: write (%u)\r\n", (unsigned)s);
        return;
    }

    s = PFlash_VerifyPage256(PFLASH_BANK_B_BASE, testBuf);
    if (s != PFLASH_OK)
    {
        Debug_Printf("[PFLASH] FAIL: verify (%u)\r\n", (unsigned)s);
        return;
    }

    Debug_Printf("[PFLASH] %s\r\n", (s == PFLASH_OK) ? "PASSED" : "FAILED");
    prv_ServiceWdtIfOwner();
}

#endif /* TARGET_EVAL_BOARD */


/* ================================================================== */
/*  Local helpers — Phase 3 (core release / WDT handover)             */
/* ================================================================== */

/* Wait for CPU1/CPU2 ready flags, servicing the TLF WDT while CPU0
 * still owns it and draining core log rings so init output appears
 * live. Returns TRUE if both cores came up within timeoutMs. */
boolean prv_WaitForCores(uint32 timeoutMs)
{
    uint32 waitStart = Stm_GetTimeMs();

    while (!g_ipcShared.cpu1Ready || !g_ipcShared.cpu2Ready)
    {
        if (g_wdtOwner == 0u)
        {
            Tlf35585_ServiceWdt();
        }
        Debug_DrainRings();

        if ((Stm_GetTimeMs() - waitStart) > timeoutMs)
        {
            Debug_Print("[SYS] ERROR: core init timeout\r\n");
            if (!g_ipcShared.cpu1Ready)
            {
                Debug_Print("[SYS]   CPU1 (PMC) not ready\r\n");
            }
            if (!g_ipcShared.cpu2Ready)
            {
                Debug_Print("[SYS]   CPU2 (FuSa) not ready\r\n");
            }
            return FALSE;
        }
    }
    return TRUE;
}

/* Transfer TLF window-watchdog ownership to CPU2. One final service
 * as owner, then publish. CPU2 spins on g_wdtOwner == 2 before its
 * first service, so there is no overlap and a bounded (µs) gap. */
void prv_HandoverTlfWdt(void)
{
    Tlf35585_ServiceWdt();          /* final service as owner */
    __dsync();
    g_wdtOwner = 2u;
    __dsync();
    Debug_Print("[SYS] All cores running\r\n");
    Debug_Print("[SYS] TLF WDT -> CPU2\r\n");
}

/* ================================================================== */
/*  Local helpers — main loop                                         */
/* ================================================================== */

/* One-shot SOTA commit once the PMC reports the platform is ON. */
void prv_CommitSotaOnce(void)
{
    static boolean s_committed = FALSE;

    if (!s_committed && (g_ipcShared.pmc.pmState == (uint32)PM_STATE_ON))
    {
        BootValid_CommitUpdate();
        s_committed = TRUE;
    }
}

/* NV-log voltage faults published by CPU2, rate-limited: log on
 * change, or re-log the same persisting fault at most every 10 s. */
void prv_ForwardVoltageFaults(void)
{
    static uint32 s_lastLoggedSeq = 0u;
    uint32 seq = g_ipcShared.fusa.faultSeq;

    if (seq != s_lastLoggedSeq)
    {
        uint32 code = g_ipcShared.fusa.faultCode;

        s_lastLoggedSeq = seq;              /* consume ALWAYS, even when skipping */

        if (code >= (uint32)IPC_FAULT_PMIC) /* TLF-origin: already logged via
                                             * the tlfEvtSeq pipe — skip here */
            return;

        {
            uint32 nvData[4] = { code,
                                 g_ipcShared.fusa.faultChannel,
                                 g_ipcShared.fusa.faultMv,
                                 seq };
            NvLog_Write(NVLOG_EVT_FAULT_VOLTAGE, NVLOG_SRC_VOLTAGE,
                        NVLOG_SEV_ERROR, nvData);
        }
    }
}

void prv_ForwardVoltageWarnings(void)
{
    static uint32 s_lastWarnSeq = 0u;
    uint32 seq = g_ipcShared.fusa.warnSeq;

    if (seq != s_lastWarnSeq)
    {
        NvLog_WriteU32(NVLOG_EVT_FAULT_VOLTAGE, NVLOG_SRC_VOLTAGE,
                       NVLOG_SEV_WARNING,
                       (g_ipcShared.fusa.warnChannel << 16u) |
                       (g_ipcShared.fusa.warnMv & 0xFFFFu));
        s_lastWarnSeq = seq;
    }
}

/* Print + NV-log TLF events published by CPU2 (seq/data in IPC).
 * One print and one NV entry per new event — CPU2 itself must not
 * print or touch NvLog for these (see Tlf35585_LogEvent). */
void prv_ForwardTlfEvents(void)
{
    static uint32 s_lastTlfEvtSeq = 0u;

    uint32 seq = g_ipcShared.fusa.tlfEvtSeq;

    if (seq != s_lastTlfEvtSeq)
    {
        uint32 pmicData[4] = { g_ipcShared.fusa.tlfEvtData[0],   /* SYSSF   */
                               g_ipcShared.fusa.tlfEvtData[1],   /* MONSF1  */
                               g_ipcShared.fusa.tlfEvtData[2],   /* MONSF2  */
                               g_ipcShared.fusa.tlfEvtData[3] }; /* DEVSTAT */

        Debug_Printf("[TLF] EVT seq=%u SYSSF=0x%02X M1=0x%02X M2=0x%02X DEV=0x%02X\r\n",
                     (unsigned)seq,
                     (unsigned)pmicData[0], (unsigned)pmicData[1],
                     (unsigned)pmicData[2], (unsigned)pmicData[3]);

        NvLog_Write(NVLOG_EVT_PMIC_FAULT, NVLOG_SRC_PMIC,
                    NVLOG_SEV_ERROR, pmicData);
        s_lastTlfEvtSeq = seq;
    }
}

/* 20 Hz loop pacing with rate-limited overrun reporting. */
void prv_PaceLoop(uint32 loopStartMs)
{
    static uint32 s_lastOverrunLogMs = 0u;

    uint32 elapsedMs = Stm_GetTimeMs() - loopStartMs;

    if (elapsedMs < 50u)
    {
        Stm_DelayMs(50u - elapsedMs);
    }
    else
    {
        uint32 nowMs = Stm_GetTimeMs();
        if ((nowMs - s_lastOverrunLogMs) >= MAIN_LOOP_OVERRUN_LOG_INTERVAL_MS)
        {
            s_lastOverrunLogMs = nowMs;
            Debug_Printf("[CPU0] loop overrun: %ums\r\n", (unsigned)elapsedMs);
        }
    }
}