/**
 * @file    Cpu2_Main.c
 * @brief   CPU2 — Functional Safety core
 *
 * Owns: EVADC (VoltMon), QSPI2 (TLF35585), QSPI3 (FuSa SPI slave)
 * Writes: g_ipcShared.fusa (voltages, PMIC status, fault codes)
 * Reads: g_ipcShared.pmc.pmState (to know when rails are live)
 *
 * Design: minimal shared data flow, CPU2 writes only, others read.
 * Peripheral access is exclusive — no other core touches EVADC/QSPI2/QSPI3.
 */

#include "Ifx_Types.h"
#include "IfxCpu.h"
#include "IfxScuWdt.h"
#include "IfxPort.h"
#include "Ipc.h"
#include "Stm_Timer.h"
#include "Uart_Debug.h"
#include "VoltMon.h"
#include "Tlf35585.h"
#include "FusaSpi.h"
#include "Platform_Cfg.h"
#include "Platform_PinCfg.h"
#include "AppPin.h"

extern IfxCpu_syncEvent g_cpuSyncEvent;

/* FuSa loop runs at 100 Hz — tighter than PMC */
#ifndef FUSA_LOOP_PERIOD_MS
#define FUSA_LOOP_PERIOD_MS     10u
#endif

/* ================================================================== */
/*  VoltMon fault callback (runs on CPU2)                             */
/* ================================================================== */

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

/* ================================================================== */
/*  TLF fault callback (runs on CPU2)                                 */
/* ================================================================== */

static void prv_OnTlfFault(void)
{
    Ipc_SignalFault(IPC_FAULT_PMIC, 0u, 0u);
}

/* ================================================================== */
/*  Publish data to shared memory                                     */
/* ================================================================== */

static void prv_PublishVoltages(void)
{
    uint16 chMv[VOLTMON_MAX_CHANNELS];
    uint32 i;

    for (i = 0u; i < VOLTMON_MAX_CHANNELS; i++)
        chMv[i] = VoltMon_GetLastMv(i);

    /* TODO: build UV/OV bitmasks from VoltMon fault state */
    Ipc_UpdateVoltages(chMv, 0u, 0u);
}

static void prv_PublishTlfStatus(void)
{
    Ipc_UpdateTlfStatus(
        (uint32)Tlf35585_GetDevState(),
        0u,     /* SYSSF — expose via accessor when available */
        0u,     /* WDSTAT */
        (uint32)Tlf35585_GetDevState(),
        0u,     /* WDT svc count */
        0u,     /* WDT miss count */
        (uint32)Tlf35585_IsErrActive(),
        (uint32)Tlf35585_IsSafeStateActive()
    );
}

/* ================================================================== */
/*  CPU2 entry point                                                  */
/* ================================================================== */

void core2_main(void)
{
    *(volatile uint32 *)0xB0050004u = 0xC2C2C2C2u;
    IfxCpu_enableInterrupts();
    IfxScuWdt_disableCpuWatchdog(IfxScuWdt_getCpuWatchdogPassword());

    IfxCpu_emitEvent(&g_cpuSyncEvent);
        while ((g_cpuSyncEvent & CORE_SYNC_MASK) != CORE_SYNC_MASK){}

    /* ---- CPU2 init ---- */

    /* VoltMon — EVADC owned by CPU2 */
    Debug_Print("[CPU2] Init: VoltMon...\r\n");
    VoltMon_Init();
    VoltMon_RegisterFaultCb(prv_OnVoltageFault);

    /* TLF35585 — QSPI2 owned by CPU2.
     * EarlyInit + Init already ran on CPU0 before sync barrier.
     * CPU2 takes over WDT service and fault checks from here. */
    Tlf35585_RegisterFaultCb(prv_OnTlfFault);
    Debug_Print("[CPU2] Init: TLF service takeover\r\n");

    /* FuSa SPI slave — QSPI3 owned by CPU2 */
    FusaSpi_Init();
    Debug_Print("[CPU2] Init: FusaSpi OK\r\n");
    Debug_Print("[CPU2] Ready\r\n");
    __dsync();
    g_ipcShared.cpu2Ready = TRUE;      /* flag set only after all init prints queued */
    __dsync();
    while (g_wdtOwner != 2u){}
    Tlf35585_EnableIsrMode();

    /* ---- CPU2 main loop (100 Hz) ---- */
    for (;;)
    {
        uint32 loopStartMs = Stm_GetTimeMs();

        /* TLF watchdog — must run on the core that owns QSPI2 */
        Tlf35585_ServiceWdt();
        Tlf35585_CheckFaults();

        /* Scan all voltage channels */
        VoltMon_Scan();

        /* Update FuSa SPI register map */
        FusaSpi_Update();

        /* Publish to shared memory */
        prv_PublishVoltages();
        prv_PublishTlfStatus();

        /* Clear FUSA_VOLTAGE_ERR if all channels OK */
        {
            //boolean anyFault = FALSE;
            uint32 ch;
            for (ch = 0u; ch < VOLTMON_MAX_CHANNELS; ch++)
            {
                uint16 mv = VoltMon_GetLastMv(ch);
                if (mv != 0u)
                {
                    /* Non-zero means channel is configured and has a reading.
                     * The actual fault check is in VoltMon_Scan/prv_CheckThresholds
                     * which calls prv_OnVoltageFault above. We just check the
                     * IPC fault flag here for the GPIO clear. */
                }
            }
            if (!VoltMon_AnyFaultActive() && !Tlf35585_IsErrActive())
            {
                IfxPort_setPinHigh(
                    AppPin_GetPort(PIN_FUSA_VOLTAGE_ERR_L.portIdx),
                    PIN_FUSA_VOLTAGE_ERR_L.pinIdx);
                FusaSpi_DeassertAlert();
                Ipc_ClearFaultActive();
            }
        }

        /* Loop pacing — 100 Hz */
        {
            uint32 elapsed = Stm_GetTimeMs() - loopStartMs;
            if (elapsed < FUSA_LOOP_PERIOD_MS)
                Stm_DelayMs(FUSA_LOOP_PERIOD_MS - elapsed);
        }

        {
            static uint32 s_lastSnapMs = 0u;
            if ((Stm_GetTimeMs() - s_lastSnapMs) >= 1000u)
            {
                s_lastSnapMs = Stm_GetTimeMs();
                Tlf35585_PublishRegSnapshot();
            }
        }
    }
}

void core3_main(void)
{
    *(volatile uint32 *)0xB0050008u = 0xC3C3C3C3u;
    IfxScuWdt_disableCpuWatchdog(IfxScuWdt_getCpuWatchdogPassword());
    IfxCpu_emitEvent(&g_cpuSyncEvent);
    Debug_Printf("[SYS] canaries: %08X %08X %08X\r\n",
             *(volatile uint32 *)0xB0050000u,
             *(volatile uint32 *)0xB0050004u,
             *(volatile uint32 *)0xB0050008u);
    while ((g_cpuSyncEvent & CORE_SYNC_MASK) != CORE_SYNC_MASK){}
    for (;;) { __nop(); }
}


