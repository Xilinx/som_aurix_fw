/**
 * @file    Ipc.c
 * @brief   Inter-core communication — LMU mailboxes + IR signaling
 */

#include "Ipc.h"
#include "IfxSrc.h"
#include "IfxCpu_Irq.h"
#include <string.h>
#include "Stm_Timer.h"

/* ================================================================== */
/*  Shared memory instance (linker places in LMU SRAM)                */
/* ================================================================== */

volatile Ipc_SharedMem_t g_ipcShared __attribute__((section(".ipc_shared")));
volatile uint32 g_wdtOwner __attribute__((section(".ipc_shared"))) = 0u;
volatile Ipc_DbgRing_t g_dbgRing1 __attribute__((section(".ipc_shared")));
volatile Ipc_DbgRing_t g_dbgRing2 __attribute__((section(".ipc_shared")));
/* ================================================================== */
/*  IR service request nodes                                          */
/*                                                                    */
/*  The TC387 has general-purpose service request nodes               */
/*  (GPSR) that can be used for software-triggered interrupts.        */
/*  GPSR[coreId][channel] — we use channel 0 for each direction.      */
/* ================================================================== */

/* IR interrupt handler on CPU1: command from CPU0 */
IFX_INTERRUPT(ipcIrCpu1FromCpu0, IPC_CORE_PMC, IPC_IR_PRIO_CPU0_TO_CPU1)
{
    /* Clear the service request */
    /* The command is in g_ipcShared.cmd — CPU1's main loop processes it.
     * This ISR just ensures CPU1 wakes from any wait. */
}

/* IR interrupt handler on CPU0: status update from CPU1 */
IFX_INTERRUPT(ipcIrCpu0FromCpu1, IPC_CORE_ORCHESTRATOR, IPC_IR_PRIO_CPU1_TO_CPU0)
{
    /* CPU1 has updated PMC status — CPU0's main loop reads it. */
}

/* IR interrupt handler on CPU1: fault from CPU2 */
IFX_INTERRUPT(ipcIrCpu1FromCpu2, IPC_CORE_PMC, IPC_IR_PRIO_CPU2_TO_CPU1)
{
    /* CPU2 detected a fault — CPU1 reads g_ipcShared.fusa.lastFault
     * and takes immediate action in its main loop. */
}

/* IR interrupt handler on CPU0: fault from CPU2 */
IFX_INTERRUPT(ipcIrCpu0FromCpu2, IPC_CORE_ORCHESTRATOR, IPC_IR_PRIO_CPU2_TO_CPU0)
{
    /* CPU2 fault notification — CPU0 logs to NvLog. */
}

/* ================================================================== */
/*  Public API                                                        */
/* ================================================================== */

void Ipc_Init(void)
{
    /* Zero all shared memory */
    memset((void *)&g_ipcShared, 0, sizeof(g_ipcShared));

    /* Configure GPSR service request nodes for inter-core interrupts.
     * Each direction gets its own GPSR channel. */

    /* CPU0 → CPU1: GPSR[1].SR[0] targets CPU1 */
    {
        volatile Ifx_SRC_SRCR *src = &MODULE_SRC.GPSR.GPSR[1].SR[0];
        IfxSrc_init(src, IfxSrc_Tos_cpu1, IPC_IR_PRIO_CPU0_TO_CPU1);
        IfxSrc_enable(src);
    }

    /* CPU2 → CPU1: GPSR[1].SR[1] targets CPU1 */
    {
        volatile Ifx_SRC_SRCR *src = &MODULE_SRC.GPSR.GPSR[1].SR[1];
        IfxSrc_init(src, IfxSrc_Tos_cpu1, IPC_IR_PRIO_CPU2_TO_CPU1);
        IfxSrc_enable(src);
    }

    /* CPU1 → CPU0: GPSR[0].SR[0] targets CPU0 */
    {
        volatile Ifx_SRC_SRCR *src = &MODULE_SRC.GPSR.GPSR[0].SR[0];
        IfxSrc_init(src, IfxSrc_Tos_cpu0, IPC_IR_PRIO_CPU1_TO_CPU0);
        IfxSrc_enable(src);
    }

    /* CPU2 → CPU0: GPSR[0].SR[1] targets CPU0 */
    {
        volatile Ifx_SRC_SRCR *src = &MODULE_SRC.GPSR.GPSR[0].SR[1];
        IfxSrc_init(src, IfxSrc_Tos_cpu0, IPC_IR_PRIO_CPU2_TO_CPU0);
        IfxSrc_enable(src);
    }
    {
        g_wdtOwner      = 0u;
        g_dbgRing1.head = 0u;  
        g_dbgRing1.tail = 0u;
        g_dbgRing2.head = 0u;  
        g_dbgRing2.tail = 0u;
        __dsync();
    }
}

void Ipc_SendCommand(Ipc_Command_t cmd, uint32 param)
{
    g_ipcShared.cmd.command = cmd;
    g_ipcShared.cmd.param   = param;
    __dsync();  /* Ensure data is visible before signaling */
    g_ipcShared.cmd.seqNum++;
    __dsync();

    /* Fire IR to CPU1 */
    IfxSrc_setRequest(&MODULE_SRC.GPSR.GPSR[1].SR[0]);
}

boolean Ipc_SendCommandWait(Ipc_Command_t cmd, uint32 param, uint32 timeoutMs)
{
    uint32 startMs;

    if (!g_ipcShared.cpu1Ready)
        return FALSE;                 /* consumer not up — don't fire-and-lose */

    Ipc_SendCommand(cmd, param);

    startMs = Stm_GetTimeMs();
    while (!Ipc_IsCommandAcked())
    {
        if ((Stm_GetTimeMs() - startMs) > timeoutMs)
            return FALSE;
    }
    return TRUE;
}

boolean Ipc_IsCommandAcked(void)
{
    return (g_ipcShared.cmd.ackNum == g_ipcShared.cmd.seqNum);
}

void Ipc_SignalFault(Ipc_FaultCode_t code, uint32 channel, uint32 mv)
{
    g_ipcShared.fusa.faultCode    = (uint32)code;
    g_ipcShared.fusa.faultChannel = channel;
    g_ipcShared.fusa.faultMv      = mv;
    g_ipcShared.fusa.faultActive  = 1u;
    __dsync();
    g_ipcShared.fusa.faultSeq++;
    __dsync();
}

void Ipc_ClearFaultActive(void)            /* CPU2 only */
{
    g_ipcShared.fusa.faultActive = 0u;
    __dsync();
}

void Ipc_UpdatePmcStatus(uint32 pmState, uint32 resetCause,
                         uint32 retryCount, sint32 apuTempC,
                         uint32 prochotActive)
{
    g_ipcShared.pmc.pmState       = pmState;
    g_ipcShared.pmc.resetCause    = resetCause;
    g_ipcShared.pmc.retryCount    = retryCount;
    g_ipcShared.pmc.apuTempC      = apuTempC;
    g_ipcShared.pmc.prochotActive = prochotActive;
    g_ipcShared.pmc.updateCounter++;
    __dsync();
}

void Ipc_UpdateVoltages(const uint16 *pChannelMv, uint32 uvFlags,
                        uint32 ovFlags)
{
    uint32 i;
    for (i = 0u; i < 24u; i++)
    {
        g_ipcShared.fusa.channelMv[i] = pChannelMv[i];
    }
    g_ipcShared.fusa.uvFaultFlags = uvFlags;
    g_ipcShared.fusa.ovFaultFlags = ovFlags;
    g_ipcShared.fusa.updateCounter++;
    __dsync();
}

void Ipc_UpdateTlfStatus(uint32 devstat, uint32 syssf, uint32 wdstat,
                         uint32 state, uint32 wdtSvc, uint32 wdtMiss,
                         uint32 errPin, uint32 ssPin)
{
    g_ipcShared.fusa.tlfDevstat = devstat;
    g_ipcShared.fusa.tlfSyssf   = syssf;
    g_ipcShared.fusa.tlfWdstat  = wdstat;
    g_ipcShared.fusa.tlfState   = state;
    g_ipcShared.fusa.tlfWdtSvc  = wdtSvc;
    g_ipcShared.fusa.tlfWdtMiss = wdtMiss;
    g_ipcShared.fusa.tlfErrPin  = errPin;
    g_ipcShared.fusa.tlfSsPin   = ssPin;
    __dsync();
}

void Ipc_SignalWarning(uint32 channel, uint32 mv)
{
    g_ipcShared.fusa.warnChannel = channel;
    g_ipcShared.fusa.warnMv      = mv;
    __dsync();
    g_ipcShared.fusa.warnSeq++;
    __dsync();
}