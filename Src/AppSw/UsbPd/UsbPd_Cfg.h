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

/**
 * @brief Static configuration for each CYPD6129 instance.
 */
typedef struct
{
    uint8        i2cAddr;   /* 7-bit I2C address */
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

#endif /* USBPD_CFG_H */
