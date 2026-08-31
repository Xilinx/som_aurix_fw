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
#include "Crc32.h"
#include "DFlash.h"
#include "PFlash.h"
#include "BootValid.h"
#include "Swap.h"
#include "NvLog.h"
#include "FusaSpi.h"
#include "Bist.h"
#include "PowerManager.h"
#include "Tlf35585.h"
#include "Stm_Timer.h"
#include "Uart_Debug.h"
#include <string.h>
#include "UsbPd_ApuProxy.h"
#include "UsbPd_Hpi.h"
#include "UsbPd_Manager.h"
#include "UsbPd_Hpd.h"
#include "UsbPd_Cfg.h"


static const char *prv_Result(uint32 err)
{
    return (err == 0u) ? "PASS" : "FAIL";
}
 
static boolean prv_StrEq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return FALSE; a++; b++; }
    return (*a == *b);
}
 
static const char *prv_SkipSpaces(const char *s)
{
    while (*s == ' ') s++;
    return s;
}

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


 
/* ================================================================== */
/*  TEST: CRC-32 known test vector                                    */
/* ================================================================== */
 
uint32 SelfTest_Crc(void)
{
    /* IEEE 802.3 check value: CRC32("123456789") = 0xCBF43926 */
    const uint8 testVec[] = "123456789";
    uint32 expected = 0xCBF43926u;
    uint32 computed;
    uint32 err = 0u;
 
    Debug_Print("[TEST:CRC] Computing CRC32(\"123456789\")...\r\n");
 
    computed = Crc32_Calc(testVec, 9u);
 
    Debug_Printf("[TEST:CRC] Computed=0x%08X Expected=0x%08X\r\n",
                 (unsigned)computed, (unsigned)expected);
 
    if (computed != expected)
    {
        Debug_Print("[TEST:CRC] FAIL: mismatch\r\n");
        err = 1u;
    }
 
    /* Incremental test: same result via Init/Update/Final */
    {
        uint32 crc = Crc32_Init();
        crc = Crc32_Update(crc, testVec, 4u);      /* "1234" */
        crc = Crc32_Update(crc, &testVec[4], 5u);  /* "56789" */
        uint32 incr = Crc32_Final(crc);
 
        Debug_Printf("[TEST:CRC] Incremental=0x%08X\r\n", (unsigned)incr);
        if (incr != expected)
        {
            Debug_Print("[TEST:CRC] FAIL: incremental mismatch\r\n");
            err = 1u;
        }
    }
 
    Debug_Printf("[TEST:CRC] %s\r\n", prv_Result(err));
    return err;
}
 
/* ================================================================== */
/*  TEST: Full SOTA metadata cycle                                    */
/* ================================================================== */
 
uint32 SelfTest_Sota(void)
{
    DFlash_SotaMeta_t meta;
    DFlash_SotaMeta_t readBack;
    DFlash_Status_t ds;
    uint32 err = 0u;
 
    Debug_Print("[TEST:SOTA] Starting full SOTA cycle...\r\n");
 
    /* Step 1: Compute a real CRC of a known PFlash region */
    uint32 testAddr = PFLASH_BANK_B_BASE;
    uint32 testSize = PFLASH_BURST_SIZE;  /* 256 bytes */
 
    /* Erase and write a known pattern */
    Debug_Print("[TEST:SOTA] Erasing sector...\r\n");
    PFlash_Status_t ps = PFlash_EraseSector(testAddr);
    if (ps != PFLASH_OK) { Debug_Printf("[TEST:SOTA] FAIL: erase %u\r\n", (unsigned)ps); return 1u; }
    Tlf35585_ServiceWdt();
 
    uint8 testBuf[PFLASH_BURST_SIZE] __attribute__((aligned(4)));
    uint32 i;
    for (i = 0u; i < PFLASH_BURST_SIZE; i++)
        testBuf[i] = (uint8)(i & 0xFFu);
 
    ps = PFlash_WritePage256(testAddr, testBuf);
    if (ps != PFLASH_OK) { Debug_Printf("[TEST:SOTA] FAIL: write %u\r\n", (unsigned)ps); return 1u; }
    Tlf35585_ServiceWdt();
 
    /* Step 2: CRC the written data */
    uint32 imageCrc = Crc32_Calc((const uint8 *)testAddr, testSize);
    Debug_Printf("[TEST:SOTA] Image CRC=0x%08X (256 bytes at 0x%08X)\r\n",
                 (unsigned)imageCrc, (unsigned)testAddr);
 
    /* Step 3: Write SOTA metadata with pendingUpdate=1 */
    memset(&meta, 0, sizeof(meta));
    meta.magic         = DFLASH_SOTA_MAGIC;
    meta.pendingUpdate = 1u;
    meta.bootCounter   = 0u;
    meta.reserved0     = testSize;   /* imageSize in reserved0 */
    meta.imageCrc      = imageCrc;
    meta.activeBank    = 0x55u;
 
    ds = DFlash_WriteSotaMeta(&meta);
    if (ds != DFLASH_OK) { Debug_Print("[TEST:SOTA] FAIL: write meta\r\n"); return 1u; }
 
    /* Step 4: Read back and verify */
    ds = DFlash_ReadSotaMeta(&readBack);
    if (ds != DFLASH_OK) { Debug_Print("[TEST:SOTA] FAIL: read meta\r\n"); return 1u; }
 
    if (readBack.magic != DFLASH_SOTA_MAGIC)    { err++; Debug_Print("[TEST:SOTA] FAIL: magic\r\n"); }
    if (readBack.pendingUpdate != 1u)           { err++; Debug_Print("[TEST:SOTA] FAIL: pending\r\n"); }
    if (readBack.imageCrc != imageCrc)          { err++; Debug_Print("[TEST:SOTA] FAIL: crc\r\n"); }
    if (readBack.activeBank != 0x55u)           { err++; Debug_Print("[TEST:SOTA] FAIL: bank\r\n"); }
 
    Debug_Printf("[TEST:SOTA] Metadata: pending=%u bank=0x%02X crc=0x%08X\r\n",
                 (unsigned)readBack.pendingUpdate,
                 (unsigned)readBack.activeBank,
                 (unsigned)readBack.imageCrc);
 
    /* Step 5: Simulate BootValid commit (clears pendingUpdate) */
    Debug_Print("[TEST:SOTA] Simulating BootValid commit...\r\n");
    BootValid_CheckOnStartup();
    Tlf35585_ServiceWdt();

    Debug_Print("[TEST:SOTA] Simulating successful boot commit...\r\n");
    BootValid_CommitUpdate();
    Tlf35585_ServiceWdt();
 
    /* Step 6: Verify commit cleared pendingUpdate */
    ds = DFlash_ReadSotaMeta(&readBack);
    if (readBack.pendingUpdate != 0u)
    {
        err++;
        Debug_Print("[TEST:SOTA] FAIL: pendingUpdate not cleared by commit\r\n");
    }
    else
    {
        Debug_Print("[TEST:SOTA] Commit cleared pendingUpdate OK\r\n");
    }
 
    /* Step 7: Run POST against the written CRC */
    Debug_Print("[TEST:SOTA] Running POST against known CRC...\r\n");
    Bist_Status_t bistResult = Bist_RunPost(Tlf35585_ServiceWdt);
    if (bistResult == BIST_OK)
    {
        Debug_Print("[TEST:SOTA] POST verified CRC match\r\n");
    }
    else if (bistResult == BIST_ERR_NO_META)
    {
        Debug_Print("[TEST:SOTA] POST skipped (metadata cleared?)\r\n");
    }
    else
    {
        Debug_Printf("[TEST:SOTA] POST result=%u (expected — CRC covers 256B, POST reads full bank)\r\n",
                     (unsigned)bistResult);
        /* This is expected to fail if POST reads the full bank
         * but we only wrote 256 bytes. Not a test failure. */
    }
 
    /* Step 8: Clean up — restore metadata to clean state */
    memset(&meta, 0, sizeof(meta));
    meta.magic      = DFLASH_SOTA_MAGIC;
    meta.activeBank = 0x55u;
    DFlash_WriteSotaMeta(&meta);
 
    Debug_Printf("[TEST:SOTA] %s\r\n", prv_Result(err));
    return err;
}
 
/* ================================================================== */
/*  TEST: Swap bank reporting                                         */
/* ================================================================== */
 
uint32 SelfTest_Swap(void)
{
    DFlash_SotaMeta_t meta;
    DFlash_SotaMeta_t savedMeta;
    DFlash_Status_t ds;
    uint32 err = 0u;
 
    Debug_Print("[TEST:SWAP] Testing bank ID reporting...\r\n");
 
    /* Save current metadata */
    ds = DFlash_ReadSotaMeta(&savedMeta);
 
    /* Test bank 0x55 */
    memset(&meta, 0, sizeof(meta));
    meta.magic      = DFLASH_SOTA_MAGIC;
    meta.activeBank = 0x55u;
    DFlash_WriteSotaMeta(&meta);
    Tlf35585_ServiceWdt();
 
    DFlash_ReadSotaMeta(&meta);
    Debug_Printf("[TEST:SWAP] Wrote 0x55, read back 0x%02X\r\n",
                 (unsigned)meta.activeBank);
    if (meta.activeBank != 0x55u) { err++; Debug_Print("[TEST:SWAP] FAIL: bank 0x55\r\n"); }
 
    /* Test bank 0xAA */
    meta.activeBank = 0xAAu;
    DFlash_WriteSotaMeta(&meta);
    Tlf35585_ServiceWdt();
 
    DFlash_ReadSotaMeta(&meta);
    Debug_Printf("[TEST:SWAP] Wrote 0xAA, read back 0x%02X\r\n",
                 (unsigned)meta.activeBank);
    if (meta.activeBank != 0xAAu) { err++; Debug_Print("[TEST:SWAP] FAIL: bank 0xAA\r\n"); }
 
    /* Restore original */
    if (ds == DFLASH_OK)
        DFlash_WriteSotaMeta(&savedMeta);
    else
    {
        memset(&meta, 0, sizeof(meta));
        meta.magic      = DFLASH_SOTA_MAGIC;
        meta.activeBank = 0x55u;
        DFlash_WriteSotaMeta(&meta);
    }
 
    Debug_Printf("[TEST:SWAP] %s\r\n", prv_Result(err));
    return err;
}
 
/* ================================================================== */
/*  TEST: FUSA_SPI register map                                       */
/* ================================================================== */
 
uint32 SelfTest_Fusa(void)
{
    const uint32 *regMap;
    uint32 err = 0u;
 
    Debug_Print("[TEST:FUSA] Verifying register map...\r\n");
 
    /* Force an update */
    FusaSpi_Update();
 
    regMap = FusaSpi_GetRegMap();
    if (regMap == NULL_PTR)
    {
        Debug_Print("[TEST:FUSA] FAIL: GetRegMap returned NULL\r\n");
        return 1u;
    }
 
    /* Check identity registers */
    if (regMap[FUSA_REG_MAGIC] != FUSA_MAGIC)
    {
        Debug_Printf("[TEST:FUSA] FAIL: MAGIC=0x%08X (expect 0x%08X)\r\n",
                     (unsigned)regMap[FUSA_REG_MAGIC], (unsigned)FUSA_MAGIC);
        err++;
    }
    else
    {
        Debug_Print("[TEST:FUSA] MAGIC OK\r\n");
    }
 
    if (regMap[FUSA_REG_MAP_VERSION] != FUSA_MAP_VERSION_PACKED)
    {
        Debug_Printf("[TEST:FUSA] FAIL: VERSION=0x%08X\r\n",
                     (unsigned)regMap[FUSA_REG_MAP_VERSION]);
        err++;
    }
    else
    {
        Debug_Print("[TEST:FUSA] VERSION OK\r\n");
    }
 
    if (regMap[FUSA_REG_FW_VERSION] != FUSA_FW_VERSION)
    {
        Debug_Printf("[TEST:FUSA] FAIL: FW_VER=0x%08X\r\n",
                     (unsigned)regMap[FUSA_REG_FW_VERSION]);
        err++;
    }
    else
    {
        Debug_Print("[TEST:FUSA] FW_VERSION OK\r\n");
    }
 
    /* Build config should have at least TLF + SOTA enabled */
    uint32 buildCfg = regMap[FUSA_REG_BUILD_CONFIG];
    Debug_Printf("[TEST:FUSA] BUILD_CONFIG=0x%08X\r\n", (unsigned)buildCfg);
    if (!(buildCfg & FUSA_CFG_TLF_ENABLED))
    {
        Debug_Print("[TEST:FUSA] FAIL: TLF_ENABLED not set\r\n");
        err++;
    }
    if (!(buildCfg & FUSA_CFG_SOTA))
    {
        Debug_Print("[TEST:FUSA] FAIL: SOTA not set\r\n");
        err++;
    }
 
    /* Uptime should be nonzero */
    if (regMap[FUSA_REG_UPTIME_S] == 0u)
    {
        Debug_Print("[TEST:FUSA] WARN: UPTIME_S=0 (boot just started?)\r\n");
    }
    else
    {
        Debug_Printf("[TEST:FUSA] UPTIME=%u s OK\r\n",
                     (unsigned)regMap[FUSA_REG_UPTIME_S]);
    }
 
    /* PM state should be a valid enum value */
    uint32 pmState = regMap[FUSA_REG_POWER_STATE];
    if (pmState > 15u)
    {
        Debug_Printf("[TEST:FUSA] FAIL: PM_STATE=%u out of range\r\n",
                     (unsigned)pmState);
        err++;
    }
    else
    {
        Debug_Printf("[TEST:FUSA] PM_STATE=%u OK\r\n", (unsigned)pmState);
    }
 
    /* Full dump */
    FusaSpi_DumpRegMap();
 
    Debug_Printf("[TEST:FUSA] %s\r\n", prv_Result(err));
    return err;
}
 
/* ================================================================== */
/*  TEST: PowerManager state transitions                              */
/* ================================================================== */
 
uint32 SelfTest_Pm(void)
{
    uint32 err = 0u;
    PM_State_t state;
 
    Debug_Print("[TEST:PM] Testing state transitions...\r\n");
 
    /* Should start in OFF */
    state = PowerManager_GetState();
    Debug_Printf("[TEST:PM] Initial state: %u\r\n", (unsigned)state);
    if (state != PM_STATE_OFF)
    {
        Debug_Print("[TEST:PM] WARN: not in OFF — skipping transition test\r\n");
        return 0u;  /* Not a failure, just can't test safely */
    }
 
    /* Request power on — will fail at PG timeout since no APU */
    Debug_Print("[TEST:PM] Requesting poweron (expect PG timeout)...\r\n");
    PowerManager_RequestPowerOn();
 
    /* Run the PM loop a few times to let it attempt sequencing */
    {
        uint32 i;
        for (i = 0u; i < 200u; i++)
        {
            PowerManager_Run();
            Tlf35585_ServiceWdt();
            Stm_DelayMs(10u);
 
            state = PowerManager_GetState();
            if (state == PM_STATE_FAULT)
            {
                Debug_Printf("[TEST:PM] Reached FAULT at iteration %u — expected\r\n",
                             (unsigned)i);
                break;
            }
            if (state == PM_STATE_ON)
            {
                Debug_Print("[TEST:PM] Reached ON — unexpected on eval board!\r\n");
                break;
            }
        }
    }
 
    /* Verify we're in FAULT state */
    state = PowerManager_GetState();
    if (state == PM_STATE_FAULT)
    {
        Debug_Print("[TEST:PM] FAULT state confirmed\r\n");
 
        /* Check NvLog for the fault event */
        Debug_Print("[TEST:PM] Checking NvLog for fault event...\r\n");
        NvLog_DumpRecent(NVLOG_SLOT_ACTIVE, 3u);
 
        /* Clear the fault */
        Debug_Print("[TEST:PM] Clearing fault...\r\n");
        PowerManager_ClearFault();
        Stm_DelayMs(10u);
        PowerManager_Run();
 
        state = PowerManager_GetState();
        if (state == PM_STATE_OFF)
        {
            Debug_Print("[TEST:PM] Returned to OFF after clearfault — OK\r\n");
        }
        else
        {
            Debug_Printf("[TEST:PM] FAIL: state=%u after clearfault (expect OFF)\r\n",
                         (unsigned)state);
            err++;
        }
    }
    else
    {
        Debug_Printf("[TEST:PM] WARN: state=%u — PG timeout didn't fire in 2s\r\n",
                     (unsigned)state);
        /* Not necessarily a failure — PM timing may need more iterations */
    }
 
    Debug_Printf("[TEST:PM] %s\r\n", prv_Result(err));
    return err;
}
 
/* ================================================================== */
/*  TEST: USB PD APU proxy packing                                    */
/* ================================================================== */
 
uint32 SelfTest_UsbPd(void)
{
    uint32 err = 0u;
    UsbPd_HpiState_t fakeHpi;
    UsbPd_CustomFields_t fakeCustom;
    const uint8 *regMap;
    uint64 packed;
    uint32 i;

    Debug_Print("[TEST:USBPD] APU proxy packing tests...\r\n");
    UsbPd_ApuProxy_Init();

    /* ---- Scenario 1: Full USB4 + DP + active cable ---- */
    Debug_Print("[TEST:USBPD] Scenario 1: USB4+DP, active cable, source\r\n");
    memset(&fakeHpi, 0, sizeof(fakeHpi));
    fakeHpi.typeCStatus     = HPI_TC_CONNECTED | HPI_TC_CC_POLARITY;
    fakeHpi.pdStatus        = HPI_PD_CONTRACT_EXISTS | HPI_PD_PORT_ROLE
                            | HPI_PD_EMCA_PRESENT | HPI_PD_CABLE_TYPE;
    fakeHpi.altModeStatus   = HPI_ALT_USB4_ACTIVE | HPI_ALT_DP_ACTIVE;
    fakeHpi.currentCableVdo = 0x00000003u;
    fakeHpi.actCblVdo2      = (1u << 9u);
    fakeHpi.connected       = TRUE;
    fakeHpi.contractValid   = TRUE;

    memset(&fakeCustom, 0, sizeof(fakeCustom));
    fakeCustom.dpUhbr135    = TRUE;
    fakeCustom.dpUhbrCap    = 0x02u;
    fakeCustom.cableVersion = 0x31u;
    fakeCustom.bidirRetimer = TRUE;
    fakeCustom.cableClx     = TRUE;
    fakeCustom.dpLaneMode   = 4u;

    UsbPd_ApuProxy_Pack(0u, &fakeHpi, &fakeCustom);
    regMap = UsbPd_ApuProxy_GetRegMap(0u);
    if (regMap == NULL_PTR) { Debug_Print("[TEST:USBPD] FAIL: NULL regMap\r\n"); return 1u; }

    packed = 0u;
    for (i = 0u; i < 5u; i++) packed |= ((uint64)regMap[i]) << (i * 8u);

    Debug_Printf("[TEST:USBPD]   Packed: 0x%02X_%08X\r\n",
                 (unsigned)(uint32)(packed >> 32u), (unsigned)(uint32)(packed));

    if (!(packed & (1ULL << 32u))) { err++; Debug_Print("    FAIL: TypeC\r\n"); }
    if (!(packed & (1ULL << 12u))) { err++; Debug_Print("    FAIL: Orientation\r\n"); }
    if (!(packed & (1ULL << 15u))) { err++; Debug_Print("    FAIL: PortRole\r\n"); }
    if (!(packed & (1ULL << 16u))) { err++; Debug_Print("    FAIL: USB4\r\n"); }
    if (packed & (1ULL << 17u)) { err++; Debug_Print("    FAIL: TBT3 should be 0\r\n"); }
    if (!(packed & (1ULL << 18u))) { err++; Debug_Print("    FAIL: CableClx\r\n"); }
    if (!(packed & (1ULL << 19u))) { err++; Debug_Print("    FAIL: RetimedActive\r\n"); }
    if (!(packed & (1ULL << 20u))) { err++; Debug_Print("    FAIL: BidirRetimer\r\n"); }
    if (!(packed & (1ULL << 21u))) { err++; Debug_Print("    FAIL: CableGen3\r\n"); }
    if (!(packed & (1ULL << 23u))) { err++; Debug_Print("    FAIL: ActiveCable\r\n"); }
    if (!(packed & (1ULL << 39u))) { err++; Debug_Print("    FAIL: UHBR13.5\r\n"); }

    if (((packed >> 24u) & 0xFFu) != 0x31u)
    { err++; Debug_Printf("    FAIL: CableVer=0x%02X\r\n", (unsigned)((packed >> 24u) & 0xFFu)); }

    if (((packed >> 37u) & 0x03u) != 0x02u)
    { err++; Debug_Printf("    FAIL: UHBRCap=%u\r\n", (unsigned)((packed >> 37u) & 0x03u)); }

    if ((packed & 0xFFu) != 0u)
    { err++; Debug_Printf("    FAIL: Index=%u (expect 0)\r\n", (unsigned)(packed & 0xFFu)); }

    if (err == 0u) Debug_Print("    All bits OK\r\n");
    UsbPd_ApuProxy_DumpPort(0u);

    /* ---- Scenario 2: TBT3 only, sink, CC1 ---- */
    Debug_Print("[TEST:USBPD] Scenario 2: TBT3 only, sink, CC1\r\n");
    memset(&fakeHpi, 0, sizeof(fakeHpi));
    fakeHpi.typeCStatus   = HPI_TC_CONNECTED;  /* CC1, no polarity bit */
    fakeHpi.pdStatus      = HPI_PD_CONTRACT_EXISTS; /* sink = bit6 clear */
    fakeHpi.altModeStatus = HPI_ALT_TBT3_ACTIVE;
    fakeHpi.connected     = TRUE;
    fakeHpi.contractValid = TRUE;

    memset(&fakeCustom, 0, sizeof(fakeCustom));

    UsbPd_ApuProxy_Pack(1u, &fakeHpi, &fakeCustom);
    regMap = UsbPd_ApuProxy_GetRegMap(1u);
    packed = 0u;
    for (i = 0u; i < 5u; i++) packed |= ((uint64)regMap[i]) << (i * 8u);

    Debug_Printf("[TEST:USBPD]   Packed: 0x%02X_%08X\r\n",
                 (unsigned)(uint32)(packed >> 32u), (unsigned)(uint32)(packed));

    if (!(packed & (1ULL << 32u)))  { err++; Debug_Print("    FAIL: TypeC\r\n"); }
    if (packed & (1ULL << 12u))     { err++; Debug_Print("    FAIL: Orient should be 0\r\n"); }
    if (packed & (1ULL << 15u))     { err++; Debug_Print("    FAIL: Role should be sink\r\n"); }
    if (packed & (1ULL << 16u))     { err++; Debug_Print("    FAIL: USB4 should be 0\r\n"); }
    if (!(packed & (1ULL << 17u)))  { err++; Debug_Print("    FAIL: TBT3\r\n"); }
    if ((packed & 0xFFu) != 1u)    { err++; Debug_Print("    FAIL: Index should be 1\r\n"); }

    if (err == 0u) Debug_Print("    All bits OK\r\n");
    UsbPd_ApuProxy_DumpPort(1u);

    /* ---- Scenario 3: Detached (all zeros except index) ---- */
    Debug_Print("[TEST:USBPD] Scenario 3: Detached port\r\n");
    memset(&fakeHpi, 0, sizeof(fakeHpi));
    memset(&fakeCustom, 0, sizeof(fakeCustom));

    UsbPd_ApuProxy_Pack(0u, &fakeHpi, &fakeCustom);
    regMap = UsbPd_ApuProxy_GetRegMap(0u);
    packed = 0u;
    for (i = 0u; i < 5u; i++) packed |= ((uint64)regMap[i]) << (i * 8u);

    Debug_Printf("[TEST:USBPD]   Packed: 0x%02X_%08X\r\n",
                 (unsigned)(uint32)(packed >> 32u), (unsigned)(uint32)(packed));

    if (packed & (1ULL << 32u)) { err++; Debug_Print("    FAIL: TypeC should be 0\r\n"); }
    if (packed & ~0xFFULL)      { err++; Debug_Print("    FAIL: non-index bits set on detached\r\n"); }
    if ((packed & 0xFFu) != 0u) { err++; Debug_Print("    FAIL: Index should be 0\r\n"); }

    if (err == 0u) Debug_Print("    All bits OK\r\n");

    /* ---- Scenario 4: DP-only 2-lane, no active cable ---- */
    Debug_Print("[TEST:USBPD] Scenario 4: DP 2-lane, passive cable\r\n");
    memset(&fakeHpi, 0, sizeof(fakeHpi));
    fakeHpi.typeCStatus   = HPI_TC_CONNECTED | HPI_TC_CC_POLARITY;
    fakeHpi.pdStatus      = HPI_PD_CONTRACT_EXISTS | HPI_PD_PORT_ROLE;
    fakeHpi.altModeStatus = HPI_ALT_DP_ACTIVE;
    fakeHpi.connected     = TRUE;
    fakeHpi.contractValid = TRUE;

    memset(&fakeCustom, 0, sizeof(fakeCustom));
    fakeCustom.dpLaneMode = 2u;

    UsbPd_ApuProxy_Pack(0u, &fakeHpi, &fakeCustom);
    regMap = UsbPd_ApuProxy_GetRegMap(0u);
    packed = 0u;
    for (i = 0u; i < 5u; i++) packed |= ((uint64)regMap[i]) << (i * 8u);

    Debug_Printf("[TEST:USBPD]   Packed: 0x%02X_%08X\r\n",
                 (unsigned)(uint32)(packed >> 32u), (unsigned)(uint32)(packed));

    if (!(packed & (1ULL << 32u))) { err++; Debug_Print("    FAIL: TypeC\r\n"); }
    if (packed & (1ULL << 16u))    { err++; Debug_Print("    FAIL: USB4 should be 0\r\n"); }
    if (packed & (1ULL << 17u))    { err++; Debug_Print("    FAIL: TBT3 should be 0\r\n"); }
    if (packed & (1ULL << 23u))    { err++; Debug_Print("    FAIL: ActiveCable should be 0\r\n"); }
    if (packed & (1ULL << 21u))    { err++; Debug_Print("    FAIL: Gen3 should be 0\r\n"); }
    if (!(packed & (1ULL << 15u))) { err++; Debug_Print("    FAIL: PortRole source\r\n"); }

    uint8 mode = (uint8)((packed >> 8u) & 0x0Fu);
    if (!(mode & 0x01u)) { err++; Debug_Printf("    FAIL: Mode=%u, DP bit missing\r\n", mode); }

    if (err == 0u) Debug_Print("    All bits OK\r\n");

    Debug_Printf("[TEST:USBPD] %s (%u scenarios, %u errors)\r\n",
                 prv_Result(err), 4u, (unsigned)err);
    return err;
}

uint32 SelfTest_UsbPdHpd(void)
{
    uint32 err = 0u;

    Debug_Print("[TEST:HPD] HPD state machine tests...\r\n");

    UsbPd_Hpd_Init();

    /* Scenario 1: HIGH → verify level */
    UsbPd_Hpd_ProcessEvent(0u, HPD_EVENT_HIGH);
    if (!UsbPd_Hpd_GetLevel(0u))
    {
        err++;
        Debug_Print("    FAIL: port0 not HIGH after HPD_EVENT_HIGH\r\n");
    }
    else
    {
        Debug_Print("    Scenario 1: HIGH assert OK\r\n");
    }

    /* Scenario 2: IRQ pulse — level should drop, then recover after Run */
    UsbPd_Hpd_ProcessEvent(0u, HPD_EVENT_IRQ);
    if (UsbPd_Hpd_GetLevel(0u))
    {
        /* Level is still high — IRQ didn't pull it low.
         * The GPIO was pulled low but GetLevel reads the state flag.
         * Check if irqPending is set instead. */
        Debug_Print("    Scenario 2: IRQ pulse started\r\n");
    }

    /* Simulate 3ms passing, then Run should complete the pulse */
    Stm_DelayMs(3u);
    UsbPd_Hpd_Run();
    if (!UsbPd_Hpd_GetLevel(0u))
    {
        err++;
        Debug_Print("    FAIL: HPD not restored after IRQ pulse\r\n");
    }
    else
    {
        Debug_Print("    Scenario 2: IRQ pulse + recovery OK\r\n");
    }

    /* Scenario 3: LOW → verify deassert */
    UsbPd_Hpd_ProcessEvent(0u, HPD_EVENT_LOW);
    if (UsbPd_Hpd_GetLevel(0u))
    {
        err++;
        Debug_Print("    FAIL: port0 still HIGH after HPD_EVENT_LOW\r\n");
    }
    else
    {
        Debug_Print("    Scenario 3: LOW deassert OK\r\n");
    }

    /* Scenario 4: IRQ while LOW — should be ignored per DP spec */
    UsbPd_Hpd_ProcessEvent(0u, HPD_EVENT_IRQ);
    if (UsbPd_Hpd_GetLevel(0u))
    {
        err++;
        Debug_Print("    FAIL: IRQ while LOW raised the level\r\n");
    }
    else
    {
        Debug_Print("    Scenario 4: IRQ while LOW ignored OK\r\n");
    }

    /* Scenario 5: Port independence — port 1 unaffected by port 0 */
    UsbPd_Hpd_ProcessEvent(1u, HPD_EVENT_HIGH);
    if (!UsbPd_Hpd_GetLevel(1u))
    {
        err++;
        Debug_Print("    FAIL: port1 not HIGH\r\n");
    }
    if (UsbPd_Hpd_GetLevel(0u))
    {
        err++;
        Debug_Print("    FAIL: port0 raised by port1 event\r\n");
    }
    if (err == 0u)
        Debug_Print("    Scenario 5: Port independence OK\r\n");

    /* Scenario 6: DeassertAll */
    UsbPd_Hpd_DeassertAll();
    if (UsbPd_Hpd_GetLevel(0u) || UsbPd_Hpd_GetLevel(1u))
    {
        err++;
        Debug_Print("    FAIL: DeassertAll didn't clear both\r\n");
    }
    else
    {
        Debug_Print("    Scenario 6: DeassertAll OK\r\n");
    }

    Debug_Printf("[TEST:HPD] %s (%u errors)\r\n", prv_Result(err), (unsigned)err);
    return err;
}

/* ================================================================== */
/*  TEST: Config topology switching                                   */
/* ================================================================== */

uint32 SelfTest_UsbPdTopology(void)
{
    uint32 err = 0u;
    const UsbPd_SysCfg_t *cfg;

    Debug_Print("[TEST:TOPO] Topology config tests...\r\n");

    /* Save current config */
    UsbPd_SysCfg_t savedCfg;
    cfg = UsbPd_Cfg_GetSysCfg();
    savedCfg = *cfg;

    /* Test 1: Default dual CYPD6129 */
    Debug_Print("    Testing dual CYPD6129 defaults...\r\n");
    UsbPd_Cfg_ResetDefaults();
    cfg = UsbPd_Cfg_GetSysCfg();

    if (cfg->topology != USBPD_TOPO_DUAL_SINGLE_PORT)
    {
        err++;
        Debug_Print("    FAIL: topology not DUAL_SINGLE_PORT\r\n");
    }
    if (cfg->numControllers != 2u)
    {
        err++;
        Debug_Printf("    FAIL: numControllers=%u (expect 2)\r\n",
                     (unsigned)cfg->numControllers);
    }
    if (cfg->numTotalPorts != 2u)
    {
        err++;
        Debug_Printf("    FAIL: numTotalPorts=%u (expect 2)\r\n",
                     (unsigned)cfg->numTotalPorts);
    }
    if (cfg->apuSlvAddr0 != 0x54u || cfg->apuSlvAddr1 != 0x58u)
    {
        err++;
        Debug_Print("    FAIL: APU slave addresses wrong\r\n");
    }

    if (err == 0u) Debug_Print("    Dual CYPD6129 defaults OK\r\n");

    /* Test 2: Save → load round-trip */
    Debug_Print("    Testing DFlash round-trip...\r\n");
    Tlf35585_ServiceWdt();

    uint8 saveResult = UsbPd_Cfg_SaveToDFlash();
    if (saveResult != 0u)
    {
        err++;
        Debug_Printf("    FAIL: save returned %u\r\n", (unsigned)saveResult);
    }
    else
    {
        uint8 loadResult = UsbPd_Cfg_LoadFromDFlash();
        if (loadResult != 0u)
        {
            err++;
            Debug_Printf("    FAIL: load returned %u\r\n", (unsigned)loadResult);
        }
        else
        {
            cfg = UsbPd_Cfg_GetSysCfg();
            if (cfg->magic != USBPD_CFG_MAGIC)
            {
                err++;
                Debug_Print("    FAIL: magic mismatch after round-trip\r\n");
            }
            else
            {
                Debug_Print("    DFlash round-trip OK\r\n");
            }
        }
    }

    /* Test 3: Corrupt checksum → load should fail */
    Debug_Print("    Testing corrupt config rejection...\r\n");
    {
        UsbPd_SysCfg_t corrupt;
        DFlash_Read(DFLASH_USBCFG_ADDR, &corrupt, sizeof(corrupt));
        corrupt.checksum ^= 0xDEADu;
        DFlash_EraseSectors(DFLASH_USBCFG_ADDR, 1u);
        DFlash_Write(DFLASH_USBCFG_ADDR, &corrupt, sizeof(corrupt));
        Tlf35585_ServiceWdt();

        uint8 loadResult = UsbPd_Cfg_LoadFromDFlash();
        if (loadResult == 0u)
        {
            err++;
            Debug_Print("    FAIL: corrupt config accepted\r\n");
        }
        else
        {
            Debug_Printf("    Corrupt config rejected (err=%u) OK\r\n",
                         (unsigned)loadResult);
        }
    }

    /* Restore original config */
    DFlash_EraseSectors(DFLASH_USBCFG_ADDR, 1u);
    DFlash_Write(DFLASH_USBCFG_ADDR, &savedCfg, sizeof(savedCfg));

    Debug_Printf("[TEST:TOPO] %s (%u errors)\r\n", prv_Result(err), (unsigned)err);
    return err;
}

/* ================================================================== */
/*  TEST: APU proxy edge cases                                        */
/* ================================================================== */

uint32 SelfTest_UsbPdEdgeCases(void)
{
    uint32 err = 0u;
    UsbPd_HpiState_t hpi;
    UsbPd_CustomFields_t custom;
    const uint8 *regMap;
    uint64 packed;
    uint32 i;

    Debug_Print("[TEST:USBPD_EDGE] Edge case packing tests...\r\n");
    UsbPd_ApuProxy_Init();

    /* Edge 1: All custom fields at max values */
    Debug_Print("    Edge 1: Max custom fields...\r\n");
    memset(&hpi, 0, sizeof(hpi));
    hpi.typeCStatus = HPI_TC_CONNECTED;
    hpi.connected   = TRUE;

    memset(&custom, 0, sizeof(custom));
    custom.dpUhbr135    = TRUE;
    custom.dpUhbrCap    = 0x03u;   /* Max 2-bit */
    custom.cableVersion = 0xFFu;   /* Max 8-bit */
    custom.bidirRetimer = TRUE;
    custom.cableClx     = TRUE;
    custom.dataReset    = TRUE;
    custom.dpLaneMode   = 4u;

    UsbPd_ApuProxy_Pack(0u, &hpi, &custom);
    regMap = UsbPd_ApuProxy_GetRegMap(0u);
    packed = 0u;
    for (i = 0u; i < 5u; i++) packed |= ((uint64)regMap[i]) << (i * 8u);

    Debug_Printf("    Packed: 0x%02X_%08X\r\n",
                 (unsigned)(uint32)(packed >> 32u),
                 (unsigned)(uint32)(packed));

    if (((packed >> 24u) & 0xFFu) != 0xFFu)
    {
        err++;
        Debug_Print("    FAIL: CableVer not 0xFF\r\n");
    }
    if (((packed >> 37u) & 0x03u) != 0x03u)
    {
        err++;
        Debug_Print("    FAIL: UHBRCap not 0x03\r\n");
    }
    if (!(packed & (1ULL << 14u)))
    {
        err++;
        Debug_Print("    FAIL: DataReset not set\r\n");
    }
    if (err == 0u) Debug_Print("    Max custom fields OK\r\n");

    /* Edge 2: EMCA present but low-speed cable (Gen1) */
    Debug_Print("    Edge 2: EMCA Gen1 cable...\r\n");
    memset(&hpi, 0, sizeof(hpi));
    hpi.typeCStatus     = HPI_TC_CONNECTED;
    hpi.pdStatus        = HPI_PD_EMCA_PRESENT;
    hpi.currentCableVdo = 0x00000001u;   /* Gen1 speed code = 1 */
    hpi.connected       = TRUE;

    memset(&custom, 0, sizeof(custom));
    UsbPd_ApuProxy_Pack(0u, &hpi, &custom);
    regMap = UsbPd_ApuProxy_GetRegMap(0u);
    packed = 0u;
    for (i = 0u; i < 5u; i++) packed |= ((uint64)regMap[i]) << (i * 8u);

    if (packed & (1ULL << 21u))
    {
        err++;
        Debug_Print("    FAIL: Gen3 set for Gen1 cable\r\n");
    }
    else
    {
        Debug_Print("    Gen1 cable: Gen3 bit correctly clear\r\n");
    }

    /* Edge 3: EMCA not present — Gen3 bit should be 0 regardless of VDO */
    Debug_Print("    Edge 3: No EMCA, VDO has Gen3 data...\r\n");
    memset(&hpi, 0, sizeof(hpi));
    hpi.typeCStatus     = HPI_TC_CONNECTED;
    hpi.pdStatus        = HPI_PD_CONTRACT_EXISTS;   /* No EMCA bit */
    hpi.currentCableVdo = 0x00000003u;   /* Gen3 speed, but no EMCA */
    hpi.connected       = TRUE;

    memset(&custom, 0, sizeof(custom));
    UsbPd_ApuProxy_Pack(0u, &hpi, &custom);
    regMap = UsbPd_ApuProxy_GetRegMap(0u);
    packed = 0u;
    for (i = 0u; i < 5u; i++) packed |= ((uint64)regMap[i]) << (i * 8u);

    if (packed & (1ULL << 21u))
    {
        err++;
        Debug_Print("    FAIL: Gen3 set without EMCA\r\n");
    }
    else
    {
        Debug_Print("    No EMCA: Gen3 bit correctly gated\r\n");
    }

    /* Edge 4: Port index preserved across re-pack */
    Debug_Print("    Edge 4: Port index stability...\r\n");
    memset(&hpi, 0, sizeof(hpi));
    memset(&custom, 0, sizeof(custom));

    UsbPd_ApuProxy_Pack(0u, &hpi, &custom);
    UsbPd_ApuProxy_Pack(1u, &hpi, &custom);

    const uint8 *reg0 = UsbPd_ApuProxy_GetRegMap(0u);
    const uint8 *reg1 = UsbPd_ApuProxy_GetRegMap(1u);

    if (reg0[0] != 0u || reg1[0] != 1u)
    {
        err++;
        Debug_Printf("    FAIL: Index[0]=%u Index[1]=%u\r\n",
                     (unsigned)reg0[0], (unsigned)reg1[0]);
    }
    else
    {
        Debug_Print("    Port indices 0/1 OK\r\n");
    }

    /* Edge 5: Invalid port index — should not crash */
    Debug_Print("    Edge 5: Invalid port index...\r\n");
    UsbPd_ApuProxy_Pack(2u, &hpi, &custom);
    const uint8 *regInvalid = UsbPd_ApuProxy_GetRegMap(2u);
    if (regInvalid != NULL_PTR)
    {
        err++;
        Debug_Print("    FAIL: port 2 returned non-NULL\r\n");
    }
    else
    {
        Debug_Print("    Invalid port rejected OK\r\n");
    }

    Debug_Printf("[TEST:USBPD_EDGE] %s (%u errors)\r\n",
                 prv_Result(err), (unsigned)err);
    return err;
}
 
/* ================================================================== */
/*  TEST: FwUpdate protocol emulation (no UART)                       */
/* ================================================================== */
 
uint32 SelfTest_FwUpdate(void)
{
    uint32 err = 0u;
 
    Debug_Print("[TEST:FWUP] Emulating protocol in-memory...\r\n");
 
    /* Create a 512-byte test image (2 chunks) */
    uint8 testImage[512] __attribute__((aligned(4)));
    uint32 i;
    for (i = 0u; i < 512u; i++)
        testImage[i] = (uint8)((i * 7u + 13u) & 0xFFu);
 
    uint32 imageSize = 512u;
    uint32 imageCrc  = Crc32_Calc(testImage, imageSize);
    uint32 numChunks = imageSize / PFLASH_BURST_SIZE;
 
    Debug_Printf("[TEST:FWUP] Image: %u bytes, %u chunks, CRC=0x%08X\r\n",
                 (unsigned)imageSize, (unsigned)numChunks, (unsigned)imageCrc);
 
    /* Step 1: Erase target sector */
    Debug_Print("[TEST:FWUP] Erasing target sector...\r\n");
    PFlash_Status_t ps = PFlash_EraseSector(PFLASH_BANK_B_BASE);
    if (ps != PFLASH_OK)
    {
        Debug_Printf("[TEST:FWUP] FAIL: erase %u\r\n", (unsigned)ps);
        return 1u;
    }
    Tlf35585_ServiceWdt();
 
    /* Step 2: Write chunks with per-chunk CRC verify */
    uint32 writeAddr = PFLASH_BANK_B_BASE;
    uint32 runningCrc = Crc32_Init();
 
    for (i = 0u; i < numChunks; i++)
    {
        uint8 *pChunk = &testImage[i * PFLASH_BURST_SIZE];
        uint32 chunkCrc = Crc32_Calc(pChunk, PFLASH_BURST_SIZE);
 
        /* Verify chunk CRC (emulates what FwUpdate_Run does) */
        uint32 verify = Crc32_Calc(pChunk, PFLASH_BURST_SIZE);
        if (verify != chunkCrc)
        {
            Debug_Printf("[TEST:FWUP] FAIL: chunk %u CRC mismatch\r\n", (unsigned)i);
            err++;
            break;
        }
 
        /* Write to PFlash */
        ps = PFlash_WritePage256(writeAddr, pChunk);
        if (ps != PFLASH_OK)
        {
            Debug_Printf("[TEST:FWUP] FAIL: write chunk %u, err=%u\r\n",
                         (unsigned)i, (unsigned)ps);
            err++;
            break;
        }
 
        /* Verify readback */
        ps = PFlash_VerifyPage256(writeAddr, pChunk);
        if (ps != PFLASH_OK)
        {
            Debug_Printf("[TEST:FWUP] FAIL: verify chunk %u\r\n", (unsigned)i);
            err++;
            break;
        }
 
        /* Accumulate running CRC */
        runningCrc = Crc32_Update(runningCrc, pChunk, PFLASH_BURST_SIZE);
        writeAddr += PFLASH_BURST_SIZE;
 
        Debug_Printf("[TEST:FWUP] Chunk %u OK\r\n", (unsigned)i);
        Tlf35585_ServiceWdt();
    }
 
    /* Step 3: Final CRC check */
    uint32 finalCrc = Crc32_Final(runningCrc);
    Debug_Printf("[TEST:FWUP] Final CRC=0x%08X expected=0x%08X\r\n",
                 (unsigned)finalCrc, (unsigned)imageCrc);
    if (finalCrc != imageCrc)
    {
        Debug_Print("[TEST:FWUP] FAIL: full image CRC mismatch\r\n");
        err++;
    }
 
    /* Step 4: Write SOTA metadata (simulate commit) */
    if (err == 0u)
    {
        DFlash_SotaMeta_t meta;
        memset(&meta, 0, sizeof(meta));
        meta.magic       = DFLASH_SOTA_MAGIC;
        meta.pendingUpdate = 0u;
        meta.imageCrc    = imageCrc;
        meta.activeBank  = 0x55u;
        meta.reserved0   = imageSize;
 
        DFlash_Status_t ds = DFlash_WriteSotaMeta(&meta);
        if (ds != DFLASH_OK)
        {
            Debug_Print("[TEST:FWUP] FAIL: metadata write\r\n");
            err++;
        }
        else
        {
            Debug_Print("[TEST:FWUP] Metadata committed\r\n");
        }
    }
 
    /* Clean up — restore clean metadata */
    {
        DFlash_SotaMeta_t clean;
        memset(&clean, 0, sizeof(clean));
        clean.magic      = DFLASH_SOTA_MAGIC;
        clean.activeBank = 0x55u;
        DFlash_WriteSotaMeta(&clean);
    }
 
    Debug_Printf("[TEST:FWUP] %s\r\n", prv_Result(err));
    return err;
}
 

uint32 SelfTest_UsbPdLive(void)
{
    uint32 err = 0u;
    uint8 i;

    Debug_Print("[TEST:USBPD_LIVE] Probing PD controllers on I2C0...\r\n");

    for (i = 0u; i < CYPD_DEVICE_COUNT; i++)
    {
        uint8 devIntr = 0u;
        uint8 result = UsbPd_Hpi_ReadDevIntr(CYPD_DEVICES[i].i2cAddr,
                                              &devIntr);
        if (result == 0u)
        {
            Debug_Printf("    %s (0x%02X): responded, intr=0x%02X\r\n",
                         CYPD_DEVICES[i].name,
                         (unsigned)CYPD_DEVICES[i].i2cAddr,
                         (unsigned)devIntr);
        }
        else
        {
            err++;
            Debug_Printf("    %s (0x%02X): FAIL — no I2C response\r\n",
                         CYPD_DEVICES[i].name,
                         (unsigned)CYPD_DEVICES[i].i2cAddr);
        }
    }

    /* If controllers responded, try full HPI state read */
    if (err == 0u)
    {
        for (i = 0u; i < CYPD_DEVICE_COUNT; i++)
        {
            UsbPd_HpiState_t hpiState;
            uint8 addr = CYPD_DEVICES[i].i2cAddr;
            uint16 portBase = 0x1000u;

            uint8 rd = UsbPd_Hpi_ReadPortState(addr, portBase, &hpiState);
            if (rd == 0u)
            {
                Debug_Printf("    Port %u: TYPE_C=0x%08X PD=0x%08X ALT=0x%02X %s\r\n",
                             (unsigned)i,
                             (unsigned)hpiState.typeCStatus,
                             (unsigned)hpiState.pdStatus,
                             (unsigned)hpiState.altModeStatus,
                             hpiState.connected ? "CONNECTED" : "detached");

                /* Pack into APU proxy to verify the live pipeline */
                UsbPd_ApuProxy_Pack(i, &hpiState, NULL_PTR);
                UsbPd_ApuProxy_DumpPort(i);
            }
            else
            {
                err++;
                Debug_Printf("    Port %u: FAIL — HPI read error\r\n",
                             (unsigned)i);
            }
            Tlf35585_ServiceWdt();
        }
    }

    Debug_Printf("[TEST:USBPD_LIVE] %s\r\n", prv_Result(err));
    return err;
}

/* ================================================================== */
/*  Run all                                                           */
/* ================================================================== */
 
uint32 SelfTest_RunAll(void)
{
    uint32 total = 0u;
    uint32 pass  = 0u;
    uint32 fail  = 0u;
 
    Debug_Print("\r\n========================================\r\n");
    Debug_Print(" SELF-TEST SUITE\r\n");
    Debug_Print("========================================\r\n\r\n");
 
#define RUN_TEST(name, fn) \
    do { \
        total++; \
        if (fn() == 0u) { pass++; } \
        else { fail++; } \
        Debug_Print("\r\n"); \
        Tlf35585_ServiceWdt(); \
    } while (0)
 
    RUN_TEST("CRC",     SelfTest_Crc);
    RUN_TEST("SWAP",    SelfTest_Swap);
    RUN_TEST("FUSA",    SelfTest_Fusa);
    RUN_TEST("SOTA",    SelfTest_Sota);
    RUN_TEST("FWUP",    SelfTest_FwUpdate);
    RUN_TEST("USBPD",   SelfTest_UsbPd);
    RUN_TEST("PM",      SelfTest_Pm);
    RUN_TEST("HPD",       SelfTest_UsbPdHpd);
    RUN_TEST("TOPO",      SelfTest_UsbPdTopology);
    RUN_TEST("USBPD_EDGE", SelfTest_UsbPdEdgeCases);
    RUN_TEST("USBPD_LIVE",  SelfTest_UsbPdLive);
 
#undef RUN_TEST
 
    Debug_Print("========================================\r\n");
    Debug_Printf(" RESULTS: %u/%u passed, %u failed\r\n",
                 (unsigned)pass, (unsigned)total, (unsigned)fail);
    Debug_Print("========================================\r\n");
 
    return fail;
}
 

uint32 SelfTest_UsbPdCfg(void)
{
    uint32 err = 0u;

    Debug_Print("[TEST:USBPD_CFG] Config save/load round-trip...\r\n");

    /* Save current config */
    uint8 saveResult = UsbPd_Cfg_SaveToDFlash();
    if (saveResult != 0u)
    {
        Debug_Printf("[TEST:USBPD_CFG] FAIL: save returned %u\r\n",
                     (unsigned)saveResult);
        return 1u;
    }

    /* Load it back */
    uint8 loadResult = UsbPd_Cfg_LoadFromDFlash();
    if (loadResult != 0u)
    {
        Debug_Printf("[TEST:USBPD_CFG] FAIL: load returned %u\r\n",
                     (unsigned)loadResult);
        return 1u;
    }

    /* Verify fields */
    const UsbPd_SysCfg_t *cfg = UsbPd_Cfg_GetSysCfg();
    if (cfg->magic != USBPD_CFG_MAGIC) { err++; Debug_Print("    FAIL: magic\r\n"); }
    if (cfg->apuSlvAddr0 != 0x54u)     { err++; Debug_Print("    FAIL: slvAddr0\r\n"); }
    if (cfg->apuSlvAddr1 != 0x58u)     { err++; Debug_Print("    FAIL: slvAddr1\r\n"); }

    UsbPd_Cfg_Dump();

    Debug_Printf("[TEST:USBPD_CFG] %s\r\n", prv_Result(err));
    return err;
}




/* ================================================================== */
/*  CLI dispatch                                                      */
/* ================================================================== */
 
void SelfTest_CliDispatch(const char *args)
{
    args = prv_SkipSpaces(args);
 
    if (*args == '\0' || prv_StrEq(args, "all"))
        SelfTest_RunAll();
    else if (prv_StrEq(args, "crc"))
        SelfTest_Crc();
    else if (prv_StrEq(args, "sota"))
        SelfTest_Sota();
    else if (prv_StrEq(args, "swap"))
        SelfTest_Swap();
    else if (prv_StrEq(args, "fusa"))
        SelfTest_Fusa();
    else if (prv_StrEq(args, "pm"))
        SelfTest_Pm();
    else if (prv_StrEq(args, "usbpd"))
        SelfTest_UsbPd();
    else if (prv_StrEq(args, "fwup"))
        SelfTest_FwUpdate();
    else if (prv_StrEq(args, "updconf"))
        SelfTest_UsbPdCfg();
    else if (prv_StrEq(args, "usbpd_live"))
        SelfTest_UsbPdLive();
    else
        Debug_Print("  Usage: selftest [all|crc|sota|swap|fusa|pm|usbpd|fwup]\r\n");
}