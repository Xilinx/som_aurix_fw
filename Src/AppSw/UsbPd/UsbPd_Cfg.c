/**
 * @file    UsbPd_Cfg.c
 * @brief   CYPD6129 device configuration table — runtime initialisation.
 *
 * AppPin_t members are uint8 only — struct assignment from extern const
 * AppPin_t objects is a plain runtime copy, no Tasking E306 restriction.
 */

#include "UsbPd_Cfg.h"
#include "DFlash.h"
#include "Uart_Debug.h"
#include <string.h>

Cypd_DevCfg_t CYPD_DEVICES[CYPD_DEVICE_COUNT];

/* v0.2 system config */
static UsbPd_SysCfg_t s_sysCfg;

static uint32 prv_CalcChecksum(const UsbPd_SysCfg_t *p)
{
    const uint32 *w = (const uint32 *)p;
    uint32 n = (sizeof(UsbPd_SysCfg_t) / 4u) - 1u;
    uint32 cs = 0u, i;
    for (i = 0u; i < n; i++) cs ^= w[i];
    return cs;
}

static void prv_SetDefaults(void)
{
    memset(&s_sysCfg, 0, sizeof(s_sysCfg));
    s_sysCfg.magic          = USBPD_CFG_MAGIC;
    s_sysCfg.version        = USBPD_CFG_VERSION;
    s_sysCfg.topology       = USBPD_TOPO_DUAL_SINGLE_PORT;
    s_sysCfg.numControllers = 2u;
    s_sysCfg.numTotalPorts  = 2u;
    s_sysCfg.apuSlvAddr0    = 0x54u;
    s_sysCfg.apuSlvAddr1    = 0x58u;
    s_sysCfg.ctrlAddr[0]    = CYPD_PORT0_I2C_ADDR;
    s_sysCfg.ctrlAddr[1]    = CYPD_PORT1_I2C_ADDR;
    s_sysCfg.portBase[0]    = 0x1000u;
    s_sysCfg.portBase[1]    = 0x1000u;
    s_sysCfg.alertPollingMs = 5u;
    s_sysCfg.checksum       = prv_CalcChecksum(&s_sysCfg);
}

void UsbPd_CfgInit(void)
{
    /* Try DFlash first */
    if (UsbPd_Cfg_LoadFromDFlash() != 0u)
    {
        Debug_Print("[USBPD_CFG] No valid DFlash config, using defaults\r\n");
        prv_SetDefaults();
    }
    else
    {
        Debug_Print("[USBPD_CFG] Loaded from DFlash\r\n");
    }

    /* Populate the existing CYPD_DEVICES[] table from sysCfg.
     * This preserves backward compatibility with Cypd6129_Drv
     * and UsbPd_Manager. */
    CYPD_DEVICES[0].i2cAddr  = s_sysCfg.ctrlAddr[0];
    CYPD_DEVICES[0].i2cBus   = 0u;
    CYPD_DEVICES[0].intPin   = PIN_CYPD0_INT_L;
    CYPD_DEVICES[0].resetPin = PIN_CYPD0_RESET_L;
    CYPD_DEVICES[0].name     = "CYPD_P0";

    if (s_sysCfg.numControllers >= 2u)
    {
        CYPD_DEVICES[1].i2cAddr  = s_sysCfg.ctrlAddr[1];
        CYPD_DEVICES[1].i2cBus   = 0u;
        CYPD_DEVICES[1].intPin   = PIN_CYPD1_INT_L;
        CYPD_DEVICES[1].resetPin = PIN_CYPD1_RESET_L;
        CYPD_DEVICES[1].name     = "CYPD_P1";
    }

    UsbPd_Cfg_Dump();
}

const UsbPd_SysCfg_t *UsbPd_Cfg_GetSysCfg(void)
{
    return &s_sysCfg;
}

uint8 UsbPd_Cfg_SaveToDFlash(void)
{
    s_sysCfg.checksum = prv_CalcChecksum(&s_sysCfg);
    DFlash_Status_t ds = DFlash_EraseSectors(DFLASH_USBCFG_ADDR, 1u);
    if (ds != DFLASH_OK) return 1u;
    ds = DFlash_Write(DFLASH_USBCFG_ADDR, &s_sysCfg, sizeof(s_sysCfg));
    return (ds == DFLASH_OK) ? 0u : 2u;
}

uint8 UsbPd_Cfg_LoadFromDFlash(void)
{
    UsbPd_SysCfg_t loaded;
    DFlash_Status_t ds = DFlash_Read(DFLASH_USBCFG_ADDR, &loaded, sizeof(loaded));
    if (ds != DFLASH_OK) return 1u;
    if (loaded.magic != USBPD_CFG_MAGIC) return 2u;
    if (loaded.version != USBPD_CFG_VERSION) return 3u;
    if (loaded.checksum != prv_CalcChecksum(&loaded)) return 4u;
    s_sysCfg = loaded;
    return 0u;
}

void UsbPd_Cfg_ResetDefaults(void)
{
    prv_SetDefaults();
    UsbPd_Cfg_SaveToDFlash();
}

void UsbPd_Cfg_Dump(void)
{
    Debug_Printf("[USBPD_CFG] Topology: %s, ctrls=%u, ports=%u\r\n",
                 (s_sysCfg.topology == USBPD_TOPO_DUAL_SINGLE_PORT)
                     ? "Dual CYPD6129" : "Single CYPD6229",
                 (unsigned)s_sysCfg.numControllers,
                 (unsigned)s_sysCfg.numTotalPorts);
    Debug_Printf("[USBPD_CFG] APU slave: 0x%02X / 0x%02X\r\n",
                 (unsigned)s_sysCfg.apuSlvAddr0,
                 (unsigned)s_sysCfg.apuSlvAddr1);
}