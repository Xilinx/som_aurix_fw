/**
 * @file    DebugCli.c
 * @brief   Interactive UART command parser
 */

#include "DebugCli.h"
#include "Uart_Debug.h"
#include "Stm_Timer.h"
#include "PowerManager.h"
#include "Tlf35585.h"
#include "VoltMon.h"
#include "NvLog.h"
#include "FusaSpi.h"
#include "Bist.h"
#include "Swap.h"
#include "IfxAsclin_Asc.h"
#include <string.h>
#include "PowerManager.h"
#include "Tlf35585.h"
#include "Ipc.h"
#include "SelfTest.h"
#include "SysMonitor.h"
#include "UsbPd_Manager.h"
#include "Platform_PinCfg.h"
#include "FwUpdate.h"
#include "I2c_Master.h"
#include "IfxAsclin_Asc.h"
#include "Uart_Xfer.h"
#include "Cypd6129_Drv.h"
#include "IfxI2c_I2c.h"
#include <stdlib.h>
#include "IfxI2c_reg.h"
#include "I2c_Slave.h"

static I2cSlave_Inst_t s_cliSlave;


#define CLI_SLAVE_MODULE   MODULE_I2C0
#define CLI_SNIFF_MODULE    MODULE_I2C0
#define CLI_SNIFF_SDA_PORT (&MODULE_P13)      /* <-- set to bus-0 SDA port   */
#define CLI_SNIFF_SDA_PIN  1u                 /* <-- set to bus-0 SDA pin    */
#define CLI_SNIFF_SCL_PORT (&MODULE_P13)      /* <-- set to bus-0 SCL port   */
#define CLI_SNIFF_SCL_PIN  2u                 /* <-- set to bus-0 SCL pin    */
#define CLI_SNIFF_MAXPKT   32u                /* bytes logged per packet     */
 

#define SNIFF_LOG 64u
static uint8  s_snPkt[SNIFF_LOG][8];
static uint8  s_snAck[SNIFF_LOG][8];
static uint8  s_snLen[SNIFF_LOG];
static uint32 s_snT[SNIFF_LOG];
/* ---- 2. Command handler: paste next to prv_CmdI2cScan --------------- */
 
#define SNIFF_SDA()  ((uint32)IfxPort_getPinState(CLI_SNIFF_SDA_PORT, CLI_SNIFF_SDA_PIN))
#define SNIFF_SCL()  ((uint32)IfxPort_getPinState(CLI_SNIFF_SCL_PORT, CLI_SNIFF_SCL_PIN))

/* ================================================================== */
/*  External references (defined in Uart_Debug.c)                     */
/* ================================================================== */

/* The debug UART ASC handle — needed for reading RX chars.
 * If your Uart_Debug doesn't expose this, add:
 *   IfxAsclin_Asc *Debug_GetAscHandle(void);
 * or expose a Debug_ReadChar function. */
extern IfxAsclin_Asc *Debug_GetAscHandle(void);

/* ================================================================== */
/*  State                                                             */
/* ================================================================== */
static char    s_cmdBuf[CLI_MAX_CMD_LEN + 1u];
static uint8   s_cmdLen = 0u;
static boolean s_initialised = FALSE;
static uint32        s_schedCmd     = 0u;   /* 0 = none */
static uint32        s_schedDelayMs = 0u;
static uint32        s_schedOnSeen  = 0u;   /* ms timestamp when ON first seen */

static boolean s_autobootArmed = (AUTOBOOT_DEFAULT != 0u);
static boolean s_autobootDone  = FALSE;   /* one shot per AURIX boot      */
static uint32  s_cliUpMs       = 0u;
static uint32  s_onSeenMs      = 0u;


/* ================================================================== */
/*  Private: string helpers                                           */
/* ================================================================== */

static boolean prv_StrEq(const char *a, const char *b)
{
    if ((a == NULL_PTR) || (b == NULL_PTR)) return FALSE;
    while (*a && *b)
    {
        if (*a != *b) return FALSE;
        a++; b++;
    }
    return (*a == *b);
}

/** Check if s starts with prefix, return pointer past prefix or NULL */
static const char *prv_StartsWith(const char *s, const char *prefix)
{
    if ((s == NULL_PTR) || (prefix == NULL_PTR)) return NULL_PTR;
    while (*prefix)
    {
        if (*s != *prefix) return NULL_PTR;
        s++; prefix++;
    }
    return s;
}

/** Simple atoi for small positive integers */
static uint32 prv_Atoi(const char *s)
{
    uint32 val = 0u;
    while (*s >= '0' && *s <= '9')
    {
        val = val * 10u + (uint32)(*s - '0');
        s++;
    }
    return val;
}

static uint32 prv_AtoiHex(const char *s)
{
    uint32 v = 0u;
    if ((s[0] == '0') && ((s[1] == 'x') || (s[1] == 'X'))) s += 2;
    while (1)
    {
        char c = *s++;
        if      ((c >= '0') && (c <= '9')) v = (v << 4u) | (uint32)(c - '0');
        else if ((c >= 'a') && (c <= 'f')) v = (v << 4u) | (uint32)(c - 'a' + 10);
        else if ((c >= 'A') && (c <= 'F')) v = (v << 4u) | (uint32)(c - 'A' + 10);
        else break;
    }
    return v;
}

/** Skip leading spaces */
static const char *prv_SkipSpaces(const char *s)
{
    while (*s == ' ') s++;
    return s;
}

/* ================================================================== */
/*  Command handlers                                                  */
/* ================================================================== */

static void prv_CmdHelp(void)
{
    Debug_Print(
        "Commands:\r\n"
        "  status      Full system status\r\n"
        "  poweron     Power on the APU\r\n"
        "  poweroff    Graceful shutdown\r\n"
        "  forceoff    Forced power off\r\n"
        "  warmreset   Warm reset\r\n"
        "  coldreset   Cold reboot\r\n"
        "  clearfault  Clear fault latch\r\n"
        "  tlf         TLF35585 status\r\n"
        "  vmon        Voltage monitor report\r\n"
        "  nvlog       NV log slot summary\r\n"
        "  nvlog recent [N]  Last N events\r\n"
        "  fusa        FUSA_SPI register dump\r\n"
        "  bist        POST/BIST status\r\n"
        "  usbpd       USB PD port states\r\n"
        "  temp        APU temperature\r\n"
        "  uptime      Seconds since boot\r\n"
        "  version     Firmware version\r\n"
        "  help        This message\r\n"
        "  selftest    Usage: selftest [all|crc|sota|swap|fusa|pm|usbpd|fwup] \r\n"
    );
}

static void prv_CmdStatus(void)
{
    uint32 uptimeS = Stm_GetTimeMs() / 1000u;

    Debug_Print("=== System Status ===\r\n");
    Debug_Printf("  FW: TC387 COM-HPC Controller v0.1\r\n");
    Debug_Printf("  Uptime:   %u s\r\n", (unsigned)uptimeS);
    Debug_Printf("  PM state: %u\r\n",
                 (unsigned)g_ipcShared.pmc.pmState);
    Debug_Printf("  Bank:     0x%02X\r\n",
                 (unsigned)Swap_GetCurrentBank());

    /* TLF summary */
    Debug_Printf("  TLF:      DEV=0x%02X ERR=%u SS=%u\r\n",
                 (unsigned)Tlf35585_GetDevState(),
                 (unsigned)Tlf35585_IsErrActive(),
                 (unsigned)Tlf35585_IsSafeStateActive());

    /* NvLog summary */
    {
        NvLog_Stats_t stats;
        NvLog_GetStats(&stats);
        Debug_Printf("  NvLog:    boot=%u slot=%u events=%u\r\n",
                     (unsigned)stats.bootCounter,
                     (unsigned)stats.activeSlot,
                     (unsigned)stats.eventsInSlot);
    }

    /* BIST */
    {
        Bist_Stats_t bist;
        Bist_GetStats(&bist);
        Debug_Printf("  POST:     %s\r\n",
                     (bist.postResult == BIST_OK) ? "PASS" :
                     (bist.postResult == BIST_ERR_NO_META) ? "SKIP" : "FAIL");
    }

    Debug_Printf("  PROCHOT:  %u\r\n",
                 (unsigned)SysMonitor_IsThrottling());

    Debug_Print("=====================\r\n");
}

static void prv_CmdPowerOn(void)
{
    uint32 st = g_ipcShared.pmc.pmState;
    if ((st != (uint32)PM_STATE_OFF) && (st != (uint32)PM_STATE_S5))
    {
        Debug_Printf("  Rejected: PM state is %u (not OFF/S5)\r\n", (unsigned)st);
        return;
    }
    Debug_Print("  Requesting power on...\r\n");
    if (!Ipc_SendCommandWait(IPC_CMD_POWER_ON, 0u, 200u))
        Debug_Print("  ERROR: CPU1 did not ack\r\n");
}

static void prv_CmdPowerOff(void)
{
    if (g_ipcShared.pmc.pmState != PM_STATE_ON)
    {
        Debug_Printf("  Rejected: current state is %u (not ON)\r\n",
                     (unsigned)g_ipcShared.pmc.pmState);
        return;
    }
    Debug_Print("  Requesting power off...\r\n");
    NvLog_WriteU32(NVLOG_EVT_SHUTDOWN_OPERATOR, NVLOG_SRC_DEBUG,
                   NVLOG_SEV_INFO, 0u);
    if (!Ipc_SendCommandWait(IPC_CMD_POWER_OFF, 0u, 200u))
        Debug_Print("  ERROR: CPU1 did not ack\r\n");
}

static void prv_CmdForceOff(void)
{
    Debug_Print("  Forcing power off...\r\n");
    NvLog_WriteU32(NVLOG_EVT_SHUTDOWN_FORCED, NVLOG_SRC_DEBUG,
                   NVLOG_SEV_WARNING, 0u);
    NvLog_SealSlot(NVLOG_EVT_SHUTDOWN_FORCED);
    if (!Ipc_SendCommandWait(IPC_CMD_FORCED_OFF, 0u, 200u))
        Debug_Print("  ERROR: CPU1 did not ack\r\n");
}

static void prv_CmdWarmReset(void)
{
    if (g_ipcShared.pmc.pmState != PM_STATE_ON)
    {
        Debug_Printf("  Rejected: current state is %u (not ON)\r\n",
                     (unsigned)g_ipcShared.pmc.pmState);
        return;
    }
    Debug_Print("  Requesting warm reset...\r\n");
    NvLog_WriteU32(NVLOG_EVT_RESET_WARM, NVLOG_SRC_DEBUG,
                   NVLOG_SEV_INFO, 0u);
    if (!Ipc_SendCommandWait(IPC_CMD_WARM_RESET, 0u, 200u))
        Debug_Print("  ERROR: CPU1 did not ack\r\n");
}

static void prv_CmdColdReset(void)
{
    Debug_Print("  Requesting cold reboot...\r\n");
    NvLog_WriteU32(NVLOG_EVT_RESET_COLD, NVLOG_SRC_DEBUG,
                   NVLOG_SEV_INFO, 0u);
    PowerManager_RequestColdReset();
}

static void prv_CmdClearFault(void)
{
    if (g_ipcShared.pmc.pmState != PM_STATE_FAULT)
    {
        Debug_Print("  No fault to clear\r\n");
        return;
    }
    Debug_Print("  Clearing fault latch...\r\n");
    if (!Ipc_SendCommandWait(IPC_CMD_CLEAR_FAULT, 0u, 200u))
        Debug_Print("  ERROR: CPU1 did not ack\r\n");
}

static void prv_CmdTlf(void)
{
    Tlf35585_DumpStatus("35585");
}

static void prv_CmdVmon(void)
{
    uint32 ch;
    Debug_Print("[VMON] --- IPC Voltage Report ---\r\n");
    for (ch = 0u; ch < VOLTMON_MAX_CHANNELS; ch++)
    {
        uint16 mv = g_ipcShared.fusa.channelMv[ch];
        if (mv != 0u)
        {
            Debug_Printf("[VMON] CH%u = %u mV\r\n",
                         (unsigned)ch, (unsigned)mv);
        }
    }
    Debug_Print("[VMON] -----------------\r\n");
}

static void prv_CmdNvlog(const char *args)
{
    const char *sub = prv_SkipSpaces(args);

    if (*sub == '\0')
    {
        NvLog_DumpSlotInfo();
    }
    else if (strncmp(sub, "recent", 6) == 0)
    {
        uint32 count = 9u;
        uint8  slot  = NVLOG_SLOT_ACTIVE;
        const char *p = prv_SkipSpaces(sub + 6);

        if (*p != '\0')
        {
            count = prv_Atoi(p);
            while ((*p >= '0') && (*p <= '9')) p++;     /* skip the digits */
            p = prv_SkipSpaces(p);
            if (*p != '\0')
                slot = (uint8)prv_Atoi(p);
        }
        NvLog_DumpRecent(slot, count);
    }
    else if (strncmp(sub, "slot", 4) == 0)
    {
        const char *p = prv_SkipSpaces(sub + 4);
        if (*p == '\0')
        {
            Debug_Print("[NVLOG] usage: nvlog slot <0-3>\r\n");
            return;
        }
        NvLog_DumpRecent((uint8)prv_Atoi(p), 0xFFFFFFFFu);
    }
    else
    {
        Debug_Print("[NVLOG] usage: nvlog | nvlog recent [N] [slot] | nvlog slot <s>\r\n");
    }
}

static void prv_CmdFusa(void)
{
    FusaSpi_DumpRegMap();
}

static void prv_CmdBist(void)
{
    Bist_DumpStatus();
}

static void prv_CmdUptime(void)
{
    uint32 s = Stm_GetTimeMs() / 1000u;
    uint32 h = s / 3600u;
    uint32 m = (s % 3600u) / 60u;
    Debug_Printf("  Uptime: %u:%02u:%02u (%u s)\r\n",
                 (unsigned)h, (unsigned)m, (unsigned)(s % 60u),
                 (unsigned)s);
}

static void prv_CmdVersion(void)
{
    Debug_Printf("  TC387 COM-HPC Controller v0.1\r\n");
}

static void prv_CmdUsbPd(void)
{
    uint8 i;
    for (i = 0u; i < 2u; i++)
    {
        UsbPd_PortState_t st = UsbPdManager_GetPortState(i);
        const char *stStr;
        switch (st)
        {
            case USBPD_PORT_ATTACHED:  stStr = "ATTACHED"; break;
            case USBPD_PORT_CONTRACT:  stStr = "CONTRACT"; break;
            default:                   stStr = "DETACHED"; break;
        }
        Debug_Printf("  Port %u: %s\r\n", (unsigned)i, stStr);
    }
}

static void prv_CmdTemp(void)
{
    /* SysMonitor exposes temperature via the last-read cache.
     * If not exposed, print "not available". */
    Debug_Print("  APU temp: read via 'status' (SB-TSI polled in SysMonitor)\r\n");
}

static void prv_CmdMux(const char *args)
{
    const char *a = prv_SkipSpaces(args);
    if (prv_StrEq(a, "apu"))
    {
        Debug_Print("  UART MUX -> APU. CLI unreachable on this port;\r\n"
                    "  use side UART or reset to return.\r\n");
        Stm_DelayMs(20u);                    /* let the warning flush */
        IfxPort_setPinLow(AppPin_GetPort(PIN_UART_MUX_SEL.portIdx),
                          PIN_UART_MUX_SEL.pinIdx);   /* check polarity! */
    }
    else if (prv_StrEq(a, "aurix"))
    {
        IfxPort_setPinHigh(AppPin_GetPort(PIN_UART_MUX_SEL.portIdx),
                           PIN_UART_MUX_SEL.pinIdx);
        Debug_Print("  UART MUX -> AURIX\r\n");
    }
    else
        Debug_Print("  usage: mux apu|aurix\r\n");
}

static void prv_CmdAutoboot(const char *args)
{
    const char *a = prv_SkipSpaces(args);
    if      (prv_StrEq(a, "on"))  { s_autobootArmed = TRUE;  s_autobootDone = FALSE; }
    else if (prv_StrEq(a, "off")) { s_autobootArmed = FALSE; }
    else { Debug_Printf("  autoboot: %u (usage: autoboot on|off)\r\n",
                        (unsigned)s_autobootArmed); return; }
    Debug_Printf("  autoboot = %u\r\n", (unsigned)s_autobootArmed);
}


static void prv_CmdSched(const char *args)
{
    const char *a = prv_SkipSpaces(args);
    const char *num;
    if      ((num = prv_StartsWith(a, "off "))   != NULL_PTR) s_schedCmd = (uint32)IPC_CMD_POWER_OFF;
    else if ((num = prv_StartsWith(a, "force ")) != NULL_PTR) s_schedCmd = (uint32)IPC_CMD_FORCED_OFF;
    else if ((num = prv_StartsWith(a, "warm "))  != NULL_PTR) s_schedCmd = (uint32)IPC_CMD_WARM_RESET;
    else if (prv_StrEq(a, "cancel")) { s_schedCmd = 0u; Debug_Print("  sched cleared\r\n"); return; }
    else { Debug_Print("  usage: sched off|force|warm <secs> | sched cancel\r\n"); return; }
    s_schedDelayMs = prv_Atoi(prv_SkipSpaces(num)) * 1000u;
    s_schedOnSeen  = 0u;
    Debug_Printf("  armed: cmd=%u, %u ms after ON\r\n",
                 (unsigned)s_schedCmd, (unsigned)s_schedDelayMs);
}

static void prv_CmdI2cScan(const char *args)
{
    uint8 addr, dummy, found = 0u;
    uint32 histo[8] = {0};
    uint8 bus = 1u;                              /* default: APML */
    const char *a = prv_SkipSpaces(args);
    if (*a != '\0') bus = (uint8)prv_Atoi(a);
    if (bus > 1u) { Debug_Print("[I2C] usage: i2cscan [0|1]\r\n"); return; }

    Debug_Printf("[I2C] scanning bus %u (%s) 0x08-0x77...\r\n",
                 (unsigned)bus, (bus == 1u) ? "APML" : "HPI");
    for (addr = 0x08u; addr <= 0x77u; addr++)
    {
        I2c_Status_t st;
        if (bus == 1u)
            st = I2cMaster_ApmlReadByte(addr, 0x00u, &dummy);
        else
            st = I2cMaster_ReadReg16_Bus(0u, addr, 0x0000u, &dummy, 1u);
        if (st == I2C_OK) { Debug_Printf("[I2C]   ACK at 0x%02X\r\n", (unsigned)addr); found++; }
        else if ((uint32)st < 8u) histo[(uint32)st]++;
    }
    Debug_Printf("[I2C] done, %u device(s); errs:", (unsigned)found);
    { uint32 i; for (i = 0u; i < 8u; i++) if (histo[i]) Debug_Printf(" [%u]x%u", (unsigned)i, (unsigned)histo[i]); }
    Debug_Print("\r\n");
}

static void prv_CmdI2cStat(void)
{
    Debug_Printf("[I2C] BUSSTAT bus0=%u bus1=%u\r\n",
                 (unsigned)I2cMaster_GetRawBusStatus(0u),
                 (unsigned)I2cMaster_GetRawBusStatus(1u));
}

/* In DebugCli.c: */
static void prv_CmdFwUpdate(void)
{
    Debug_Print("[FWUP] Entering update mode on debug UART...\r\n");
    Debug_Print("[FWUP] Run aurix_update.py now. Press ESC to abort.\r\n");

    UartXfer_SetHandle(Debug_GetAscHandle());
    FwUpdate_Abort();  /* Reset state to IDLE */

    while (1)
    {
        FwUpdate_State_t st = FwUpdate_Run();
        Tlf35585_ServiceWdt();

        if (st == FWUPDATE_DONE || st == FWUPDATE_ERROR)
            break;
    }

    UartXfer_SetHandle(NULL_PTR);  /* Restore default */
    Debug_Print("[FWUP] Exited update mode\r\n");
}


static void prv_CmdSysmon(const char *args)
{
    const char *a = prv_SkipSpaces(args);
    if (prv_StrEq(a, "pause"))
    {
        g_ipcShared.sysmonPause = 1u;
        __dsync();
        Debug_Print("  SysMonitor paused (APML bus free for diagnostics)\r\n");
    }
    else if (prv_StrEq(a, "resume"))
    {
        g_ipcShared.sysmonPause = 0u;
        __dsync();
        Debug_Print("  SysMonitor resumed\r\n");
    }
    else
    {
        Debug_Printf("  sysmon: %s (usage: sysmon pause|resume)\r\n",
                     (g_ipcShared.sysmonPause != 0u) ? "PAUSED" : "running");
    }
}

static void prv_CmdI2cReset(const char *args)
{
    const char *a = prv_SkipSpaces(args);
    uint8 bus = (uint8)prv_Atoi(a);
    if ((*a == '\0') || (bus > 1u)) { Debug_Print("[I2C] usage: i2creset 0|1\r\n"); return; }
    I2cMaster_ReinitBus(bus);
    Debug_Printf("[I2C] bus %u reinitialised, BUSSTAT now %u\r\n",
                 (unsigned)bus, (unsigned)I2cMaster_GetRawBusStatus(bus));
}

static void prv_CmdI2cId(const char *args)
{
    const char *a = prv_SkipSpaces(args);
    uint8 addr;
    uint8 buf[2];
    I2c_Status_t st;

    if (*a == '\0') { Debug_Print("[I2C] usage: i2cid <hex addr, e.g. 54>\r\n"); return; }
    addr = (uint8)prv_AtoiHex(a);                       /* see below */

    st = I2cMaster_ReadReg16_Bus(0u, addr, CYPD_REG_SILICON_ID, buf, 2u);
    if (st == I2C_OK)
        Debug_Printf("[I2C] 0x%02X: SILICON_ID=0x%02X%02X\r\n",
                     (unsigned)addr, (unsigned)buf[1], (unsigned)buf[0]);
    else
        Debug_Printf("[I2C] 0x%02X: SILICON_ID read failed (%u) — not HPI?\r\n",
                     (unsigned)addr, (unsigned)st);

    st = I2cMaster_ReadReg16_Bus(0u, addr, CYPD_REG_DEVICE_MODE, buf, 2u);
    if (st == I2C_OK)
        Debug_Printf("[I2C] 0x%02X: DEVICE_MODE=0x%02X%02X\r\n",
                     (unsigned)addr, (unsigned)buf[1], (unsigned)buf[0]);
}

static uint32 prv_HexToU32(const char *s)
{
    uint32 val = 0u;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    while (*s)
    {
        uint8 c = (uint8)*s;
        if (c >= '0' && c <= '9')      val = (val << 4u) | (c - '0');
        else if (c >= 'a' && c <= 'f') val = (val << 4u) | (c - 'a' + 10u);
        else if (c >= 'A' && c <= 'F') val = (val << 4u) | (c - 'A' + 10u);
        else break;
        s++;
    }
    return val;
}

static void prv_CmdI2cRead(const char *args)
{
    args = prv_SkipSpaces(args);
    uint8 bus = (uint8)prv_Atoi(args);
    while (*args && *args != ' ') args++;
    args = prv_SkipSpaces(args);
    uint8 addr = (uint8)prv_HexToU32(args);
    while (*args && *args != ' ') args++;
    args = prv_SkipSpaces(args);
    uint8 reg = (uint8)prv_HexToU32(args);

    uint8 val = 0u;
    I2c_Status_t st;

    if (bus == 1u)
    {
        st = I2cMaster_ApmlReadByte(addr, reg, &val);
    }
    else
    {
        st = I2cMaster_ReadReg16_Bus(bus, addr, (uint16)reg, &val, 1u);
    }

    Debug_Printf("[I2C] bus=%u addr=0x%02X reg=0x%02X → %s val=0x%02X\r\n",
                 (unsigned)bus, (unsigned)addr, (unsigned)reg,
                 (st == I2C_OK) ? "OK" : "FAIL", (unsigned)val);
}
static void prv_CmdPin(void)
{
    Debug_Print("[PIN] name              lvl\r\n");
    Debug_Printf("[PIN] PLTRST_L (P34.2)   %u   (1 = carrier out of reset)\r\n",
                 (unsigned)prv_ReadPin(&PIN_PLTRST_L));
    Debug_Printf("[PIN] APU_RESET_IN_L     %u   (mirror input: must be 1)\r\n",
                 (unsigned)prv_ReadPin(&PIN_APU_RESET_IN_L));
    Debug_Printf("[PIN] CB_RSTBTN_L        %u   (mirror input: must be 1)\r\n",
                 (unsigned)prv_ReadPin(&PIN_CB_RSTBTN_L));
    Debug_Printf("[PIN] RSMRST_OUT_L       %u\r\n",
                 (unsigned)prv_ReadPin(&PIN_RSMRST_OUT_L));
    Debug_Printf("[PIN] COLD_RST           %u\r\n",
                 (unsigned)prv_ReadPin(&PIN_COLD_RST));
    Debug_Printf("[PIN] APU_PWROK          %u   VIN_PWR_OK %u\r\n",
                 (unsigned)prv_ReadPin(&PIN_APU_PWROK),
                 (unsigned)prv_ReadPin(&PIN_VIN_PWR_OK));
    Debug_Printf("[PIN] SLP_S3 %u  SLP_S5 %u  THERMTRIP_L %u\r\n",
                 (unsigned)prv_ReadPin(&PIN_SLP_S3),
                 (unsigned)prv_ReadPin(&PIN_SLP_S5),
                 (unsigned)prv_ReadPin(&PIN_THERMTRIP_L));
    {
        uint8 i, pdHi = 0u, apmlHi = 0u;
        for (i = 0u; i < 10u; i++)
        {
            if (prv_ReadPin(&PIN_USBC_PD_ALERT_L)) pdHi++;
            if (prv_ReadPin(&PIN_APML_ALERT))      apmlHi++;
            Stm_DelayMs(1u);
        }
        Debug_Printf("[PIN] USBC_PD_ALERT_L    %u/10 high  %s\r\n",
                     (unsigned)pdHi,
                     (pdHi == 10u) ? "(pulled up - PD domain alive?)" :
                     (pdHi == 0u)  ? "(solid low - asserted or dead)"  :
                                     "(UNSTABLE - floating!)");
        Debug_Printf("[PIN] APML_ALERT         %u/10 high  %s\r\n",
                     (unsigned)apmlHi,
                     (apmlHi == 10u) ? "(pulled up)" :
                     (apmlHi == 0u)  ? "(solid low)" : "(UNSTABLE - floating!)");
        Debug_Printf("[PIN] USBC_PD_INT (10.8) %u\r\n",
                     (unsigned)prv_ReadPin(&PIN_USBC_PD_INT_TO_APU));
    }
}


static void prv_CmdI2cProbe(void)
{
    /* Temporarily read SDA/SCL as GPIO to check voltage level */
    boolean sda = (boolean)IfxPort_getPinState(&MODULE_P11, 13u);
    boolean scl = (boolean)IfxPort_getPinState(&MODULE_P11, 14u);
    Debug_Printf("[I2C1] SDA(P11.13)=%u SCL(P11.14)=%u\r\n",
                 (unsigned)sda, (unsigned)scl);
    /* Both should read HIGH (1) if pull-ups are working */
    /* If either reads LOW, the line is stuck — wiring issue */
}

static void prv_CmdI2cDiag(void)
{
    boolean sda, scl;

    /* Step 1: Read with I2C module active */
    sda = (boolean)IfxPort_getPinState(&MODULE_P11, 13u);
    scl = (boolean)IfxPort_getPinState(&MODULE_P11, 14u);
    Debug_Printf("[I2C1] With module active: SDA=%u SCL=%u\r\n",
                 (unsigned)sda, (unsigned)scl);

    /* Step 2: Disable I2C1 module, set pins to input (hi-Z) */
    MODULE_I2C1.RUNCTRL.U = 0u;  /* Stop the module */

    IfxPort_setPinModeInput(&MODULE_P11, 13u, IfxPort_InputMode_noPullDevice);
    IfxPort_setPinModeInput(&MODULE_P11, 14u, IfxPort_InputMode_noPullDevice);

    Stm_DelayMs(1u);

    sda = (boolean)IfxPort_getPinState(&MODULE_P11, 13u);
    scl = (boolean)IfxPort_getPinState(&MODULE_P11, 14u);
    Debug_Printf("[I2C1] Pins as input (AURIX released): SDA=%u SCL=%u\r\n",
                 (unsigned)sda, (unsigned)scl);

    if (scl == 0u)
    {
        Debug_Print("[I2C1] SCL still LOW with AURIX released\r\n");
        Debug_Print("[I2C1]   -> APU or external device is holding SCL low\r\n");
        Debug_Print("[I2C1]   -> Check if APU SIC pin is configured as output driving low\r\n");
    }
    else
    {
        Debug_Print("[I2C1] SCL went HIGH when AURIX released\r\n");
        Debug_Print("[I2C1]   -> AURIX I2C module was holding SCL low\r\n");
        Debug_Print("[I2C1]   -> I2C1 init has a pin mode or module config issue\r\n");

        /* Step 3: Try bus recovery — toggle SCL 9 times as GPIO */
        Debug_Print("[I2C1] Attempting bus recovery (9 clock pulses)...\r\n");
        IfxPort_setPinModeOutput(&MODULE_P11, 14u,
                                  IfxPort_OutputMode_openDrain,
                                  IfxPort_OutputIdx_general);
        uint32 i;
        for (i = 0u; i < 9u; i++)
        {
            IfxPort_setPinLow(&MODULE_P11, 14u);
            Stm_DelayMs(1u);
            IfxPort_setPinHigh(&MODULE_P11, 14u);
            Stm_DelayMs(1u);
        }

        sda = (boolean)IfxPort_getPinState(&MODULE_P11, 13u);
        scl = (boolean)IfxPort_getPinState(&MODULE_P11, 14u);
        Debug_Printf("[I2C1] After recovery: SDA=%u SCL=%u\r\n",
                     (unsigned)sda, (unsigned)scl);
    }

    /* Step 4: Re-init I2C to restore normal operation */
    Debug_Print("[I2C1] Re-initialising I2C1...\r\n");
    I2cMaster_ReinitBus(1u);

    Stm_DelayMs(10u);
    sda = (boolean)IfxPort_getPinState(&MODULE_P11, 13u);
    scl = (boolean)IfxPort_getPinState(&MODULE_P11, 14u);
    Debug_Printf("[I2C1] After reinit: SDA=%u SCL=%u\r\n",
                 (unsigned)sda, (unsigned)scl);
}

static void prv_CmdI2cBitbang(void)
{
    /* Bit-bang a single I2C read to 0x4C (SB-TSI) reg 0x01
     * This bypasses the iLLD I2C module entirely */
    uint8 addr;
    uint32 found = 0u;

    MODULE_I2C1.RUNCTRL.U = 0u;
    Stm_DelayMs(1u);

    IfxPort_setPinModeOutput(&MODULE_P11, 13u,
                             IfxPort_OutputMode_openDrain,
                             IfxPort_OutputIdx_general);
    IfxPort_setPinModeOutput(&MODULE_P11, 14u,
                             IfxPort_OutputMode_openDrain,
                             IfxPort_OutputIdx_general);

    Debug_Print("[I2C1_BB] Scanning 0x08-0x77 via bit-bang...\r\n");

    for (addr = 0x08u; addr <= 0x77u; addr++)
    {
        /* Release lines */
        IfxPort_setPinHigh(&MODULE_P11, 13u);
        IfxPort_setPinHigh(&MODULE_P11, 14u);
        Stm_DelayUs(10u);

        /* START */
        IfxPort_setPinLow(&MODULE_P11, 13u);
        Stm_DelayUs(5u);
        IfxPort_setPinLow(&MODULE_P11, 14u);
        Stm_DelayUs(5u);

        /* Send address byte: addr << 1 | 0 (write) */
        uint8 addrByte = (addr << 1u) | 0u;
        sint8 bit;
        for (bit = 7; bit >= 0; bit--)
        {
            if (addrByte & (1u << bit))
                IfxPort_setPinHigh(&MODULE_P11, 13u);
            else
                IfxPort_setPinLow(&MODULE_P11, 13u);
            Stm_DelayUs(5u);
            IfxPort_setPinHigh(&MODULE_P11, 14u);
            Stm_DelayUs(5u);
            IfxPort_setPinLow(&MODULE_P11, 14u);
            Stm_DelayUs(5u);
        }

        /* ACK bit */
        IfxPort_setPinHigh(&MODULE_P11, 13u);
        IfxPort_setPinModeInput(&MODULE_P11, 13u,
                                IfxPort_InputMode_noPullDevice);
        Stm_DelayUs(5u);
        IfxPort_setPinHigh(&MODULE_P11, 14u);
        Stm_DelayUs(5u);
        boolean ack = (boolean)(IfxPort_getPinState(&MODULE_P11, 13u) == 0u);
        IfxPort_setPinLow(&MODULE_P11, 14u);
        Stm_DelayUs(5u);

        /* Restore SDA as output for STOP */
        IfxPort_setPinModeOutput(&MODULE_P11, 13u,
                                 IfxPort_OutputMode_openDrain,
                                 IfxPort_OutputIdx_general);

        /* STOP */
        IfxPort_setPinLow(&MODULE_P11, 13u);
        Stm_DelayUs(5u);
        IfxPort_setPinHigh(&MODULE_P11, 14u);
        Stm_DelayUs(5u);
        IfxPort_setPinHigh(&MODULE_P11, 13u);
        Stm_DelayUs(10u);

        if (ack)
        {
            Debug_Printf("[I2C1_BB] ACK at 0x%02X\r\n", (unsigned)addr);
            found++;
        }

        if ((addr & 0x0Fu) == 0x0Fu)
            Tlf35585_ServiceWdt();
    }

    Debug_Printf("[I2C1_BB] Done, %u device(s) found\r\n", (unsigned)found);

    /* Restore I2C module */
    I2cMaster_ReinitBus(1u);
}



/**
 * i2csniff [secs]
 * Passive sniff of bus 0: logs every transaction from any master to any
 * address. Plug a Type-C device in while it runs. ESC to stop.
 */
static void prv_CmdI2cSniff(const char *args)
{
    IfxAsclin_Asc *asc = Debug_GetAscHandle();
    const char *a = prv_SkipSpaces(args);
    uint32 secs = 30u, t0, pkts = 0u;
    uint8  pkt[CLI_SNIFF_MAXPKT];
    uint8  ack[CLI_SNIFF_MAXPKT];
    uint32 n = 0u;
    boolean inPkt = FALSE, aborted = FALSE;
    uint32 sdaPrev, sclPrev;
    uint32 seen[16] = {0};                    /* bitmap of 7-bit addresses seen */
 
    if (*a != '\0') secs = prv_Atoi(a);
 
    /* release the bus: module off, pins hi-Z inputs */
    CLI_SNIFF_MODULE.RUNCTRL.U = 0u;
    IfxPort_setPinModeInput(CLI_SNIFF_SDA_PORT, CLI_SNIFF_SDA_PIN, IfxPort_InputMode_noPullDevice);
    IfxPort_setPinModeInput(CLI_SNIFF_SCL_PORT, CLI_SNIFF_SCL_PIN, IfxPort_InputMode_noPullDevice);
    Stm_DelayMs(1u);
 
    Debug_Printf("[SNIFF] bus0 idle SDA=%u SCL=%u; listening %us, ESC to stop\r\n",
                 (unsigned)SNIFF_SDA(), (unsigned)SNIFF_SCL(), (unsigned)secs);
 
    sdaPrev = SNIFF_SDA();
    sclPrev = SNIFF_SCL();
    t0 = Stm_GetTimeMs();
 
    while ((Stm_GetTimeMs() - t0) < (secs * 1000u))
    {
        uint32 sda = SNIFF_SDA();
        uint32 scl = SNIFF_SCL();
 
        /* START: SDA falls while SCL high ------------------------------ */
        if (scl && sclPrev && sdaPrev && !sda)
        {
            uint32 tPkt = Stm_GetTimeMs() - t0;
            boolean stop = FALSE;
            n = 0u;
 
            /* decode bytes until STOP or repeated START ---------------- */
            while (!stop && (n < CLI_SNIFF_MAXPKT))
            {
                uint32 b = 0u, bit;
                boolean rs = FALSE;
 
                for (bit = 0u; bit < 8u; bit++)
                {
                    uint32 d;
                    while (SNIFF_SCL()) {}                 /* wait SCL low  */
                    while (!SNIFF_SCL()) {}                /* wait SCL high */
                    d = SNIFF_SDA();
                    /* while SCL high, watch for STOP (SDA rises) or
                       repeated START (SDA falls) on the first bit        */
                    while (SNIFF_SCL())
                    {
                        uint32 d2 = SNIFF_SDA();
                        if (bit == 0u)
                        {
                            if (d2 && !d)  { stop = TRUE; break; }
                            if (!d2 && d)  { rs   = TRUE; break; }
                        }
                    }
                    if (stop || rs) break;
                    b = (b << 1u) | d;
                }
                if (stop) break;
                if (rs) { pkt[n] = 0xFFu; ack[n] = 2u; n++; continue; }  /* mark Sr */
 
                /* ACK bit */
                while (SNIFF_SCL()) {}
                while (!SNIFF_SCL()) {}
                pkt[n] = (uint8)b;
                ack[n] = (uint8)(SNIFF_SDA() == 0u);       /* 1 = ACK   */
                n++;
            }
 
            /* print packet ---------------------------------------------- */
            {
                if (pkts < SNIFF_LOG)
                {
                    uint32 i, m = (n < 8u) ? n : 8u;
                    for (i = 0u; i < m; i++) { s_snPkt[pkts][i] = pkt[i]; s_snAck[pkts][i] = ack[i]; }
                    s_snLen[pkts] = (uint8)m;
                    s_snT[pkts]   = tPkt;
                }
                pkts++;
                {
                    uint32 i;
                    for (i = 0u; i < n; i++)
                        if ((i == 0u) || (ack[i - 1u] == 2u))
                            seen[(pkt[i] >> 1u) >> 5u] |= (1u << ((pkt[i] >> 1u) & 31u));
                }
            }
            sda = SNIFF_SDA();
            scl = SNIFF_SCL();
        }
 
        sdaPrev = sda;
        sclPrev = scl;
 
        /* housekeeping only while bus idle */
        if (sda && scl)
        {
            Tlf35585_ServiceWdt();
            if (asc != NULL_PTR)
            {
                uint8 ch; Ifx_SizeT c = 1u;
                if (IfxAsclin_Asc_read(asc, &ch, &c, 0u) && (c == 1u) && (ch == 0x1Bu))
                {
                    aborted = TRUE;
                    break;
                }
            }
        }
    }
 
    I2cMaster_ReinitBus(0u);
     {
        uint32 p, i, m = (pkts < SNIFF_LOG) ? pkts : SNIFF_LOG;
        for (p = 0u; p < m; p++)
        {
            Debug_Printf("[SNIFF] #%u @%ums:", (unsigned)(p + 1u), (unsigned)s_snT[p]);
            for (i = 0u; i < s_snLen[p]; i++)
            {
                if (s_snAck[p][i] == 2u) { Debug_Print(" Sr"); continue; }
                if ((i == 0u) || (s_snAck[p][i - 1u] == 2u))
                    Debug_Printf(" [%02X %c%s]", (unsigned)(s_snPkt[p][i] >> 1u),
                                 (s_snPkt[p][i] & 1u) ? 'R' : 'W', s_snAck[p][i] ? "" : " NAK");
                else
                    Debug_Printf(" %02X%s", (unsigned)s_snPkt[p][i], s_snAck[p][i] ? "" : "n");
            }
            Debug_Print(" P\r\n");
        }
    }
    Debug_Printf("[SNIFF] %s, %u packet(s). Addresses seen:",
                 aborted ? "aborted" : "done", (unsigned)pkts);
    {
        uint32 i;
        for (i = 0u; i < 128u; i++)
            if (seen[i >> 5u] & (1u << (i & 31u)))
                Debug_Printf(" 0x%02X", (unsigned)i);
    }
    Debug_Print("\r\n");
}

static void prv_CmdI2cSlaveScan(const char *args)
{
    Ifx_I2C *i2c = &CLI_SLAVE_MODULE;
    const char *a = prv_SkipSpaces(args);
    uint32 dwell = 100u, pass, addr7, hits = 0u;

    if (*a != '\0') dwell = prv_Atoi(a);

    Debug_Printf("[I2CSS] slave sweep 0x08-0x77, %ums each, keep the cable plugged in\r\n",
                 (unsigned)dwell);

    for (pass = 0u; pass < 2u; pass++)
    {
        Debug_Printf("[I2CSS] pass %u: ADR = addr %s\r\n",
                     (unsigned)pass, pass ? "unshifted" : "<< 1");
        for (addr7 = 0x08u; addr7 <= 0x77u; addr7++)
        {
            uint32 t0, am = 0u, rx = 0u;

            i2c->RUNCTRL.B.RUN  = 0u;
            i2c->ADDRCFG.B.MNS  = 0u;
            i2c->ADDRCFG.B.TBAM = 0u;
            i2c->ADDRCFG.B.ADR  = pass ? addr7 : (addr7 << 1u);
            i2c->ADDRCFG.B.GCE  = 0u;
            i2c->ADDRCFG.B.MCE  = 0u;
            i2c->ADDRCFG.B.SOPE = 1u;
            i2c->ADDRCFG.B.SONA = 1u;
            i2c->PIRQSC.U       = 0xFFFFFFFFu;
            i2c->ERRIRQSC.U     = 0xFFFFFFFFu;
            i2c->RUNCTRL.B.RUN  = 1u;

            t0 = Stm_GetTimeMs();
            while ((Stm_GetTimeMs() - t0) < dwell)
            {
                uint32 ffs = i2c->FFSSTAT.B.FFS;
                if (i2c->PIRQSS.B.AM) { am++; i2c->PIRQSC.B.AM = 1u; }
                while (ffs--) { (void)i2c->RXD.U; rx++; }
                if (i2c->PIRQSS.U) i2c->PIRQSC.U = i2c->PIRQSS.U;
                if (i2c->ERRIRQSS.U) i2c->ERRIRQSC.U = i2c->ERRIRQSS.U;
            }
            if (am || rx)
            {
                Debug_Printf("[I2CSS]   0x%02X: %u match(es), %u byte(s)\r\n",
                             (unsigned)addr7, (unsigned)am, (unsigned)rx);
                hits++;
            }
            if ((addr7 & 0x07u) == 0u) Tlf35585_ServiceWdt();
        }
    }

    I2cMaster_ReinitBus(0u);
    Debug_Printf("[I2CSS] done, %u address(es) with traffic; bus0 restored\r\n", (unsigned)hits);
}

static void prv_CmdI2cSlave(const char *args)
{
    Ifx_I2C       *i2c  = &CLI_SLAVE_MODULE;
    IfxAsclin_Asc *asc  = Debug_GetAscHandle();
    const char    *a    = prv_SkipSpaces(args);
    uint32 addr7, secs = 30u, t0;
    uint32 matches = 0u, bytes = 0u;
    boolean aborted = FALSE;

    if (*a == '\0')
    {
        Debug_Print("[I2CS] usage: i2cslave <hexaddr7> [secs]   e.g. i2cslave 0x40 60\r\n");
        return;
    }
    addr7 = prv_AtoiHex(a);
    while (*a && *a != ' ') a++;
    a = prv_SkipSpaces(a);
    if (*a != '\0') secs = prv_Atoi(a);
    if ((addr7 < 0x08u) || (addr7 > 0x77u))
    {
        Debug_Print("[I2CS] addr must be 0x08..0x77\r\n");
        return;
    }

    /* reconfigure module as 7-bit slave */
    i2c->RUNCTRL.B.RUN  = 0u;
    i2c->ADDRCFG.B.MNS  = 0u;                /* 0 = slave            */
    i2c->ADDRCFG.B.TBAM = 0u;                /* 7-bit addressing     */
    i2c->ADDRCFG.B.ADR  = (addr7 << 1u);     /* ADR[7:1] = address   */
    i2c->ADDRCFG.B.GCE  = 0u;
    i2c->ADDRCFG.B.MCE  = 0u;
    i2c->ADDRCFG.B.SOPE = 1u;                /* stop on packet end   */
    i2c->ADDRCFG.B.SONA = 1u;                /* stop on NACK         */
    i2c->FIFOCFG.B.RXFA = 0u;                /* byte-aligned RX FIFO */
    i2c->FIFOCFG.B.RXFC = 1u;                /* flow controlled      */
    i2c->FIFOCFG.B.RXBS = 3u;                /* burst 8              */
    i2c->PIRQSC.U       = 0xFFFFFFFFu;
    i2c->ERRIRQSC.U     = 0xFFFFFFFFu;
    i2c->RUNCTRL.B.RUN  = 1u;

    Debug_Printf("[I2CS] bus0 slave @0x%02X for %us, plug a Type-C device in. ESC to stop.\r\n",
                 (unsigned)addr7, (unsigned)secs);

    t0 = Stm_GetTimeMs();
    while ((Stm_GetTimeMs() - t0) < (secs * 1000u))
    {
        uint32 pirq = i2c->PIRQSS.U;
        uint32 eirq = i2c->ERRIRQSS.U;
        uint32 ffs  = i2c->FFSSTAT.B.FFS;    /* bytes waiting in RX FIFO */

        if (pirq & (1u << 0u))               /* AM: address match */
        {
            matches++;
            Debug_Printf("[I2CS] addr match #%u @%ums\r\n",
                         (unsigned)matches, (unsigned)(Stm_GetTimeMs() - t0));
        }
        while (ffs--)
        {
            uint8 d = (uint8)i2c->RXD.U;
            Debug_Printf("[I2CS]   rx 0x%02X\r\n", (unsigned)d);
            bytes++;
        }
        if (pirq & ~(1u << 0u))
            Debug_Printf("[I2CS] PIRQSS=0x%08X\r\n", (unsigned)pirq);
        if (pirq) i2c->PIRQSC.U = pirq;
        if (eirq)
        {
            Debug_Printf("[I2CS] ERRIRQSS=0x%08X\r\n", (unsigned)eirq);
            i2c->ERRIRQSC.U = eirq;
        }

        if (asc != NULL_PTR)                 /* ESC aborts */
        {
            uint8 ch; Ifx_SizeT n = 1u;
            if (IfxAsclin_Asc_read(asc, &ch, &n, 0u) && (n == 1u) && (ch == 0x1Bu))
            {
                aborted = TRUE;
                break;
            }
        }

        Tlf35585_ServiceWdt();
    }

    I2cMaster_ReinitBus(0u);                 /* back to master mode */
    Debug_Printf("[I2CS] %s: %u addr match(es), %u byte(s); bus0 restored\r\n",
                 aborted ? "aborted" : "done",
                 (unsigned)matches, (unsigned)bytes);
}

static void prv_CmdI2cTest(const char *args)
{
    IfxAsclin_Asc *asc = Debug_GetAscHandle();
    const char *a = prv_SkipSpaces(args);
    uint32 addr7 = 0x40u, secs = 60u, shift = 1u, t0, i, m;
    boolean aborted = FALSE;

    if (*a != '\0') { addr7 = prv_AtoiHex(a); while (*a && *a != ' ') a++; a = prv_SkipSpaces(a); }
    if (*a != '\0') { secs  = prv_Atoi(a);    while (*a && *a != ' ') a++; a = prv_SkipSpaces(a); }
    if (*a != '\0') { shift = prv_Atoi(a); }

    /* 1. master scan */
    Debug_Print("[I2CT] step 1: master scan of bus 0\r\n");
    prv_CmdI2cScan("0");

    /* 2. retimer emulator */
    Debug_Printf("[I2CT] step 2: emulating retimer @0x%02X (ADR %s) for %us. Plug a DP alt-mode device in. ESC to stop.\r\n",
                 (unsigned)addr7, shift ? "<<1" : "unshifted", (unsigned)secs);
    I2cSlave_Setup(&s_cliSlave, &MODULE_I2C0, (uint8)addr7, (uint8)shift, NULL_PTR);
    I2cSlave_Start(&s_cliSlave);

    t0 = Stm_GetTimeMs();
    while ((Stm_GetTimeMs() - t0) < (secs * 1000u))
    {
        I2cSlave_Poll(&s_cliSlave);
        if (!s_cliSlave.busy)
        {
            uint8 ch; Ifx_SizeT n = 1u;
            Tlf35585_ServiceWdt();
            if ((asc != NULL_PTR) && IfxAsclin_Asc_read(asc, &ch, &n, 0u) && (n == 1u) && (ch == 0x1Bu))
            { aborted = TRUE; break; }
        }
    }
    I2cSlave_Stop(&s_cliSlave);
    I2cMaster_ReinitBus(0u);

    /* 3. report */
    Debug_Printf("[I2CT] %s: %u addr match, %u rx, %u tx, %u nack\r\n",
                 aborted ? "aborted" : "done",
                 (unsigned)s_cliSlave.matches, (unsigned)s_cliSlave.rxBytes,
                 (unsigned)s_cliSlave.txBytes, (unsigned)s_cliSlave.nacks);
    m = (s_cliSlave.logCount < I2C_SLV_LOG_LEN) ? s_cliSlave.logCount : I2C_SLV_LOG_LEN;
    for (i = 0u; i < m; i++)
    {
        const I2cSlave_LogEntry_t *e = &s_cliSlave.log[i];
        if (e->isWrite && (e->data == 0xFFu) && (i + 1u < m) && s_cliSlave.log[i + 1u].isWrite)
            continue;                                   /* skip bare reg-select lines followed by data */
        Debug_Printf("[I2CT]  @%6ums %s reg 0x%02X %s 0x%02X\r\n",
                     (unsigned)(e->tMs - t0), e->isWrite ? "W" : "R",
                     (unsigned)e->reg, e->isWrite ? "<-" : "->", (unsigned)e->data);
    }
    if (s_cliSlave.logCount > I2C_SLV_LOG_LEN)
        Debug_Printf("[I2CT]  ... %u more not logged\r\n", (unsigned)(s_cliSlave.logCount - I2C_SLV_LOG_LEN));
    Debug_Print("[I2CT] register file (non-zero):");
    for (i = 0u; i < 256u; i++)
        if (s_cliSlave.regFile[i]) Debug_Printf(" [%02X]=%02X", (unsigned)i, (unsigned)s_cliSlave.regFile[i]);
    Debug_Print("\r\n");
}
/* ================================================================== */
/*  Command dispatch                                                  */
/* ================================================================== */

static void prv_Dispatch(const char *cmd)
{
    const char *args;

    if (cmd[0] == '\0')
        return;

    if (prv_StrEq(cmd, "help") || prv_StrEq(cmd, "?"))
        prv_CmdHelp();
    else if (prv_StrEq(cmd, "status"))
        prv_CmdStatus();
    else if (prv_StrEq(cmd, "poweron"))
        prv_CmdPowerOn();
    else if (prv_StrEq(cmd, "poweroff"))
        prv_CmdPowerOff();
    else if (prv_StrEq(cmd, "forceoff"))
        prv_CmdForceOff();
    else if (prv_StrEq(cmd, "warmreset"))
        prv_CmdWarmReset();
    else if (prv_StrEq(cmd, "coldreset"))
        prv_CmdColdReset();
    else if (prv_StrEq(cmd, "clearfault"))
        prv_CmdClearFault();
    else if (prv_StrEq(cmd, "tlf"))
        prv_CmdTlf();
    else if (prv_StrEq(cmd, "vmon"))
        prv_CmdVmon();
    else if ((args = prv_StartsWith(cmd, "nvlog")) != NULL_PTR)
        prv_CmdNvlog(args);
    else if (prv_StrEq(cmd, "fusa"))
        prv_CmdFusa();
    else if (prv_StrEq(cmd, "bist"))
        prv_CmdBist();
    else if (((args = prv_StartsWith(cmd, "i2cscan")) != NULL_PTR) && ((*args == ' ') || (*args == '\0')))
        prv_CmdI2cScan(args);

    else if (((args = prv_StartsWith(cmd, "i2cid")) != NULL_PTR) && ((*args == ' ') || (*args == '\0')))
        prv_CmdI2cId(args);

    else if (((args = prv_StartsWith(cmd, "i2cread8")) != NULL_PTR) && ((*args == ' ') || (*args == '\0')))
        prv_CmdI2cRead(args);

    else if (((args = prv_StartsWith(cmd, "i2cread")) != NULL_PTR) && ((*args == ' ') || (*args == '\0')))
        prv_CmdI2cRead(args);

    else if (((args = prv_StartsWith(cmd, "pin")) != NULL_PTR) && ((*args == ' ') || (*args == '\0')))
        prv_CmdPin();

    else if (((args = prv_StartsWith(cmd, "i2csniff")) != NULL_PTR) && ((*args == ' ') || (*args == '\0')))
        prv_CmdI2cSniff(args);

    else if (((args = prv_StartsWith(cmd, "i2cslave")) != NULL_PTR) && ((*args == ' ') || (*args == '\0')))
        prv_CmdI2cSlave(args);

    else if (((args = prv_StartsWith(cmd, "i2cslavescan")) != NULL_PTR) && ((*args == ' ') || (*args == '\0')))
        prv_CmdI2cSlaveScan(args);

    else if (((args = prv_StartsWith(cmd, "i2ctest")) != NULL_PTR) && ((*args == ' ') || (*args == '\0')))
        prv_CmdI2cTest(args);
        
    else if (prv_StrEq(cmd, "i2cstat"))
        prv_CmdI2cStat();
    else if (((args = prv_StartsWith(cmd, "i2creset")) != NULL_PTR) && ((*args == ' ') || (*args == '\0')))
        prv_CmdI2cReset(args);
    else if (((args = prv_StartsWith(cmd, "sysmon")) != NULL_PTR) && ((*args == ' ') || (*args == '\0')))
        prv_CmdSysmon(args);
    else if (((args = prv_StartsWith(cmd, "mux")) != NULL_PTR) && ((*args == ' ') || (*args == '\0')))
        prv_CmdMux(args);
    else if (((args = prv_StartsWith(cmd, "test-nvlog")) != NULL_PTR) && ((*args == ' ') || (*args == '\0')))
        SelfTest_NvLogStress();
    else if (((args = prv_StartsWith(cmd, "i2cprobe")) != NULL_PTR) && ((*args == ' ') || (*args == '\0')))
        prv_CmdI2cProbe();
    else if (((args = prv_StartsWith(cmd, "i2cdiag")) != NULL_PTR) && ((*args == ' ') || (*args == '\0')))
        prv_CmdI2cDiag();
    else if (((args = prv_StartsWith(cmd, "i2cbb")) != NULL_PTR) && ((*args == ' ') || (*args == '\0')))
        prv_CmdI2cBitbang();                                     
    else if (prv_StrEq(cmd, "uptime"))
        prv_CmdUptime();
    else if (prv_StrEq(cmd, "uptime"))
        prv_CmdUptime();
    else if (prv_StrEq(cmd, "version"))
        prv_CmdVersion();
    else if ((args = prv_StartsWith(cmd, "selftest")) != NULL_PTR)
        SelfTest_CliDispatch(args);
    else if (prv_StrEq(args, "hpd"))
        SelfTest_UsbPdHpd();
    else if (prv_StrEq(args, "topo"))
        SelfTest_UsbPdTopology();
    else if (prv_StrEq(args, "usbpd_edge"))
        SelfTest_UsbPdEdgeCases();
    else if (prv_StrEq(cmd, "usbpd"))
        prv_CmdUsbPd();
    else if (prv_StrEq(cmd, "temp"))
        prv_CmdTemp();
    else if (((args = prv_StartsWith(cmd, "autoboot")) != NULL_PTR) && ((*args == ' ') || (*args == '\0')))   /* ADD */
        prv_CmdAutoboot(args);                                                                                 /* ADD */
    else if (((args = prv_StartsWith(cmd, "sched")) != NULL_PTR) && ((*args == ' ') || (*args == '\0')))
        prv_CmdSched(args);
    else if (prv_StrEq(cmd, "fwupdate"))
        prv_CmdFwUpdate();                                                                                  /* ADD */
    else
        Debug_Printf("  Unknown command: '%s'. Type 'help'.\r\n", cmd);
}




/* =========1========================================================= */
/*  Public API                                                        */
/* ================================================================== */

void DebugCli_Init(void)
{
    s_cmdLen = 0u;
    s_cmdBuf[0] = '\0';
    s_initialised = TRUE;
    s_cliUpMs = Stm_GetTimeMs();
    if (s_autobootArmed)
        Debug_Print(" AUTOBOOT in 5s — press any key to cancel\r\n");
    Debug_Print("\r\n");
    Debug_Print("=================================\r\n");
    Debug_Print(" TC387 COM-HPC Controller CLI\r\n");
    Debug_Print(" Type 'help' for commands\r\n");
    Debug_Print("=================================\r\n");
    Debug_Print(CLI_PROMPT);
}

void DebugCli_Run(void)
{
    IfxAsclin_Asc *asc;
    uint8 ch;
    Ifx_SizeT count;

    if (!s_initialised) return;

    asc = Debug_GetAscHandle();
    if (asc == NULL_PTR) return;

    /* Process all available characters */
    while (1)
    {
        count = 1u;
        if (!IfxAsclin_Asc_read(asc, &ch, &count, 0u) || count == 0u)
            break;
        if (s_autobootArmed && !s_autobootDone)
        {
            s_autobootArmed = FALSE;
            Debug_Print("\r\n  [autoboot cancelled]\r\n");
        }
        /* Echo the character */
        {
            Ifx_SizeT echoCount = 1u;
            IfxAsclin_Asc_write(asc, &ch, &echoCount, 0u);
        }

        if (ch == '\r' || ch == '\n')
        {
            /* End of command */
            Debug_Print("\r\n");
            s_cmdBuf[s_cmdLen] = '\0';
            prv_Dispatch(s_cmdBuf);
            s_cmdLen = 0u;
            Debug_Print(CLI_PROMPT);
        }
        else if (ch == '\b' || ch == 0x7Fu)
        {
            /* Backspace */
            if (s_cmdLen > 0u)
            {
                s_cmdLen--;
                Debug_Print(" \b");  /* Overwrite + back */
            }
        }
        else if (ch >= ' ' && s_cmdLen < CLI_MAX_CMD_LEN)
        {
            /* Normal character */
            s_cmdBuf[s_cmdLen++] = (char)ch;
        }
    }
}




void DebugCli_Poll(void)
{
    uint32 now = Stm_GetTimeMs();

    /* ---- Autoboot ------------------------------------------------- */
    if (s_autobootArmed && !s_autobootDone
        && ((now - s_cliUpMs) >= AUTOBOOT_DELAY_MS)
        && g_ipcShared.cpu1Ready
        && (g_ipcShared.pmc.pmState == (uint32)PM_STATE_OFF))
    {
        s_autobootDone = TRUE;
        Debug_Print("[SYS] AUTOBOOT: requesting power on\r\n");
        NvLog_WriteU32(NVLOG_EVT_BOOT, NVLOG_SRC_SYSTEM, NVLOG_SEV_INFO, 0xAB007u);
        if (!Ipc_SendCommandWait(IPC_CMD_POWER_ON, 0u, 200u))
            Debug_Print("[SYS] AUTOBOOT: CPU1 did not ack\r\n");
    }

    if (g_ipcShared.pmc.pmState == (uint32)PM_STATE_ON)
    {
        if (s_onSeenMs == 0u) s_onSeenMs = now;

        /* ---- Scheduled command ------------------------------------ */
        if ((s_schedCmd != 0u)
            && ((now - s_onSeenMs) >= s_schedDelayMs))
        {
            uint32 cmd = s_schedCmd;
            s_schedCmd = 0u;                     /* one shot */
            Debug_Printf("[SYS] SCHED: sending cmd %u\r\n", (unsigned)cmd);
            NvLog_WriteU32(NVLOG_EVT_SHUTDOWN_OPERATOR, NVLOG_SRC_DEBUG,
                           NVLOG_SEV_INFO, cmd);
            if (!Ipc_SendCommandWait((Ipc_Command_t)cmd, 0u, 200u))
                Debug_Print("[SYS] SCHED: CPU1 did not ack\r\n");
        }
    }
    else
    {
        s_onSeenMs     = 0u;
    }
}