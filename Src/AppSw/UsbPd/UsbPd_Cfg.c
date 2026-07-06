/**
 * @file    UsbPd_Cfg.c
 * @brief   CYPD6129 device configuration table — runtime initialisation.
 *
 * AppPin_t members are uint8 only — struct assignment from extern const
 * AppPin_t objects is a plain runtime copy, no Tasking E306 restriction.
 */

#include "UsbPd_Cfg.h"

Cypd_DevCfg_t CYPD_DEVICES[CYPD_DEVICE_COUNT];

void UsbPd_CfgInit(void)
{
    CYPD_DEVICES[0].i2cAddr  = CYPD_PORT0_I2C_ADDR;
    CYPD_DEVICES[0].intPin   = PIN_CYPD0_INT_L;
    CYPD_DEVICES[0].resetPin = PIN_CYPD0_RESET_L;
    CYPD_DEVICES[0].name     = "CYPD_P0";

    CYPD_DEVICES[1].i2cAddr  = CYPD_PORT1_I2C_ADDR;
    CYPD_DEVICES[1].intPin   = PIN_CYPD1_INT_L;
    CYPD_DEVICES[1].resetPin = PIN_CYPD1_RESET_L;
    CYPD_DEVICES[1].name     = "CYPD_P1";
}
