/**
 * @file    Cpu1_Main.c
 * @brief   CPU1 — Power Management Controller core
 *
 * Owns: PowerManager, SysMonitor, ComHpcWdt, UsbPd, ERU callbacks
 * Reads: g_ipcShared.cmd (commands from CPU0)
 *        g_ipcShared.fusa (voltage/PMIC faults from CPU2)
 * Writes: g_ipcShared.pmc (PM state, temp, PROCHOT)
 */

#include "Ifx_Types.h"
#include "IfxCpu.h"
#include "IfxScuWdt.h"
#include "Ipc.h"
#include "Stm_Timer.h"
#include "Uart_Debug.h"
#include "PowerManager.h"
#include "Eru_FaultIsr.h"
#include "Platform_Cfg.h"
#include "SysMonitor.h"
#include "ComHpcWdt.h"
#include "UsbPd_Manager.h"
#include "UsbPd_Hpd.h"
#include "UsbPd_ApuProxy.h"
#include "I2c_Slave.h"
#include "UsbPd_Cfg.h"

extern IfxCpu_syncEvent g_cpuSyncEvent;

/* ================================================================== */
/*  IPC command processing                                            */
/* ================================================================== */

static uint32 s_lastAckedSeq = 0u;
static uint32 s_lastActedFaultSeq = 0u;

static void prv_ProcessCommands(void)
{
    uint32 seq = g_ipcShared.cmd.seqNum;
    if (seq == s_lastAckedSeq)
        return;

    Ipc_Command_t cmd = g_ipcShared.cmd.command;

    switch (cmd)
    {
        case IPC_CMD_POWER_ON:      PowerManager_RequestPowerOn();    break;
        case IPC_CMD_POWER_OFF:     PowerManager_RequestPowerOff();   break;
        case IPC_CMD_FORCED_OFF:    PowerManager_RequestForcedOff();  break;
        case IPC_CMD_WARM_RESET:    PowerManager_RequestWarmReset();  break;
        case IPC_CMD_COLD_RESET:    PowerManager_RequestColdReset();  break;
        case IPC_CMD_CLEAR_FAULT:   PowerManager_ClearFault();        break;
        default:                                                      break;
    }

    s_lastAckedSeq = seq;
    g_ipcShared.cmd.ackNum = seq;
    __dsync();
}

static void prv_PublishStatus(void)
{
    Ipc_UpdatePmcStatus(
        (uint32)PowerManager_GetState(),
        (uint32)PowerManager_GetResetCause(),
        (uint32)PowerManager_GetRetryCount(),
        0,
        0u
    );
#if !defined(TARGET_EVAL_BOARD)
    g_ipcShared.pmc.prochotActive = (uint32)SysMonitor_IsThrottling();
#endif
}

static void prv_CheckFusaFaults(void)
{
    uint32 seq = g_ipcShared.fusa.faultSeq;
    if (seq == s_lastActedFaultSeq)
        return;
    s_lastActedFaultSeq = seq;

    switch ((Ipc_FaultCode_t)g_ipcShared.fusa.faultCode)
    {
        case IPC_FAULT_UV:
        case IPC_FAULT_OV:
            if (PowerManager_GetState() == PM_STATE_ON)
                PowerManager_RequestForcedOff();
            break;
        case IPC_FAULT_PMIC:
        case IPC_FAULT_PMIC_SS:
            PowerManager_RequestForcedOff();
            break;
        default:
            break;
    }
}

/* ================================================================== */
/*  CPU1 entry point                                                  */
/* ================================================================== */

void core1_main(void)
{
    *(volatile uint32 *)0xB0050000u = 0xC1C1C1C1u;
    IfxCpu_enableInterrupts();
    IfxScuWdt_disableCpuWatchdog(IfxScuWdt_getCpuWatchdogPassword());

    IfxCpu_emitEvent(&g_cpuSyncEvent);
    while ((g_cpuSyncEvent & CORE_SYNC_MASK) != CORE_SYNC_MASK){}

    /* ---- CPU1 init ---- */
    Debug_Print("[CPU1] Init: PowerManager...\r\n");
    PowerManager_Init();
    Eru_RegisterCallback(ERU_CB_THERMTRIP, PowerManager_OnThermtripIsr);

#if !defined(TARGET_EVAL_BOARD)
    SysMonitor_Init();
    SysMonitor_RegisterShutdownCb(PowerManager_OnThermtripIsr);
    Debug_Print("[CPU1] Init: SysMonitor OK\r\n");

#if (SYSMON_CARRIER_WD_ENABLE == 1u)
    Eru_RegisterCallback(ERU_CB_WD_STROBE, ComHpcWdt_OnStrobeIsr);
    ComHpcWdt_Init();
    Debug_Print("[CPU1] Init: ComHpcWdt OK\r\n");
#endif

#if (USBPD_FEATURE_ENABLE == 1u)
    I2cSlave_Init(NULL_PTR);
    UsbPd_Hpd_Init();
    UsbPd_ApuProxy_Init();
    UsbPdManager_Init();
    Debug_Print("[CPU1] Init: UsbPd OK\r\n");
#endif
#endif

    s_lastAckedSeq = g_ipcShared.cmd.seqNum;
    Debug_Print("[CPU1] Ready\r\n");
    g_ipcShared.cpu1Ready = TRUE;
    __dsync();
    /* ---- CPU1 main loop ---- */
    for (;;)
    {
        uint32 loopStartMs = Stm_GetTimeMs();

        prv_ProcessCommands();
        prv_CheckFusaFaults();
        PowerManager_Run();

#if !defined(TARGET_EVAL_BOARD)
        UsbPd_Hpd_Run();

#if (SYSMON_CARRIER_WD_ENABLE == 1u)
        ComHpcWdt_Run();
#endif

        if (PowerManager_GetState() == PM_STATE_ON)
        {
            SysMonitor_Run();
#if (USBPD_FEATURE_ENABLE == 1u)
            UsbPdManager_Run();
#endif
        }
#endif

        prv_PublishStatus();

        {
            uint32 elapsed = Stm_GetTimeMs() - loopStartMs;
            if (elapsed < MAIN_LOOP_PERIOD_MS)
                Stm_DelayMs(MAIN_LOOP_PERIOD_MS - elapsed);
        }
    }
}