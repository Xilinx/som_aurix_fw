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
#include "Platform_PinCfg.h"
#include "Eru_FaultIsr.h"
#include "ComHpcWdt.h"
#include "SysMonitor.h"
#include "SelfTest.h"
#include "VoltMon.h"
#include "FusaSpi.h"
#include "IfxPort.h"
#include "AppPin.h"

static boolean s_fusaReady = FALSE;

/* ================================================================== */
/*  Multicore sync                                                    */
/* ================================================================== */
/* Rendezvous barrier + report. Judged on the sync word itself, not
 * the iLLD boolean (its sense is inverted in this iLLD version).
 * Ipc_ReleaseAndWaitBarrier() publishes CORE_SYNC_GO so CPU1/2/3 do
 * not treat a leftover 0xF in NOLOAD LMU as "all cores arrived". */
static void prv_SyncBarrier(void)
{
    uint32 sync;

    Ipc_ReleaseAndWaitBarrier();

    sync = (uint32)g_cpuSyncEvent;
    Debug_Printf("[SYS] barrier %s, sync=0x%X\r\n",
                 ((sync & 0xFu) == 0xFu) ? "OK" : "INCOMPLETE",
                 (unsigned)sync);
}

static void prv_FlashKeepAlive(void)
{
    if (g_wdtOwner == 0u) { Tlf35585_ServiceWdt(); }
}

static void prv_OnVoltageFault(const VoltMon_ChCfg_t *ch,
                               uint16 measuredMv,
                               VoltMon_Severity_t severity)
{
    if (severity >= VOLTMON_FAULT)
    {
        IfxPort_setPinLow(AppPin_GetPort(PIN_FUSA_VOLTAGE_ERR_L.portIdx),
                          PIN_FUSA_VOLTAGE_ERR_L.pinIdx);
        FusaSpi_AssertAlert();

        Ipc_FaultCode_t code = (measuredMv < ch->uvFaultMv)
                               ? IPC_FAULT_UV : IPC_FAULT_OV;
        Ipc_SignalFault(code, (uint32)ch->evadcChannel, (uint32)measuredMv);
    }
}

static void prv_OnTlfFault(void)
{
    Ipc_SignalFault(IPC_FAULT_PMIC, 0u, 0u);
}

static void prv_PublishVoltages(void)
{
    uint16 chMv[VOLTMON_MAX_CHANNELS];
    uint32 i;

    for (i = 0u; i < VOLTMON_MAX_CHANNELS; i++)
    {
        chMv[i] = VoltMon_GetLastMv(i);
    }
    Ipc_UpdateVoltages(chMv, 0u, 0u);
}

static void prv_PublishTlfStatus(void)
{
    Ipc_UpdateTlfStatus(
        (uint32)Tlf35585_GetDevState(),
        0u,
        0u,
        (uint32)Tlf35585_GetDevState(),
        0u,
        0u,
        (uint32)Tlf35585_IsErrActive(),
        (uint32)Tlf35585_IsSafeStateActive()
    );
}

static void prv_InitFusa(void)
{
    Debug_Print("[CPU0] Init: VoltMon...\r\n");
    VoltMon_Init();
    VoltMon_RegisterFaultCb(prv_OnVoltageFault);

    /* Replaces the boot callback. CPU1 turns a PMIC IPC fault into
     * a forced power-off, which is the path that was live on CPU2. */
    Tlf35585_RegisterFaultCb(prv_OnTlfFault);
    Tlf35585_EnableIsrMode();
    Debug_Print("[CPU0] Init: TLF service stays here\r\n");

    FusaSpi_Init();
    Debug_Print("[CPU0] Init: FusaSpi OK\r\n");
    s_fusaReady = TRUE;
}

static void prv_RunFusa(void)
{
    static uint32 s_lastSnapMs = 0u;

    Tlf35585_ServiceWdt();
    Tlf35585_CheckFaults();
    VoltMon_Scan();
    FusaSpi_Update();
    prv_PublishVoltages();
    prv_PublishTlfStatus();

    if (!VoltMon_AnyFaultActive() && !Tlf35585_IsErrActive())
    {
        IfxPort_setPinHigh(
            AppPin_GetPort(PIN_FUSA_VOLTAGE_ERR_L.portIdx),
            PIN_FUSA_VOLTAGE_ERR_L.pinIdx);
        FusaSpi_DeassertAlert();
        Ipc_ClearFaultActive();
    }

    if ((Stm_GetTimeMs() - s_lastSnapMs) >= 1000u)
    {
        s_lastSnapMs = Stm_GetTimeMs();
        Tlf35585_PublishRegSnapshot();
    }
}

/* ================================================================== */
/*  Firmware version string                                           */
/* ================================================================== */
#if defined(TARGET_EVAL_BOARD)
#define FW_VERSION_STR  "TC387 COM-HPC Controller v0.3 [EVAL BOARD] [MULTICORE]\r\n"
#else
#define FW_VERSION_STR  "TC387 COM-HPC Controller v0.3 [MULTICORE]\r\n"
#endif



/* ================================================================== */
/*  CPU0 entry point                                                  */
/* ================================================================== */
int core0_main(void)
{
    uint32 prevSync = Ipc_ResetCoreBarrier();   /* before CPU1/2/3 sample leftover 0xF */

    g_prevBootTrace = g_bootTrace;                              /* keep last boot's record */
    g_bootTrace     = 0xB0000000u | (SCU_SWAPCTRL.U & 0xFFu);
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
     g_bootTrace |= 0x2000u;
    Debug_Print("[SYS] Init: UART OK\r\n");
    Debug_Printf("[SYS] Core sync: leftover event=0x%X (%s)\r\n",
                 (unsigned)prevSync,
                 (prevSync == 0u) ? "power-on or already clear"
                                  : "cleared; was retained across system reset");
    UartXfer_Init();
    Debug_Print("[SYS] Init: Side UART OK\r\n");
    I2cMaster_Init();
    Debug_Print("[SYS] Init: I2C OK\r\n");

    /* TLF full init — CPU0 keeps the window watchdog for the whole boot. */
    if (Tlf35585_Init() != TLF_OK)
    {
        Debug_Print("[SYS] Init: TLF FAILED\r\n");
    }
    else
    {
        Debug_Print("[SYS] Init: TLF OK\r\n");
    }
    Tlf35585_RegisterFaultCb(PowerManager_RequestPowerOff);

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
    //SelfTest_DFlash();

    PFlash_Init();
    PFlash_RegisterKeepAliveCb(prv_FlashKeepAlive);
    Debug_Print("[SYS] Init: PFlash OK\r\n");
    Debug_Printf("[SYS] Active bank: 0x%02X\r\n", (unsigned)(Swap_GetCurrentBank()));
    FwUpdate_Init();
    //SelfTest_PFlash();
    BootValid_Status_t bootStatus = BootValid_CheckOnStartup();
    Debug_Printf("[SYS] Init: BootValid = %u\r\n", (unsigned)bootStatus);
    prv_SyncBarrier();
    if (prv_WaitForCores(5000u))
    {
        prv_InitFusa();
        /* QSPI2 TLF IRQ is already TOS CPU0, the watchdog owner.
         * PFlash erase runs on CPU2, so this loop keeps servicing
         * the window while a sector erase has CPU2's interrupts off. */
        ComHpcWdt_Init();
        Eru_RegisterCallback(ERU_CB_WD_STROBE, ComHpcWdt_OnStrobeIsr);
        Eru_FaultIsr_Init();
        Debug_Print("[SYS] All cores running\r\n");
        Debug_Print("[SYS] TLF WDT and host WDT stay on CPU0\r\n");
    }
    if (bootStatus == BOOTVALID_PENDING)
    {
        Bist_RunPost(prv_FlashKeepAlive);          /* only a freshly OTA'd, contiguous image */
    }
    else
    {
        Bist_SetSkipped();  /* status shows SKIP */
        Debug_Print("[BIST] POST skipped (no pending update)\r\n");
    }
    Debug_Print("[SYS] Init: POST complete\r\n");
    /* On core-init timeout: g_wdtOwner stays 0 and the loop below
     * keeps servicing the TLF. FuSa init is skipped. */

    /* ============================================================== */
    /*  Phase 4: Debug CLI + main loop                                */
    /* ============================================================== */
    Debug_DrainRings();                  /* flush tail of core init logs */
    Debug_Print("[SYS] CPU0 entering main loop\r\n");

    for (;;)
    {
        uint32 loopStartMs = Stm_GetTimeMs();

        /* CPU2 asked for the debug UART. Flush, then stop touching it. */
        if (g_ipcShared.uartOwner == 1u)
        {
            Debug_FlushBlocking();
            g_ipcShared.uartOwner = 2u;
            __dsync();
        }

        if (s_fusaReady)
        {
            prv_RunFusa();
        }
        else if (g_wdtOwner == 0u)
        {
            Tlf35585_ServiceWdt();
        }

        ComHpcWdt_Run();

        if (g_ipcShared.uartOwner < 2u)
        {
            Debug_DrainRings();
        }

        prv_PaceLoop(loopStartMs);
    }

    return 0;
}