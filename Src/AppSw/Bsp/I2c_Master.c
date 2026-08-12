/**
 * @file    I2c_Master.c
 * @brief   Polling I2C master — iLLD 1.20.0 API (IfxI2c_I2c_write2 / read2).
 *
 * Key API differences from older iLLD versions:
 *   - write2 / read2 replace write / read (and are fully blocking — no getStatus needed)
 *   - Status enum: IfxI2c_I2c_Status_al (arbitration lost), _busNotFree (not _busy)
 *   - config.pins is IFX_CONST IfxI2c_Pins* (pointer, not embedded struct)
 *   - Repeated-start for combined write-then-read via device.enableRepeatedStart
 */

#include "I2c_Master.h"
#include "Stm_Timer.h"
#include "IfxI2c_I2c.h"
#include "IfxI2c_PinMap.h"

static IfxI2c_I2c s_i2cHandle;
static IfxI2c_I2c s_i2c1Handle;   /* I2C1 — APML (P11.14/P11.13) */
static IfxI2c_I2c_Device s_deviceHandle;
static IfxI2c_I2c_Device s_apmlDevHandle; /* I2C1 — APML (P11.14/P11.13) */


void I2cMaster_Init(void)
{
    IfxI2c_I2c_Config cfg;
    /* Local const — automatic storage, no E306 restriction on initialiser. */
    const IfxI2c_Pins pins = {
        &IfxI2c0_SCL_P13_1_INOUT,   /* P13.1 SCL */
        &IfxI2c0_SDA_P13_2_INOUT,   /* P13.2 SDA */
        IfxPort_PadDriver_cmosAutomotiveSpeed1
    };

    IfxI2c_I2c_initConfig(&cfg, &MODULE_I2C0);
    cfg.baudrate = (float32)I2C_MASTER_FREQ_HZ;
    cfg.mode     = IfxI2c_Mode_StandardAndFast;
    cfg.pins     = &pins;

    IfxI2c_I2c_initModule(&s_i2cHandle, &cfg);
    /* ---- I2C1: APML SB-TSI (P11.14 SCL / P11.13 SDA) ------------------- */
    const IfxI2c_Pins pins1 = {
        &IfxI2c1_SCL_P11_14_INOUT,
        &IfxI2c1_SDA_P11_13_INOUT,
        IfxPort_PadDriver_cmosAutomotiveSpeed1
    };

    IfxI2c_I2c_initConfig(&cfg, &MODULE_I2C1);
    cfg.baudrate = 400000.0f;   /* 400 kHz fast-mode per PPR §5.3.2 */
    cfg.mode     = IfxI2c_Mode_StandardAndFast;
    cfg.pins     = &pins1;

    IfxI2c_I2c_initModule(&s_i2c1Handle, &cfg);
}

/* Initialise device handle for a given 7-bit address. */
static void prv_SetDevice(uint8 addr7bit, boolean repeatedStart)
{
    IfxI2c_I2c_deviceConfig devCfg;
    IfxI2c_I2c_initDeviceConfig(&devCfg, &s_i2cHandle);
    devCfg.deviceAddress       = (uint16)((uint16)addr7bit << 1u); /* 8-bit shifted */
    devCfg.enableRepeatedStart = repeatedStart;
    IfxI2c_I2c_initDevice(&s_deviceHandle, &devCfg);
}

/* Map iLLD 1.20.0 status to our enum. */
static I2c_Status_t prv_MapStatus(IfxI2c_I2c_Status st)
{
    switch (st)
    {
        case IfxI2c_I2c_Status_ok:         return I2C_OK;
        case IfxI2c_I2c_Status_nak:        return I2C_ERR_NAK;
        case IfxI2c_I2c_Status_al:         return I2C_ERR_ARB_LOST;
        case IfxI2c_I2c_Status_busNotFree: return I2C_ERR_BUS_BUSY;
        default:                           return I2C_ERR_BUS_BUSY;
    }
}

I2c_Status_t I2cMaster_Write(uint8 addr7bit, const uint8 *pData, uint16 len)
{
    IfxI2c_I2c_Status st;
    prv_SetDevice(addr7bit, FALSE);
    /* write2 is blocking — returns when transfer completes or fails. */
    st = IfxI2c_I2c_write2(&s_deviceHandle, (volatile uint8 *)pData, (Ifx_SizeT)len);
    return prv_MapStatus(st);
}

I2c_Status_t I2cMaster_ReadReg16(uint8 addr7bit, uint16 regAddr,
                                   uint8 *pBuf,    uint16 len)
{
    uint8 addrBytes[2];
    IfxI2c_I2c_Status st;

    addrBytes[0] = (uint8)(regAddr & 0x00FFu);   /* little-endian HPI address */
    addrBytes[1] = (uint8)(regAddr >> 8u);

    /* Write phase: send register address with repeated-start (no STOP). */
    prv_SetDevice(addr7bit, TRUE);
    st = IfxI2c_I2c_write2(&s_deviceHandle, (volatile uint8 *)addrBytes, (Ifx_SizeT)2);
    if (st != IfxI2c_I2c_Status_ok)
    {
        return prv_MapStatus(st);
    }

    /* Read phase: repeated-START then read. */
    st = IfxI2c_I2c_read2(&s_deviceHandle, (volatile uint8 *)pBuf, (Ifx_SizeT)len);
    return prv_MapStatus(st);
}

I2c_Status_t I2cMaster_WriteReg16(uint8 addr7bit, uint16 regAddr,
                                    const uint8 *pData, uint16 len)
{
    /* Build [reg_addr_lo][reg_addr_hi][data...] in one buffer. */
    uint8  buf[2u + 32u];
    uint16 i;
    IfxI2c_I2c_Status st;

    if (len > 32u)
    {
        return I2C_ERR_BUS_BUSY;   /* reuse as invalid-arg */
    }

    buf[0] = (uint8)(regAddr & 0x00FFu);
    buf[1] = (uint8)(regAddr >> 8u);
    for (i = 0u; i < len; i++)
    {
        buf[2u + i] = pData[i];
    }

    prv_SetDevice(addr7bit, FALSE);
    st = IfxI2c_I2c_write2(&s_deviceHandle, (volatile uint8 *)buf, (Ifx_SizeT)(2u + len));
    return prv_MapStatus(st);
}

I2c_Status_t I2cMaster_ApmlReadByte(uint8 addr7bit, uint8 regAddr, uint8 *pData)
{
    IfxI2c_I2c_Status st;
    IfxI2c_I2c_deviceConfig devCfg;

    IfxI2c_I2c_initDeviceConfig(&devCfg, &s_i2c1Handle);
    devCfg.deviceAddress       = (uint16)((uint16)addr7bit << 1u);
    devCfg.enableRepeatedStart = TRUE;
    IfxI2c_I2c_initDevice(&s_apmlDevHandle, &devCfg);

    /* Write register address (repeated start, no STOP) */
    st = IfxI2c_I2c_write2(&s_apmlDevHandle,
                            (volatile uint8 *)&regAddr, (Ifx_SizeT)1);
    if (st != IfxI2c_I2c_Status_ok)
    {
        return prv_MapStatus(st);
    }

    /* Read one data byte */
    st = IfxI2c_I2c_read2(&s_apmlDevHandle,
                           (volatile uint8 *)pData, (Ifx_SizeT)1);
    return prv_MapStatus(st);
}

void I2cMaster_ReinitBus(uint8 busIdx)
{
    IfxI2c_I2c_Config cfg;

    if (busIdx == 0u)
    {
        const IfxI2c_Pins pins = {
            &IfxI2c0_SCL_P13_1_INOUT,
            &IfxI2c0_SDA_P13_2_INOUT,
            IfxPort_PadDriver_cmosAutomotiveSpeed1
        };
        IfxI2c_I2c_initConfig(&cfg, &MODULE_I2C0);
        cfg.baudrate = (float32)I2C_MASTER_FREQ_HZ;
        cfg.mode     = IfxI2c_Mode_StandardAndFast;
        cfg.pins     = &pins;
        IfxI2c_I2c_initModule(&s_i2cHandle, &cfg);
    }
    else if (busIdx == 1u)
    {
        const IfxI2c_Pins pins = {
            &IfxI2c1_SCL_P11_14_INOUT,
            &IfxI2c1_SDA_P11_13_INOUT,
            IfxPort_PadDriver_cmosAutomotiveSpeed1
        };
        IfxI2c_I2c_initConfig(&cfg, &MODULE_I2C1);
        cfg.baudrate = 400000.0f;
        cfg.mode     = IfxI2c_Mode_StandardAndFast;
        cfg.pins     = &pins;
        IfxI2c_I2c_initModule(&s_i2c1Handle, &cfg);
    }
}