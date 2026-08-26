/**
 * @file    UsbPd_Cfg.h
 * @brief   USB PD port configuration for two CYPD6129 devices.
 *
 * I2C addresses are defined in Platform_Cfg.h.
 * INT_L and RESET_L pin assignments are in Platform_PinCfg.h.
 */

#ifndef USBPD_CFG_H
#define USBPD_CFG_H

#include "Platform_Cfg.h"
#include "Platform_PinCfg.h"
#include "AppPin.h"

/** Total number of CYPD6129 devices on the board. */
#define CYPD_DEVICE_COUNT       2u
#define USBPD_FEATURE_ENABLE   1u

/**
 * @brief Static configuration for each CYPD6129 instance.
 */
typedef struct
{
    uint8        i2cAddr;   /* 7-bit I2C address */
    uint8        i2cBus;    /* 0 = I2C0, 1 = I2C1 */
    AppPin_t     intPin;    /* INT_L — active low input  */
    AppPin_t     resetPin;  /* RESET_L — active low output */
    const char  *name;      /* for debug logging */
} Cypd_DevCfg_t;

/**
 * Per-device configuration table — defined and populated in UsbPd_Cfg.c.
 * NOT const: Tasking ctc E306 rejects hardware register pointers
 * (IfxPort_Pin.port = &MODULE_Pxx) in any file-scope aggregate initialiser.
 * Table is filled at runtime by UsbPd_CfgInit() before first use.
 */
extern Cypd_DevCfg_t CYPD_DEVICES[CYPD_DEVICE_COUNT];

/**
 * @brief Populate CYPD_DEVICES[]. Must be called once before any
 *        Cypd_xxx() or UsbPdManager_xxx() call.
 */
void UsbPd_CfgInit(void);

/* ================================================================== */
/*  v0.2 System-level config (DFlash-backed)                          */
/* ================================================================== */

typedef enum
{
    USBPD_TOPO_DUAL_SINGLE_PORT = 0u,  /* Two CYPD6129s (default) */
    USBPD_TOPO_SINGLE_DUAL_PORT = 1u,  /* One CYPD6229 */
} UsbPd_Topology_t;

#define USBPD_CFG_MAGIC        0x55504443u  /* "UPDC" */
#define USBPD_CFG_VERSION      1u

typedef struct
{
    uint32              magic;
    uint32              version;
    UsbPd_Topology_t    topology;
    uint8               numControllers;
    uint8               numTotalPorts;
    uint8               apuSlvAddr0;    /* 0x54 default */
    uint8               apuSlvAddr1;    /* 0x58 default */
    uint8               ctrlAddr[2];    /* I2C addresses per controller */
    uint16              portBase[2];    /* HPI port bases per controller */
    uint32              alertPollingMs;
    uint32              checksum;
} UsbPd_SysCfg_t;

/** Get the system-level config (loaded from DFlash or defaults) */
const UsbPd_SysCfg_t *UsbPd_Cfg_GetSysCfg(void);

/** Save current config to DFlash USB config region */
uint8 UsbPd_Cfg_SaveToDFlash(void);

/** Load from DFlash; returns 0 on success */
uint8 UsbPd_Cfg_LoadFromDFlash(void);

/** Reset to compile-time defaults */
void UsbPd_Cfg_ResetDefaults(void);

/** Print config to debug UART */
void UsbPd_Cfg_Dump(void);


#endif /* USBPD_CFG_H */
