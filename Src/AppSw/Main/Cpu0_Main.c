/* ================================================================== */
/*  Cpu0_Main.c — CPU0: startup, orchestration, OTA, NV, CLI          */
/*                                                                    */
/*  Refactor notes (no behavior change intended except as listed):    */
/*    - DFlash/PFlash self-tests moved to SelfTest.c                  */
/*    - Phase 3 wait/handover and main-loop fault forwarding pulled   */
/*      into prv_ helpers; block-scoped statics became file-scoped    */
/*    - Duplicate "All cores running" print removed                   */
/*    - Bare Tlf35585_ServiceWdt() calls after Ipc_Init() are now     */
/*      guarded on g_wdtOwner == 0 (ownership rule made mechanical).  */
/*      Calls BEFORE Ipc_Init() (Phase 0, recovery mode) stay         */
/*      unconditional: g_wdtOwner lives in .ipc_shared (NOLOAD) and   */
/*      is uninitialized SRAM until Ipc_Init() runs.                  */
/*    - prv_ForwardTlfEvents() now also prints the event line (the    */
/*      CPU2-side print in Tlf35585_LogEvent should be deleted)       */
/*    - Pruned includes for modules that now live on CPU1/CPU2        */
/*      (VoltMon, FusaSpi, Eru_FaultIsr, Crc32, SysMonitor, UsbPd,    */
/*      I2c_Slave, ComHpcWdt) — re-add any the compiler asks for      */
/* ================================================================== */
#include "Ifx_Types.h"
#include "IfxCpu.h"
#include "IfxScuWdt.h"
#include "Ipc.h"
#include "Clk_Cfg.h"
#include "Port_Init.h"
#include "Stm_Timer.h"
#include "Uart_Debug.h"
#include "I2c_Master.h"
#include "PowerManager.h"
#include "Platform_Cfg.h"
#include "Tlf35585.h"
#include "DFlash.h"
#include "BootValid.h"
#include "PFlash.h"
#include "Swap.h"
#include "NvLog.h"
#include "Uart_Xfer.h"
#include "FwUpdate.h"
#include "BiosRom.h"
#include "Bist.h"
#include "DebugCli.h"
#include "Platform_PinCfg.h"
#include "SysMonitor.h"
#include "SelfTest.h"


/* ================================================================== */
/*  Multicore sync                                                    */
/* ================================================================== */
IFX_ALIGN(4) IfxCpu_syncEvent g_cpuSyncEvent __attribute__((section(".ipc_shared"))) = 0;  

/* Rendezvous barrier + report. Judged on the sync word itself, not
 * the iLLD boolean (its sense is inverted in this iLLD version). */
static void prv_SyncBarrier(void)
{
    uint32 sync;

    IfxCpu_emitEvent(&g_cpuSyncEvent);
    (void)IfxCpu_waitEvent(&g_cpuSyncEvent, 100);

    sync = g_cpuSyncEvent;
    Debug_Printf("[SYS] barrier %s, sync=0x%X\r\n",
                 ((sync & 0xFu) == 0xFu) ? "OK" : "INCOMPLETE",
                 (unsigned)sync);
}

/* ================================================================== */
/*  Firmware version string                                           */
/* ================================================================== */
#if defined(TARGET_EVAL_BOARD)
#define FW_VERSION_STR  "TC387 COM-HPC Controller v0.2 [EVAL BOARD] [MULTICORE]\r\n"
#else
#define FW_VERSION_STR  "TC387 COM-HPC Controller v0.2 [MULTICORE]\r\n"
#endif



/* ================================================================== */
/*  CPU0 entry point                                                  */
/* ================================================================== */
int core0_main(void)
{
    IfxCpu_enableInterrupts();
    IfxScuWdt_disableCpuWatchdog(IfxScuWdt_getCpuWatchdogPassword());
    IfxScuWdt_disableSafetyWatchdog(IfxScuWdt_getSafetyWatchdogPassword());

    /* ============================================================== */
    /*  Phase 0: Hardware primitives + TLF race (CPU0 only)           */
    /*  NOTE: g_wdtOwner is NOT valid yet (init'd in Ipc_Init) —      */
    /*  all TLF servicing in this phase is unconditional.             */
    /* ============================================================== */
    Clk_Init();
    Port_Init();
    Stm_Init();
    Tlf35585_EarlyInit();
    Debug_Init();
    Debug_Print("\r\n" FW_VERSION_STR);
    Debug_Print("[SYS] Init: UART OK\r\n");
    UartXfer_Init();
    Debug_Print("[SYS] Init: Side UART OK\r\n");
    I2cMaster_Init();
    Debug_Print("[SYS] Init: I2C OK\r\n");

    /* TLF full init — still on CPU0 before cores are released.
     * CPU2 takes over WDT service after the handover in Phase 3. */
    if (Tlf35585_Init() != TLF_OK)
    {
        Debug_Print("[SYS] Init: TLF FAILED\r\n");
    }
    else
    {
        Debug_Print("[SYS] Init: TLF OK\r\n");
    }
    Tlf35585_RegisterFaultCb(PowerManager_RequestPowerOff);
    Debug_Print("[SYS] Init: TLF OK\r\n");

    /* ============================================================== */
    /*  Phase 1: BIOS ROM + IPC init (before core release)            */
    /* ============================================================== */
    BiosRom_Init();
    Debug_Print("[SYS] Init: BIOS ROM bus released\r\n");
    Ipc_Init();                          /* g_wdtOwner valid from here on */
    Debug_Print("[SYS] Init: IPC shared memory OK\r\n");

    /* ============================================================== */
    /*  Phase 2: Flash + NV + SOTA (CPU0 owned)                       */
    /* ============================================================== */
    DFlash_Init();
    Debug_Print("[SYS] Init: DFlash OK\r\n");
    if (g_wdtOwner == 0u) Tlf35585_ServiceWdt();
    NvLog_Init();
    if (g_wdtOwner == 0u) Tlf35585_ServiceWdt();
    Debug_Print("[SYS] Init: NvLog OK\r\n");

#if defined(TARGET_EVAL_BOARD)
    SelfTest_DFlash();
#endif

    {
        BootValid_Status_t bootStatus = BootValid_CheckOnStartup();
        Debug_Printf("[SYS] Init: BootValid = %u\r\n", (unsigned)bootStatus);
    }

    PFlash_Init();
    PFlash_RegisterKeepAliveCb(Tlf35585_ServiceWdt);
    Debug_Print("[SYS] Init: PFlash OK\r\n");
    Debug_Printf("[SYS] Active bank: 0x%02X\r\n", (unsigned)(Swap_GetCurrentBank()));
    FwUpdate_Init();
    Bist_RunPost(Tlf35585_ServiceWdt);
    Debug_Print("[SYS] Init: POST complete\r\n");

#if defined(TARGET_EVAL_BOARD)
    SelfTest_PFlash();
#endif

    /* ============================================================== */
    /*  Phase 3: Release CPU1/CPU2, wait, hand over the TLF WDT       */
    /* ============================================================== */
    prv_SyncBarrier();

    if (prv_WaitForCores(5000u))
    {
        prv_HandoverTlfWdt();
    }
    /* On timeout: CPU0 keeps WDT ownership; the main loop below
     * continues servicing (g_wdtOwner still 0). */

    /* ============================================================== */
    /*  Phase 4: Debug CLI + main loop                                */
    /* ============================================================== */
    Debug_DrainRings();                  /* flush tail of core init logs */
    Debug_Print("[SYS] CPU0 entering main loop\r\n");
    DebugCli_Init();

    for (;;)
    {
        uint32 loopStartMs = Stm_GetTimeMs();

        NvLog_Run();
        FwUpdate_Run();
        Bist_Run(NULL_PTR);              /* keep-alive not needed: either
                                          * CPU2 owns the WDT, or the line
                                          * below services it */
        if (g_wdtOwner == 0u)
        {
            Tlf35585_ServiceWdt();       /* only if CPU2 never came up */
        }
        Debug_DrainRings();
        DebugCli_Run();
        DebugCli_Poll();
        prv_CommitSotaOnce();
        prv_ForwardVoltageFaults();
        prv_ForwardVoltageWarnings();
        prv_ForwardTlfEvents();

        prv_PaceLoop(loopStartMs);
    }

    return 0;
}