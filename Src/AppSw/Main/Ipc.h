/**
 * @file    Ipc.h
 * @brief   Inter-core communication infrastructure for TC387
 *
 * Multicore partitioning:
 *   CPU0 → Startup, OTA update, NV storage, config, boot orchestration,
 *          Debug CLI, UART
 *   CPU1 → PMC: PowerManager, SysMonitor, UsbPd, ComHpcWdt, I2C0/I2C1
 *   CPU2 → FuSa: VoltMon (EVADC), TLF35585 (QSPI2), FusaSpi (QSPI3),
 *          FUSA GPIO/Alert
 *
 * Shared data lives in LMU SRAM (0xB0000000, accessible from all cores
 * via the SRI crossbar).  Each field has a single writer (producer)
 * and one or more readers (consumers) — no locking needed for
 * naturally-aligned 32-bit writes (TriCore guarantees atomic word
 * writes to LMU).
 *
 * Commands use a sequence-number handshake:
 *   1. CPU0 writes command + increments seqNum
 *   2. CPU0 fires IR interrupt to CPU1
 *   3. CPU1 ISR reads command, processes it, writes ackNum = seqNum
 *
 * Peripheral ownership (each peripheral assigned to exactly one core):
 *   CPU0: ASCLIN0 (debug UART), ASCLIN1/4 (transfer UART), DFlash, PFlash
 *   CPU1: I2C0 (USB PD), I2C1 (APML), ASCLIN for APU sideband
 *   CPU2: EVADC (FuSa groups), QSPI2 (TLF35585), QSPI3 (FuSa SPI slave)
 */

#ifndef IPC_H
#define IPC_H

#include "Ifx_Types.h"
#include "IfxCpu.h"


/* ================================================================== */
/*  Core indices                                                      */
/* ================================================================== */

#define IPC_CORE_ORCHESTRATOR   0u      /* CPU0 */
#define IPC_CORE_PMC            1u      /* CPU1 */
#define IPC_CORE_FUSA           2u      /* CPU2 */

/* ================================================================== */
/*  IR interrupt priorities (inter-core signaling)                    */
/* ================================================================== */

#define IPC_IR_PRIO_CPU0_TO_CPU1    60u
#define IPC_IR_PRIO_CPU0_TO_CPU2    61u
#define IPC_IR_PRIO_CPU1_TO_CPU0    62u
#define IPC_IR_PRIO_CPU1_TO_CPU2    63u
#define IPC_IR_PRIO_CPU2_TO_CPU0    64u
#define IPC_IR_PRIO_CPU2_TO_CPU1    65u

#define CORE_SYNC_MASK        0xFu      /* CPU0..3 all participate     */
#define CORE_SYNC_TIMEOUT_MS  30000u    /* > worst-case CPU0 Phase 0-2 */

#define DBGRING_SIZE  4096u

/* ================================================================== */
/*  Command codes (CPU0 → CPU1)                                       */
/* ================================================================== */

typedef enum
{
    IPC_CMD_NONE            = 0u,
    IPC_CMD_POWER_ON        = 1u,
    IPC_CMD_POWER_OFF       = 2u,
    IPC_CMD_FORCED_OFF      = 3u,
    IPC_CMD_WARM_RESET      = 4u,
    IPC_CMD_COLD_RESET      = 5u,
    IPC_CMD_CLEAR_FAULT     = 6u,
    IPC_CMD_CONFIG_UPDATE   = 7u,
} Ipc_Command_t;

/* ================================================================== */
/*  Fault codes (CPU2 → CPU1, CPU2 → CPU0)                           */
/* ================================================================== */

typedef enum
{
    IPC_FAULT_NONE          = 0u,
    IPC_FAULT_UV            = 1u,       /* Undervoltage */
    IPC_FAULT_OV            = 2u,       /* Overvoltage */
    IPC_FAULT_PMIC          = 3u,       /* TLF PMIC fault */
    IPC_FAULT_PMIC_WDT      = 4u,       /* TLF WDT miss */
    IPC_FAULT_PMIC_SS       = 5u,       /* TLF safe state */
} Ipc_FaultCode_t;

/* ================================================================== */
/*  Shared mailbox: CPU0 → CPU1 (commands)                            */
/*  Writer: CPU0 only.  Reader: CPU1 only.                            */
/* ================================================================== */

typedef struct
{
    volatile uint32         seqNum;     /**< Incremented by CPU0 on each command */
    volatile uint32         ackNum;     /**< Set to seqNum by CPU1 when processed */
    volatile Ipc_Command_t  command;    /**< Current command */
    volatile uint32         param;      /**< Command-specific parameter */
} Ipc_CmdMailbox_t;

/* ================================================================== */
/*  Shared mailbox: CPU1 → all (PM status)                            */
/*  Writer: CPU1 only.  Readers: CPU0, CPU2.                          */
/* ================================================================== */

typedef struct
{
    volatile uint32 pmState;            /**< PM_State_t */
    volatile uint32 resetCause;         /**< PM_ResetCause_t */
    volatile uint32 retryCount;         /**< Fault retry counter */
    volatile sint32 apuTempC;           /**< APU die temperature (°C) */
    volatile uint32 prochotActive;      /**< PROCHOT status */
    volatile uint32 usbpdPort0State;    /**< USB PD port 0 state */
    volatile uint32 usbpdPort1State;    /**< USB PD port 1 state */
    volatile uint32 uptimeS;            /**< Seconds since boot */
    volatile uint32 updateCounter;      /**< Incremented on each update */
} Ipc_PmcStatus_t;

/* ================================================================== */
/*  Shared mailbox: CPU2 → all (FuSa status)                         */
/*  Writer: CPU2 only.  Readers: CPU0, CPU1.                          */
/* ================================================================== */

typedef struct
{
    volatile uint16 channelMv[24];      /**< VoltMon channel values */
    volatile uint32 uvFaultFlags;       /**< UV fault bitmask */
    volatile uint32 ovFaultFlags;       /**< OV fault bitmask */
    volatile uint32 tlfDevstat;         /**< TLF DEVSTAT register */
    volatile uint32 tlfSyssf;           /**< TLF system status flags */
    volatile uint32 tlfWdstat;          /**< TLF watchdog status */
    volatile uint32 tlfState;           /**< TLF state (INIT/NORMAL) */
    volatile uint32 tlfWdtSvc;          /**< WDT service count */
    volatile uint32 tlfWdtMiss;         /**< WDT miss count */
    volatile uint32 tlfErrPin;          /**< ERR pin state */
    volatile uint32 tlfSsPin;           /**< Safe-state pin state */
    volatile uint32 fusaStatus;         /**< 2-bit FUSA_STATUS field */
    volatile uint32 faultSeq;          /* bumped per new fault event     */
    volatile uint32 faultCode;         /* Ipc_FaultCode_t of latest      */
    volatile uint32 faultChannel;
    volatile uint32 faultMv;
    volatile uint32 faultActive;            /**< Measured mV at fault */
    volatile uint32 updateCounter;      /**< Incremented on each update */
    volatile uint32 tlfEvtSeq;        /* CPU2 increments per event   */
    volatile uint32 tlfEvtData[4];    /* syssf, monsf1, monsf2, devstat */
    volatile uint32 tlfRegsSeq;        /* CPU2 bumps after each snapshot   */
    volatile uint8  tlfRegs[12];       /* DEVSTAT,SYSSF,SPISF,MONSF0..2,
                                      INITERR,WDCFG0,WWDCFG0,WWDCFG1,
                                      WWDSTAT,pad                      */
    volatile uint32 warnSeq;
    volatile uint32 warnChannel;
    volatile uint32 warnMv;
} Ipc_FusaStatus_t;

/* ================================================================== */
/*  Top-level shared memory block (placed in LMU via linker)          */
/* ================================================================== */

typedef struct
{
    Ipc_CmdMailbox_t  cmd;              /**< CPU0 → CPU1 commands */
    Ipc_PmcStatus_t   pmc;             /**< CPU1 → all: PM status */
    Ipc_FusaStatus_t  fusa;            /**< CPU2 → all: FuSa status */
    volatile uint32   cpu1Ready;       /**< Set TRUE by CPU1 when init complete */
    volatile uint32   cpu2Ready;       /**< Set TRUE by CPU2 when init complete */
    volatile uint32 sysmonPause;
} Ipc_SharedMem_t;

typedef struct {
    volatile uint32 head;            /* writer-owned */
    volatile uint32 tail;            /* CPU0-owned   */
    volatile char   buf[DBGRING_SIZE];
} Ipc_DbgRing_t;


/* ================================================================== */
/*  LMU placement                                                     */
/*                                                                    */
/*  The linker script must place this at a fixed LMU address.         */
/*  Example linker section:                                           */
/*    .ipc_shared (NOLOAD) : { *(.ipc_shared) } > LMU_SRAM           */
/* ================================================================== */

extern volatile Ipc_SharedMem_t g_ipcShared __attribute__((section(".ipc_shared")));
extern volatile uint32 g_wdtOwner;
extern volatile Ipc_DbgRing_t g_dbgRing1;   /* CPU1 writes */
extern volatile Ipc_DbgRing_t g_dbgRing2;   /* CPU2 writes */


/* ================================================================== */
/*  Public API                                                        */
/* ================================================================== */

/**
 * @brief  Initialise IPC shared memory and IR interrupts.
 *
 * Called by CPU0 during startup, before releasing CPU1/CPU2.
 * Zeros all shared memory and installs IR interrupt handlers.
 */
void Ipc_Init(void);

/**
 * @brief  Send a command from CPU0 to CPU1.
 *
 * Writes the command to the mailbox, increments seqNum,
 * and fires an IR interrupt to CPU1.
 *
 * @param  cmd    Command code.
 * @param  param  Command-specific parameter.
 */
void Ipc_SendCommand(Ipc_Command_t cmd, uint32 param);


boolean Ipc_SendCommandWait(Ipc_Command_t cmd, uint32 param, uint32 timeoutMs);


/**
 * @brief  Check if the last command was acknowledged by CPU1.
 *
 * @return TRUE if ackNum == seqNum.
 */
boolean Ipc_IsCommandAcked(void);

/**
 * @brief  Signal a fault from CPU2 to CPU1 (and CPU0).
 *
 * Writes fault info to the FuSa status block and fires
 * IR interrupts to CPU1 and CPU0.
 *
 * @param  fault    Fault code.
 * @param  channel  Faulting channel index.
 * @param  mv       Measured millivolts.
 */
void Ipc_SignalFault(Ipc_FaultCode_t fault, uint32 channel, uint32 mv);

/**
 * @brief  Update PMC status from CPU1.
 *
 * Called periodically by CPU1's main loop to publish
 * current state to the shared memory block.
 */
void Ipc_UpdatePmcStatus(uint32 pmState, uint32 resetCause,
                         uint32 retryCount, sint32 apuTempC,
                         uint32 prochotActive);

/**
 * @brief  Update FuSa voltage data from CPU2.
 *
 * Called after each VoltMon_Scan() on CPU2 to publish
 * channel values to the shared memory block.
 *
 * @param  pChannelMv  Array of 24 uint16 millivolt values.
 * @param  uvFlags     Undervoltage fault bitmask.
 * @param  ovFlags     Overvoltage fault bitmask.
 */
void Ipc_UpdateVoltages(const uint16 *pChannelMv, uint32 uvFlags,
                        uint32 ovFlags);

/**
 * @brief  Update TLF PMIC status from CPU2.
 */
void Ipc_UpdateTlfStatus(uint32 devstat, uint32 syssf, uint32 wdstat,
                         uint32 state, uint32 wdtSvc, uint32 wdtMiss,
                         uint32 errPin, uint32 ssPin);

void Ipc_ClearFaultActive(void);

void Ipc_SignalWarning(uint32 channel, uint32 mv);

#endif /* IPC_H */