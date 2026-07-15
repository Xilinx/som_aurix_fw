/**
 * @file    VoltMon.c
 * @brief   EVADC voltage monitoring — scan, threshold check, fault report.
 *
 * Uses iLLD IfxEvadc_Adc API with queue-based scan mode.
 *
 * Channel table is defined statically below.  Divider scaling converts
 * raw ADC counts to actual rail voltage in millivolts.  Thresholds are
 * preliminary — update from AMD 58241 §16 datasheet min/max once
 * available.
 *
 */

#include "VoltMon.h"
#include "Uart_Debug.h"
#include "Stm_Timer.h"
#include "IfxEvadc_Adc.h"

/* All analog inputs use the same sense topology:
 *
 * dividerScale = 1000 for all channels.
 * VREF = 5.0V external precision reference.
 *
 * GND_REF channels (AN4, AN11, AN19, AN30) use 0R to GND
 * for zero-offset calibration.
 *
 * Source: Sapphire SoM schematic, U54F ADC Groups sheet.
 */
#define UV_WARN(nom)    ((uint16)((nom) * 92u / 100u))
#define UV_FAULT(nom)   ((uint16)((nom) * 90u / 100u))
#define OV_WARN(nom)    ((uint16)((nom) * 108u / 100u))
#define OV_FAULT(nom)   ((uint16)((nom) * 110u / 100u))

#ifndef IFXEVADC_QUEUE_REFILL
#define IFXEVADC_QUEUE_REFILL  (1u)   /* auto-refill queue entry after conversion */
#endif

/* clang-format off */
static const VoltMon_ChCfg_t s_chTable[] =
{
    /* ---- Group 0: VID rails (S0) ---------------------------------------- */
    { "VDDCR",       0u, 0u, 0u, 1100u, UV_WARN(1100u), UV_FAULT(1100u), OV_WARN(1100u), OV_FAULT(1100u), 1000u },
    { "VDDCR_CCD",   0u, 1u, 1u, 1100u, UV_WARN(1100u), UV_FAULT(1100u), OV_WARN(1100u), OV_FAULT(1100u), 1000u },
    { "VDDCR_SOC",   0u, 2u, 2u, 1100u, UV_WARN(1100u), UV_FAULT(1100u), OV_WARN(1100u), OV_FAULT(1100u), 1000u },
    { "VDDCR_SR",    0u, 3u, 3u, 1100u, UV_WARN(1100u), UV_FAULT(1100u), OV_WARN(1100u), OV_FAULT(1100u), 1000u },

    /* ---- Group 1: Memory channel A (S0) --------------------------------- */
    { "VDD_MEM_A",    1u, 0u, 0u, 1100u, UV_WARN(1100u), UV_FAULT(1100u), OV_WARN(1100u), OV_FAULT(1100u), 1000u },
    { "VDD_MEMQ_A",   1u, 1u, 1u, 1100u, UV_WARN(1100u), UV_FAULT(1100u), OV_WARN(1100u), OV_FAULT(1100u), 1000u },
    { "VDDIO_MEM_A",  1u, 2u, 2u, 1100u, UV_WARN(1100u), UV_FAULT(1100u), OV_WARN(1100u), OV_FAULT(1100u), 1000u },

    /* ---- Group 2: Memory channel B (S0) --------------------------------- */
    { "VDD_MEM_B",    2u, 0u, 0u, 1100u, UV_WARN(1100u), UV_FAULT(1100u), OV_WARN(1100u), OV_FAULT(1100u), 1000u },
    { "VDD_MEMQ_B",   2u, 1u, 1u, 1100u, UV_WARN(1100u), UV_FAULT(1100u), OV_WARN(1100u), OV_FAULT(1100u), 1000u },
    { "VDDIO_MEM_B",  2u, 2u, 2u, 1100u, UV_WARN(1100u), UV_FAULT(1100u), OV_WARN(1100u), OV_FAULT(1100u), 1000u },

    /* ---- Group 3: Misc / S5 rails --------------------------------------- */
    { "VDD_MISC",     3u, 0u, 0u,  750u, UV_WARN( 750u), UV_FAULT( 750u), OV_WARN( 750u), OV_FAULT( 750u), 1000u },
    { "VDD_MISC_S5",  3u, 1u, 1u,  750u, UV_WARN( 750u), UV_FAULT( 750u), OV_WARN( 750u), OV_FAULT( 750u), 1000u },
    { "VDD_1V2",      3u, 2u, 2u, 1200u, UV_WARN(1200u), UV_FAULT(1200u), OV_WARN(1200u), OV_FAULT(1200u), 1000u },
    { "VDD_1V2_S5",   3u, 3u, 3u, 1200u, UV_WARN(1200u), UV_FAULT(1200u), OV_WARN(1200u), OV_FAULT(1200u), 1000u },
    { "VDD_1V8",      3u, 4u, 4u, 1800u, UV_WARN(1800u), UV_FAULT(1800u), OV_WARN(1800u), OV_FAULT(1800u), 1000u },
    { "VDD_1V8_S5",   3u, 5u, 5u, 1800u, UV_WARN(1800u), UV_FAULT(1800u), OV_WARN(1800u), OV_FAULT(1800u), 1000u },

    /* ---- Group 4: I/O rails --------------------------------------------- */
    { "VDDIO_3V3",    4u, 0u, 0u, 3300u, UV_WARN(3300u), UV_FAULT(3300u), OV_WARN(3300u), OV_FAULT(3300u), 1000u },
    { "VDDIO_3V3_S5", 4u, 1u, 1u, 3300u, UV_WARN(3300u), UV_FAULT(3300u), OV_WARN(3300u), OV_FAULT(3300u), 1000u },
    { "VDDIO_AUDIO",  4u, 2u, 2u, 3300u, UV_WARN(3300u), UV_FAULT(3300u), OV_WARN(3300u), OV_FAULT(3300u), 1000u },
    { "VDDIO_MEM_VAA",4u, 3u, 3u, 1800u, UV_WARN(1800u), UV_FAULT(1800u), OV_WARN(1800u), OV_FAULT(1800u), 1000u },
};
/* clang-format on */

#define VOLTMON_CH_COUNT  (sizeof(s_chTable) / sizeof(s_chTable[0]))

/* ---- EVADC iLLD handles ------------------------------------------------ */

static IfxEvadc_Adc          s_evadc;

/* We need one group handle per EVADC group used (0-4).
 * TC387 has groups 0-11, but we only use 0-4. */
#define VOLTMON_NUM_GROUPS   5u

static IfxEvadc_Adc_Group    s_groups[VOLTMON_NUM_GROUPS];
static IfxEvadc_Adc_Channel  s_channels[VOLTMON_CH_COUNT];

/* Last measured values in mV */
static uint16                s_lastMv[VOLTMON_CH_COUNT];
static VoltMon_FaultCb_t     s_faultCb = NULL_PTR;
static boolean               s_initialised = FALSE;

/* ---- ADC to mV conversion ----------------------------------------------- */

static uint16 prv_CountsToRailMv(uint16 counts, uint16 dividerScale)
{
    /* ADC voltage = counts * VREF / ADC_MAX
     * Rail voltage = ADC_voltage * dividerScale / 1000
     * Combined: rail_mV = counts * VREF_mV * dividerScale / (ADC_MAX * 1000) */
    uint32 num;

    num = (uint32)counts * VOLTMON_VREF_MV;
    num = (num * dividerScale) / (VOLTMON_ADC_MAX * 1000u);

    return (uint16)num;
}

/* ---- Threshold check ---------------------------------------------------- */

static void prv_CheckThresholds(uint8 chIdx, uint16 measuredMv)
{
    const VoltMon_ChCfg_t *ch;
    VoltMon_Severity_t severity;

    ch = &s_chTable[chIdx];
    severity = VOLTMON_OK;

    if (measuredMv < ch->uvFaultMv)
    {
        severity = VOLTMON_FAULT;
        Debug_Printf("[VMON] FAULT UV: %s = %umV (min %umV)\r\n",
                     ch->name, (unsigned)measuredMv, (unsigned)ch->uvFaultMv);
    }
    else if (measuredMv < ch->uvWarnMv)
    {
        severity = VOLTMON_WARNING;
        Debug_Printf("[VMON] WARN UV: %s = %umV\r\n",
                     ch->name, (unsigned)measuredMv);
    }
    else if (measuredMv > ch->ovFaultMv)
    {
        severity = VOLTMON_FAULT;
        Debug_Printf("[VMON] FAULT OV: %s = %umV (max %umV)\r\n",
                     ch->name, (unsigned)measuredMv, (unsigned)ch->ovFaultMv);
    }
    else if (measuredMv > ch->ovWarnMv)
    {
        severity = VOLTMON_WARNING;
        Debug_Printf("[VMON] WARN OV: %s = %umV\r\n",
                     ch->name, (unsigned)measuredMv);
    }

    if ((severity >= VOLTMON_FAULT) && (s_faultCb != NULL_PTR))
    {
        s_faultCb(ch, measuredMv, severity);
    }
}

/* ---- Public API --------------------------------------------------------- */

void VoltMon_Init(void)
{
    IfxEvadc_Adc_Config        adcCfg;
    IfxEvadc_Adc_GroupConfig   grpCfg;
    IfxEvadc_Adc_ChannelConfig chCfg;
    uint8 g;
    uint8 i;

    Debug_Print("[VMON] Init: configuring EVADC...\r\n");

    /* ---- Module init ---------------------------------------------------- */
    IfxEvadc_Adc_initModuleConfig(&adcCfg, &MODULE_EVADC);
    IfxEvadc_Adc_initModule(&s_evadc, &adcCfg);

    /* ---- Group init ----------------------------------------------------- */
    for (g = 0u; g < VOLTMON_NUM_GROUPS; g++)
    {
        IfxEvadc_Adc_initGroupConfig(&grpCfg, &s_evadc);

        grpCfg.groupId = (IfxEvadc_GroupId)g;
        grpCfg.master  = grpCfg.groupId;

        /* Enable queue 0 as the request source */
        grpCfg.arbiter.requestSlotQueue0Enabled = TRUE;

        /* Software-triggered: gate always open, no external trigger */
        grpCfg.queueRequest[0].triggerConfig.gatingMode =
            IfxEvadc_GatingMode_always;

        IfxEvadc_Adc_initGroup(&s_groups[g], &grpCfg);
    }

    /* ---- Channel init --------------------------------------------------- */
    for (i = 0u; i < (uint8)VOLTMON_CH_COUNT; i++)
    {
        IfxEvadc_Adc_initChannelConfig(&chCfg,
                                        &s_groups[s_chTable[i].evadcGroup]);

        chCfg.channelId      = (IfxEvadc_ChannelId)s_chTable[i].evadcChannel;
        chCfg.resultRegister = (IfxEvadc_ChannelResult)s_chTable[i].resultReg;

        IfxEvadc_Adc_initChannel(&s_channels[i], &chCfg);

        s_lastMv[i] = 0u;
    }

    s_initialised = TRUE;
    Debug_Printf("[VMON] Init complete: %u channels across %u groups.\r\n",
                 (unsigned)VOLTMON_CH_COUNT, (unsigned)VOLTMON_NUM_GROUPS);
}

void VoltMon_RegisterFaultCb(VoltMon_FaultCb_t cb)
{
    s_faultCb = cb;
}

void VoltMon_Scan(void)
{
    uint8 i;
    uint8 g;
    Ifx_EVADC_G_RES convResult;

    if (!s_initialised)
    {
        return;
    }

    /* Clear and reload all group queues */
    for (g = 0u; g < VOLTMON_NUM_GROUPS; g++)
    {
        IfxEvadc_Adc_clearQueue(&s_groups[g], IfxEvadc_RequestSource_queue0);
    }

    /* Load all channels into their group queues and start conversion */
    for (i = 0u; i < (uint8)VOLTMON_CH_COUNT; i++)
    {
        IfxEvadc_Adc_addToQueue(&s_channels[i],
                                 IfxEvadc_RequestSource_queue0,
                                 IFXEVADC_QUEUE_REFILL);
    }

    for (g = 0u; g < VOLTMON_NUM_GROUPS; g++)
    {
        IfxEvadc_Adc_startQueue(&s_groups[g],
                                 IfxEvadc_RequestSource_queue0);
    }

    /* Brief wait for conversions to complete.
     * At ~0.5µs per conversion, 20 channels ≈ 10µs.
     * 1ms delay provides margin for multiplexer settling and
     * worst-case conversion time across all groups. */
    Stm_DelayMs(1u);

    /* Read results and check thresholds */
    for (i = 0u; i < (uint8)VOLTMON_CH_COUNT; i++)
    {
        /* getResult returns valid flag in the result struct */
        convResult = IfxEvadc_Adc_getResult(&s_channels[i]);

        if (convResult.B.VF == 1u)   /* Valid flag set */
        {
            s_lastMv[i] = prv_CountsToRailMv(
                (uint16)(convResult.B.RESULT),
                s_chTable[i].dividerScale);

            prv_CheckThresholds(i, s_lastMv[i]);
        }
    }
}

uint16 VoltMon_GetLastMv(uint8 chIdx)
{
    if (chIdx < (uint8)VOLTMON_CH_COUNT)
    {
        return s_lastMv[chIdx];
    }
    return 0u;
}

uint8 VoltMon_GetChannelCount(void)
{
    return (uint8)VOLTMON_CH_COUNT;
}