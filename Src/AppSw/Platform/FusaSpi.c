/**
 * @file    FusaSpi.c
 * @brief   COM-HPC FUSA_SPI register map — QSPI3 slave interface
 *
 * The carrier safety controller (SPI master) reads registers by:
 *   1. Sending a 32-bit frame: [ADDR:8 | 0x000000:24]
 *   2. On the NEXT frame, slave shifts out [DATA:32]
 *
 * The ISR-driven exchange prepares the response for the next
 * transaction based on the address received in the current one.
 */

#include "FusaSpi.h"

#if (FUSA_SPI_FEATURE_ENABLE == 1u)

#include "Platform_PinCfg.h"
#include "AppPin.h"
#include "Stm_Timer.h"
#include "Uart_Debug.h"
#include "PowerManager.h"
#include "VoltMon.h"
#include "Tlf35585.h"
#include "NvLog.h"
#include "IfxPort.h"
#include "IfxQspi_SpiSlave.h"
#include "IfxCpu_Irq.h"
#include "SysMonitor.h"
#include "UsbPd_Cfg.h"
#include "UsbPd_Manager.h"
#include "UsbPd_Hpd.h"
#include "ComHpcWdt.h"
#include "Ipc.h"

/* APU temperature older than this is reported as invalid. */
#define FUSA_APU_TEMP_STALE_MS      1000u

/* ================================================================== */
/*  Register map storage                                              */
/* ================================================================== */

static uint32 s_regMap[FUSA_REG_COUNT];

/* ================================================================== */
/*  SPI slave state                                                   */
/* ================================================================== */

static IfxQspi_SpiSlave s_spiSlave;
static boolean          s_initialised = FALSE;
static uint32           s_lastUpdateMs = 0u;

/* Double-buffered SPI exchange:
 * The slave always has one exchange pending.  When the master
 * clocks in a frame, the ISR completes the exchange, we read
 * the received address, prepare the response, and post the
 * next exchange. */
static uint32 s_txBuf;
static uint32 s_rxBuf;

/* ================================================================== */
/*  ISRs — must be before prv_SpiInit in the file                     */
/* ================================================================== */

IFX_INTERRUPT(qspi3TxISR, 0, QSPI3_TX_ISR_PRIO)
{
    IfxCpu_enableInterrupts();
    IfxQspi_SpiSlave_isrTransmit(&s_spiSlave);
}

IFX_INTERRUPT(qspi3RxISR, 0, QSPI3_RX_ISR_PRIO)
{
    IfxCpu_enableInterrupts();
    IfxQspi_SpiSlave_isrReceive(&s_spiSlave);

    /* Extract register address from received frame (bits [31:24]) */
    uint8 addr = (uint8)((s_rxBuf >> 24u) & 0xFFu);

    /* Prepare response for the NEXT exchange */
    if (addr < FUSA_REG_COUNT)
        s_txBuf = s_regMap[addr];
    else
        s_txBuf = 0xDEAD0000u | (uint32)addr;   /* Invalid address marker */

    /* Post the next exchange immediately */
    IfxQspi_SpiSlave_exchange(&s_spiSlave, &s_txBuf, &s_rxBuf, 1u);
}

IFX_INTERRUPT(qspi3ErISR, 0, QSPI3_ER_ISR_PRIO)
{
    IfxCpu_enableInterrupts();
    IfxQspi_SpiSlave_isrError(&s_spiSlave);
}

/* ================================================================== */
/*  Build config flags — assembled at compile time                    */
/* ================================================================== */

static uint32 prv_BuildConfigFlags(void)
{
    uint32 flags = 0u;

    flags |= FUSA_CFG_FUSA_SPI;     /* We're here, so it's enabled */

#if defined(TARGET_EVAL_BOARD)
    flags |= FUSA_CFG_EVAL_BOARD;
#endif

#if (TLF_FWD_ENABLE == 1u)
    flags |= FUSA_CFG_FWD_ENABLE;
#endif

/* These are always enabled in the current build */
    flags |= FUSA_CFG_TLF_ENABLED;
    flags |= FUSA_CFG_VOLTMON;
    flags |= FUSA_CFG_SOTA;

#if !defined(TARGET_EVAL_BOARD)
    flags |= FUSA_CFG_SYSMON;
#if (USBPD_FEATURE_ENABLE == 1u)
    flags |= FUSA_CFG_USBPD;
#endif
#if (SYSMON_CARRIER_WD_ENABLE == 1u)
    flags |= FUSA_CFG_COMHPC_WDT;
#endif
#endif

    return flags;
}

/* ================================================================== */
/*  SPI slave init                                                    */
/* ================================================================== */

static void prv_SpiInit(void)
{
    IfxQspi_SpiSlave_Config slaveCfg;
    IfxQspi_SpiSlave_initModuleConfig(&slaveCfg, &MODULE_QSPI3);

    const IfxQspi_SpiSlave_Pins slavePins = {
        &IfxQspi3_SCLKA_P02_7_IN,  IfxPort_InputMode_pullDown,
        &IfxQspi3_MTSRA_P02_6_IN,  IfxPort_InputMode_pullDown,
        &IfxQspi3_MRST_P02_5_OUT,  IfxPort_OutputMode_pushPull,
        &IfxQspi3_SLSIA_P02_4_IN,  IfxPort_InputMode_pullUp,
        IfxPort_PadDriver_cmosAutomotiveSpeed1
    };
    slaveCfg.pins = &slavePins;

    slaveCfg.mode               = IfxQspi_Mode_slave;
    slaveCfg.maximumBaudrate    = 10000000;
    slaveCfg.txPriority         = QSPI3_TX_ISR_PRIO;
    slaveCfg.rxPriority         = QSPI3_RX_ISR_PRIO;
    slaveCfg.erPriority         = QSPI3_ER_ISR_PRIO;
    slaveCfg.isrProvider        = IfxSrc_Tos_cpu0;

    slaveCfg.protocol.dataWidth      = 31u;
    slaveCfg.protocol.clockPolarity  = IfxQspi_ClockPolarity_idleLow;
    slaveCfg.protocol.shiftClock     = IfxQspi_ShiftClock_shiftTransmitDataOnTrailingEdge;
    slaveCfg.protocol.dataHeading    = IfxQspi_DataHeading_msbFirst;

    IfxQspi_SpiSlave_initModule(&s_spiSlave, &slaveCfg);

    IfxCpu_Irq_installInterruptHandler(&qspi3TxISR, QSPI3_TX_ISR_PRIO);
    IfxCpu_Irq_installInterruptHandler(&qspi3RxISR, QSPI3_RX_ISR_PRIO);
    IfxCpu_Irq_installInterruptHandler(&qspi3ErISR, QSPI3_ER_ISR_PRIO);
}
/* ================================================================== */
/*  Public API                                                        */
/* ================================================================== */

void FusaSpi_Init(void)
{
    uint32 i;

    Debug_Print("[FUSA_SPI] Init: QSPI3 slave...\r\n");

    /* Clear register map */
    for (i = 0u; i < FUSA_REG_COUNT; i++)
        s_regMap[i] = 0u;

    /* Populate static registers */
    s_regMap[FUSA_REG_MAGIC]        = FUSA_MAGIC;
    s_regMap[FUSA_REG_MAP_VERSION]  = FUSA_MAP_VERSION_PACKED;
    s_regMap[FUSA_REG_FW_VERSION]   = FUSA_FW_VERSION;
    s_regMap[FUSA_REG_BUILD_CONFIG] = prv_BuildConfigFlags();

    /* Init SPI slave hardware */
    prv_SpiInit();

    /* Prime the first exchange — slave is ready for the master */
    s_txBuf = FUSA_MAGIC;   /* First read returns magic */
    s_rxBuf = 0u;
    IfxQspi_SpiSlave_exchange(&s_spiSlave, &s_txBuf, &s_rxBuf, 1u);

    /* ALERT pin default HIGH (deasserted, active low) */
    IfxPort_setPinHigh(AppPin_GetPort(PIN_FUSA_SPI_ALERT.portIdx),
                       PIN_FUSA_SPI_ALERT.pinIdx);

    s_lastUpdateMs = Stm_GetTimeMs();
    s_initialised  = TRUE;

    Debug_Printf("[FUSA_SPI] Init OK, build_config=0x%08X\r\n",
                 (unsigned)s_regMap[FUSA_REG_BUILD_CONFIG]);
}

void FusaSpi_Update(void)
{
    uint32 nowMs;

    if (!s_initialised) return;

    nowMs = Stm_GetTimeMs();
    if ((nowMs - s_lastUpdateMs) < FUSA_SPI_UPDATE_INTERVAL_MS) return;
    s_lastUpdateMs = nowMs;

    /* ---- Platform state ---- */
    s_regMap[FUSA_REG_POWER_STATE]      = (uint32)PowerManager_GetState();
    s_regMap[FUSA_REG_LAST_RESET_CAUSE] = (uint32)PowerManager_GetResetCause();
    s_regMap[FUSA_REG_UPTIME_S]         = Stm_GetTimeMs() / 1000u;
    s_regMap[FUSA_REG_RETRY_COUNT]      = (uint32)PowerManager_GetRetryCount();

    /* PM signal flags packed: VIN_PWR_OK(0), SLP_S3(1), SLP_S5(2), APU_PWROK(3) */
    {
        uint32 pmFlags = 0u;
        /* These would be read from the actual GPIO pins.
         * For now, derive from PM state. */
        if (PowerManager_GetState() == PM_STATE_ON)
            pmFlags |= 0x0Fu;  /* All signals asserted in S0 */
        s_regMap[FUSA_REG_PM_FLAGS] = pmFlags;
    }

    /* ---- Thermal / APML ---- */
    s_regMap[FUSA_REG_THERMTRIP_COUNT] = PowerManager_GetThermtripCount();
#if !defined(TARGET_EVAL_BOARD)
    {
        /* SysMonitor (CPU1) publishes the SB-TSI reading to IPC.  It is only
         * refreshed in S0, so report INVALID outside S0 or when it is stale
         * rather than replaying the last value. */
        sint16  tempC = g_ipcShared.sysmon.apuTempC;
        boolean valid = (tempC != (sint16)SYSMON_TEMP_INVALID) &&
                        (PowerManager_GetState() == PM_STATE_ON) &&
                        ((nowMs - g_ipcShared.sysmon.apuTempMs) <= FUSA_APU_TEMP_STALE_MS);
        boolean throttling = SysMonitor_IsThrottling();

        s_regMap[FUSA_REG_APU_TEMP_C]     = (uint32)(sint32)(valid ? tempC : (sint16)SYSMON_TEMP_INVALID);
        s_regMap[FUSA_REG_PROCHOT_STATUS] = (uint32)throttling;
        s_regMap[FUSA_REG_THERMAL_STATE]  = (throttling ? 0x1u : 0x0u) |
                                            (valid      ? 0x2u : 0x0u);
    }
#endif

    /* ---- TLF PMIC ---- */
    s_regMap[FUSA_REG_TLF_STATE]    = (uint32)Tlf35585_GetDevState();
    s_regMap[FUSA_REG_TLF_ERR_PIN]  = (uint32)Tlf35585_IsErrActive();
    s_regMap[FUSA_REG_TLF_SS_PIN]   = (uint32)Tlf35585_IsSafeStateActive();
    s_regMap[FUSA_REG_TLF_WDT_SVC]  = Tlf35585_GetWwdServiceCount();
    s_regMap[FUSA_REG_TLF_WDT_MISS] = Tlf35585_GetWwdMissCount();
    /* DEVSTAT / SYSSF / WWDSTAT come from the ~1 Hz register snapshot
     * (Tlf35585_PublishRegSnapshot) so this task adds no QSPI2 traffic.
     * They read 0 until the first snapshot has been published. */
    if (g_ipcShared.fusa.tlfRegsSeq != 0u)
    {
        s_regMap[FUSA_REG_TLF_DEVSTAT] = (uint32)g_ipcShared.fusa.tlfRegs[0];
        s_regMap[FUSA_REG_TLF_SYSSF]   = (uint32)g_ipcShared.fusa.tlfRegs[1];
        s_regMap[FUSA_REG_TLF_WDSTAT]  = (uint32)g_ipcShared.fusa.tlfRegs[10];
    }

    /* ---- Voltage monitoring ---- */
    {
        uint32 ch;
        for (ch = 0u; ch < VOLTMON_MAX_CHANNELS; ch++)
        {
            if ((FUSA_REG_VOLT_CH_BASE + ch) < FUSA_REG_COUNT)
            {
                s_regMap[FUSA_REG_VOLT_CH_BASE + ch] =
                    (uint32)VoltMon_GetLastMv(ch);
            }
        }
        s_regMap[FUSA_REG_VOLT_TIMESTAMP] = nowMs;
    }
    {
        uint32 uv, ov, warn;
        VoltMon_GetFlags(&uv, &ov, &warn);
        s_regMap[FUSA_REG_VOLT_UV_FLAGS]   = uv;
        s_regMap[FUSA_REG_VOLT_OV_FLAGS]   = ov;
        s_regMap[FUSA_REG_VOLT_WARN_FLAGS] = warn;
    }

    /* ---- USB PD summary (CPU3 owns the manager) ---- */
#if !defined(TARGET_EVAL_BOARD) && (USBPD_FEATURE_ENABLE == 1u)
    if (g_ipcShared.cpu3Ready != 0u)
    {
        s_regMap[FUSA_REG_USBPD_PORT0_STATE] = UsbPdManager_GetPortStatusWord(0u);
        s_regMap[FUSA_REG_USBPD_PORT1_STATE] = UsbPdManager_GetPortStatusWord(1u);
        s_regMap[FUSA_REG_USBPD_FAULT_FLAGS] = UsbPdManager_GetFaultFlags();
        s_regMap[FUSA_REG_USBPD_HPD_STATE]   =
            (UsbPd_Hpd_GetLevel(0u) ? 0x1u : 0x0u) |
            (UsbPd_Hpd_GetLevel(1u) ? 0x2u : 0x0u);
    }
#endif

    /* ---- COM-HPC misc: raw pin levels, state of the host watchdog ---- */
    s_regMap[FUSA_REG_COMHPC_WDT_STATE] = ComHpcWdt_GetState();
    s_regMap[FUSA_REG_RAPID_SHUTDOWN] = (uint32)IfxPort_getPinState(
        AppPin_GetPort(PIN_RAPID_SD.portIdx), PIN_RAPID_SD.pinIdx);
    s_regMap[FUSA_REG_LID_STATE] = (uint32)IfxPort_getPinState(
        AppPin_GetPort(PIN_LID_L.portIdx), PIN_LID_L.pinIdx);
    s_regMap[FUSA_REG_TAMPER_STATE] = (uint32)IfxPort_getPinState(
        AppPin_GetPort(PIN_TAMPER_L.portIdx), PIN_TAMPER_L.pinIdx);

    /* ---- NV Log summary ---- */
    {
        NvLog_Stats_t stats;
        NvLog_GetStats(&stats);
        s_regMap[FUSA_REG_BOOT_COUNT]   = stats.bootCounter;
        s_regMap[FUSA_REG_NVLOG_SLOT]   = (uint32)stats.activeSlot;
        s_regMap[FUSA_REG_NVLOG_EVENTS] = stats.eventsInSlot;
        s_regMap[FUSA_REG_NVLOG_ERRORS] = stats.flushErrors;
        s_regMap[FUSA_REG_NVLOG_LAST_EVT] = (uint32)stats.lastEventType;
    }

    /* ---- FuSa status (matches GPIO-driven 2-bit field) ---- */
    {
        uint32 fusaStatus = 0u;
        uint32 pmState = (uint32)PowerManager_GetState();

        if (pmState == PM_STATE_OFF)
            fusaStatus = 0x00u;  /* 00 = power off */
        else if (pmState == PM_STATE_ON)
            fusaStatus = 0x01u;  /* 01 = power good */
        else if (pmState == PM_STATE_FAULT)
            fusaStatus = 0x02u;  /* 10 = fault */
        else
            fusaStatus = 0x03u;  /* 11 = reset/sequencing */

        s_regMap[FUSA_REG_FUSA_STATUS] = fusaStatus;
    }
}

void FusaSpi_AssertAlert(void)
{
    IfxPort_setPinLow(AppPin_GetPort(PIN_FUSA_SPI_ALERT.portIdx),
                      PIN_FUSA_SPI_ALERT.pinIdx);
    //Debug_Print("[FUSA_SPI] ALERT# asserted\r\n");
}

void FusaSpi_DeassertAlert(void)
{
    IfxPort_setPinHigh(AppPin_GetPort(PIN_FUSA_SPI_ALERT.portIdx),
                       PIN_FUSA_SPI_ALERT.pinIdx);
    //Debug_Print("[FUSA_SPI] ALERT# deasserted\r\n");
}

const uint32 *FusaSpi_GetRegMap(void)
{
    return s_regMap;
}

void FusaSpi_DumpRegMap(void)
{
    Debug_Print("[FUSA_SPI] === Register Map ===\r\n");
    Debug_Printf("[FUSA_SPI]  MAGIC=0x%08X VER=0x%08X FW=0x%08X\r\n",
                 (unsigned)s_regMap[FUSA_REG_MAGIC],
                 (unsigned)s_regMap[FUSA_REG_MAP_VERSION],
                 (unsigned)s_regMap[FUSA_REG_FW_VERSION]);
    Debug_Printf("[FUSA_SPI]  BUILD=0x%08X\r\n",
                 (unsigned)s_regMap[FUSA_REG_BUILD_CONFIG]);
    Debug_Printf("[FUSA_SPI]  PM_STATE=%u RESET=%u FUSA=%u UP=%us\r\n",
                 (unsigned)s_regMap[FUSA_REG_POWER_STATE],
                 (unsigned)s_regMap[FUSA_REG_LAST_RESET_CAUSE],
                 (unsigned)s_regMap[FUSA_REG_FUSA_STATUS],
                 (unsigned)s_regMap[FUSA_REG_UPTIME_S]);
    Debug_Printf("[FUSA_SPI]  TLF: state=%u err=%u ss=%u\r\n",
                 (unsigned)s_regMap[FUSA_REG_TLF_STATE],
                 (unsigned)s_regMap[FUSA_REG_TLF_ERR_PIN],
                 (unsigned)s_regMap[FUSA_REG_TLF_SS_PIN]);
    Debug_Printf("[FUSA_SPI]  NVLOG: boot=%u slot=%u events=%u\r\n",
                 (unsigned)s_regMap[FUSA_REG_BOOT_COUNT],
                 (unsigned)s_regMap[FUSA_REG_NVLOG_SLOT],
                 (unsigned)s_regMap[FUSA_REG_NVLOG_EVENTS]);
    Debug_Printf("[FUSA_SPI]  THERM: temp=%d state=0x%X trips=%u\r\n",
                 (int)(sint32)s_regMap[FUSA_REG_APU_TEMP_C],
                 (unsigned)s_regMap[FUSA_REG_THERMAL_STATE],
                 (unsigned)s_regMap[FUSA_REG_THERMTRIP_COUNT]);
    Debug_Printf("[FUSA_SPI]  TLF: devstat=0x%02X syssf=0x%02X wwdstat=0x%02X svc=%u miss=%u\r\n",
                 (unsigned)s_regMap[FUSA_REG_TLF_DEVSTAT],
                 (unsigned)s_regMap[FUSA_REG_TLF_SYSSF],
                 (unsigned)s_regMap[FUSA_REG_TLF_WDSTAT],
                 (unsigned)s_regMap[FUSA_REG_TLF_WDT_SVC],
                 (unsigned)s_regMap[FUSA_REG_TLF_WDT_MISS]);
    Debug_Printf("[FUSA_SPI]  VOLT flags: uv=0x%06X ov=0x%06X warn=0x%06X\r\n",
                 (unsigned)s_regMap[FUSA_REG_VOLT_UV_FLAGS],
                 (unsigned)s_regMap[FUSA_REG_VOLT_OV_FLAGS],
                 (unsigned)s_regMap[FUSA_REG_VOLT_WARN_FLAGS]);
    Debug_Printf("[FUSA_SPI]  USBPD: p0=0x%02X p1=0x%02X fault=0x%02X hpd=0x%X\r\n",
                 (unsigned)s_regMap[FUSA_REG_USBPD_PORT0_STATE],
                 (unsigned)s_regMap[FUSA_REG_USBPD_PORT1_STATE],
                 (unsigned)s_regMap[FUSA_REG_USBPD_FAULT_FLAGS],
                 (unsigned)s_regMap[FUSA_REG_USBPD_HPD_STATE]);
    Debug_Printf("[FUSA_SPI]  MISC: nvlog_last=%u comhpc_wdt=%u rapid=%u lid=%u tamper=%u\r\n",
                 (unsigned)s_regMap[FUSA_REG_NVLOG_LAST_EVT],
                 (unsigned)s_regMap[FUSA_REG_COMHPC_WDT_STATE],
                 (unsigned)s_regMap[FUSA_REG_RAPID_SHUTDOWN],
                 (unsigned)s_regMap[FUSA_REG_LID_STATE],
                 (unsigned)s_regMap[FUSA_REG_TAMPER_STATE]);
    Debug_Print("[FUSA_SPI] === End ===\r\n");
}

#else /* FUSA_SPI_FEATURE_ENABLE == 0 */

/* Stub implementations when FUSA_SPI is disabled */
void FusaSpi_Init(void) {}
void FusaSpi_Update(void) {}
void FusaSpi_AssertAlert(void) {}
void FusaSpi_DeassertAlert(void) {}
const uint32 *FusaSpi_GetRegMap(void) { return NULL_PTR; }
void FusaSpi_DumpRegMap(void) {}

#endif /* FUSA_SPI_FEATURE_ENABLE */