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

#if !defined(TARGET_EVAL_BOARD)
#include "SysMonitor.h"
#include "UsbPd_Manager.h"
#endif

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

/* ================================================================== */
/*  Private: string helpers                                           */
/* ================================================================== */

static boolean prv_StrEq(const char *a, const char *b)
{
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
#if !defined(TARGET_EVAL_BOARD)
        "  usbpd       USB PD port states\r\n"
        "  temp        APU temperature\r\n"
#endif
        "  uptime      Seconds since boot\r\n"
        "  version     Firmware version\r\n"
        "  help        This message\r\n"
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

#if !defined(TARGET_EVAL_BOARD)
    Debug_Printf("  PROCHOT:  %u\r\n",
                 (unsigned)SysMonitor_IsThrottling());
#endif

    Debug_Print("=====================\r\n");
}

static void prv_CmdPowerOn(void)
{
    if (PowerManager_GetState() != PM_STATE_OFF)
    {
        Debug_Printf("  Rejected: current state is %u (not OFF)\r\n",
                     (unsigned)PowerManager_GetState());
        return;
    }
    Debug_Print("  Requesting power on...\r\n");
    PowerManager_RequestPowerOn();
}

static void prv_CmdPowerOff(void)
{
    if (PowerManager_GetState() != PM_STATE_ON)
    {
        Debug_Printf("  Rejected: current state is %u (not ON)\r\n",
                     (unsigned)PowerManager_GetState());
        return;
    }
    Debug_Print("  Requesting power off...\r\n");
    NvLog_WriteU32(NVLOG_EVT_SHUTDOWN_OPERATOR, NVLOG_SRC_DEBUG,
                   NVLOG_SEV_INFO, 0u);
    PowerManager_RequestPowerOff();
}

static void prv_CmdForceOff(void)
{
    Debug_Print("  Forcing power off...\r\n");
    NvLog_WriteU32(NVLOG_EVT_SHUTDOWN_FORCED, NVLOG_SRC_DEBUG,
                   NVLOG_SEV_WARNING, 0u);
    NvLog_SealSlot(NVLOG_EVT_SHUTDOWN_FORCED);
    PowerManager_RequestPowerOff();  /* Same as graceful for now */
}

static void prv_CmdWarmReset(void)
{
    if (PowerManager_GetState() != PM_STATE_ON)
    {
        Debug_Printf("  Rejected: current state is %u (not ON)\r\n",
                     (unsigned)PowerManager_GetState());
        return;
    }
    Debug_Print("  Requesting warm reset...\r\n");
    NvLog_WriteU32(NVLOG_EVT_RESET_WARM, NVLOG_SRC_DEBUG,
                   NVLOG_SEV_INFO, 0u);
    PowerManager_RequestWarmReset();
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
    if (PowerManager_GetState() != PM_STATE_FAULT)
    {
        Debug_Print("  No fault to clear\r\n");
        return;
    }
    Debug_Print("  Clearing fault latch...\r\n");
    PowerManager_ClearFault();
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

#if !defined(TARGET_EVAL_BOARD)
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
#endif

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
    else if (prv_StrEq(cmd, "uptime"))
        prv_CmdUptime();
    else if (prv_StrEq(cmd, "version"))
        prv_CmdVersion();
#if !defined(TARGET_EVAL_BOARD)
    else if (prv_StrEq(cmd, "usbpd"))
        prv_CmdUsbPd();
    else if (prv_StrEq(cmd, "temp"))
        prv_CmdTemp();
#endif
    else
        Debug_Printf("  Unknown command: '%s'. Type 'help'.\r\n", cmd);
}

/* ================================================================== */
/*  Public API                                                        */
/* ================================================================== */

void DebugCli_Init(void)
{
    s_cmdLen = 0u;
    s_cmdBuf[0] = '\0';
    s_initialised = TRUE;

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