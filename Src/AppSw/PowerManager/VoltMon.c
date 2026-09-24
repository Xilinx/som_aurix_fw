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
#include "PowerManager.h"
#include "Ipc.h"
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

#ifndef IFXEVADC_QUEUE_REFILL
#define IFXEVADC_QUEUE_REFILL  (1u)
#endif

/* Bounded wait for CPU2 to ack a scan-stop request before PowerManager
 * (CPU1) proceeds with rail teardown — must never block indefinitely. */
#define VOLTMON_STOP_ACK_TIMEOUT_MS   50u


#define VOLTMON_CH_COUNT  (sizeof(s_chTable) / sizeof(s_chTable[0]))

static IfxEvadc_Adc          s_evadc;
static boolean s_voltMonEnabled = FALSE;

/* clang-format off */

#if defined(TARGET_EVAL_BOARD)
/*
 * TRB eval board — no SOM power rails present.
 * Monitor the 6 filtered ADC channels (4.7K + 47nF on the TRB)
 * to validate the full EVADC driver path.
 *
 * Thresholds wide open (0 / 5000) — no false faults on floating pins.
 * Readings are printed periodically for validation.
 *
 * TC387 EVADC group mapping:
 *   AN0-AN7   = Group 0
 *   AN16-AN23 = Group 2
 *   AN24-AN31 = Group 3
 *   AN40-AN47 = Group 5
 */
static const VoltMon_ChCfg_t s_chTable[] =
{
    /*  name          grp  ch  res  nom    uvW  uvF   ovW    ovF   div   */
    { "AN7_FILT",     0u, 7u, 7u,  0u,    0u,  0u, 5500u, 5500u, 1000u },
    { "AN20_FILT",    2u, 4u, 4u,  0u,    0u,  0u, 5500u, 5500u, 1000u },
    { "AN21_FILT",    2u, 5u, 5u,  0u,    0u,  0u, 5500u, 5500u, 1000u },
    { "AN31_FILT",    3u, 7u, 7u,  0u,    0u,  0u, 5500u, 5500u, 1000u },
    { "AN44_FILT",    5u, 4u, 4u,  0u,    0u,  0u, 5500u, 5500u, 1000u },
    { "AN45_FILT",    5u, 5u, 5u,  0u,    0u,  0u, 5500u, 5500u, 1000u },
};

/* Groups used: 0, 2, 3, 5 — array indexed by group number, so size = 6 */
#define VOLTMON_NUM_GROUPS   6u

#else /* TARGET_GP_SOM */

/*
 * Per-rail channel table.  Each row is a VoltMon_ChCfg_t:
 *
 *   { name, evadcGroup, evadcChannel, resultReg, nominalMv,
 *     uvWarnMv, uvFaultMv, ovWarnMv, ovFaultMv, dividerScale }
 *
 *   name          - rail name for logging
 *   evadcGroup    - EVADC group index (0-4)
 *   evadcChannel  - channel within group (0-15)
 *   resultReg     - result register index
 *   nominalMv     - nominal rail voltage, mV (reference only, not used
 *                   to derive the thresholds below — informational)
 *   uvWarnMv      - undervoltage warning threshold, mV
 *   uvFaultMv     - undervoltage fault threshold, mV
 *   ovWarnMv      - overvoltage warning threshold, mV
 *   ovFaultMv     - overvoltage fault threshold, mV
 *   dividerScale  - sense divider scale x1000 (e.g. 2:1 divider = 2000)
 *
 * UV/OV thresholds are independent literals per rail — update each field
 * directly; there is no relationship enforced between nominalMv and the
 * threshold columns.
 */
static const VoltMon_ChCfg_t s_chTable[] =
{
    /* ---- Group 0: VID rails (S0) ---------------------------------------- */
    { "VDDCR",       0u, 0u, 0u, 1100u,    0u,   0u,  1695u, 1700u, 1000u },
    { "VDDCR_CCD",   0u, 1u, 1u, 1100u,    0u,   0u,  1695u, 1700u, 1000u },
    { "VDDCR_SOC",   0u, 2u, 2u, 1000u,    0u,   0u,  1190u, 1350u, 1000u },
    { "VDDCR_SR",    0u, 3u, 3u,  950u,  600u, 550u,  1020u, 1070u, 1000u },
    /* ---- Group 1: Memory channel A (S0) --------------------------------- */
    { "VDD_MEM_A",    1u, 0u, 0u,  780u,  618u, 568u,   997u, 1047u, 1000u },
    { "VDD_MEMQ_A",   1u, 1u, 1u,  500u,  470u, 420u,   570u,  620u, 1000u },
    { "VDDIO_MEM_A",  1u, 2u, 2u, 1050u, 1010u, 960u,  1120u, 1170u, 1000u },

    /* ---- Group 2: Memory channel B (S0) --------------------------------- */
    { "VDD_MEM_B",    2u, 0u, 0u,  780u,  618u, 568u,   997u, 1047u, 1000u },
    { "VDD_MEMQ_B",   2u, 1u, 1u,  500u,  470u, 420u,   570u,  620u, 1000u },
    { "VDDIO_MEM_B",  2u, 2u, 2u, 1050u, 1010u, 960u,  1120u, 1170u, 1000u },

    /* ---- Group 3: Misc / S5 rails --------------------------------------- */
    { "VDD_MISC",     3u, 0u, 0u,  750u,  675u, 625u,   825u,  875u, 1000u },
    { "VDD_MISC_S5",  3u, 1u, 1u,  750u,  675u, 625u,   825u,  875u, 1000u },
    { "VDD_1V2",      3u, 2u, 2u, 1200u, 1164u, 1114u, 1236u, 1286u, 1000u },
    { "VDD_1V2_S5",   3u, 3u, 3u, 1200u, 1164u, 1114u, 1236u, 1286u, 1000u },
    { "VDD_1V8",      3u, 4u, 4u, 1800u, 1710u, 1660u, 1890u, 1940u, 1000u },
    { "VDD_1V8_S5",   3u, 5u, 5u, 1800u, 1710u, 1660u, 1890u, 1940u, 1000u },

    /* ---- Group 4: I/O rails --------------------------------------------- */
    { "VDDIO_3V3",    4u, 0u, 0u, 3300u, 3135u, 3085u, 3465u, 3515u, 1000u },
    { "VDDIO_3V3_S5", 4u, 1u, 1u, 3300u, 3135u, 3085u, 3465u, 3515u, 1000u },
    { "VDDIO_AUDIO",  4u, 2u, 2u, 1800u, 1710u, 1660u, 1890u, 1940u, 1000u },
};
/* clang-format on */


/* We need one group handle per EVADC group used (0-4).
 * TC387 has groups 0-11, but we only use 0-4. */
#define VOLTMON_NUM_GROUPS   5u

#endif

static IfxEvadc_Adc_Group    s_groups[VOLTMON_NUM_GROUPS];
static IfxEvadc_Adc_Channel  s_channels[VOLTMON_CH_COUNT];

/* Last measured values in mV */
static uint16                s_lastMv[VOLTMON_CH_COUNT];
static VoltMon_FaultCb_t     s_faultCb = NULL_PTR;
static boolean               s_initialised = FALSE;

#if (VOLTMON_SMA_ENABLE == 1u)
typedef struct
{
    uint16  buf[VOLTMON_SMA_TAPS];
    uint8   idx;
    uint8   count;
} VoltMon_SmaFilter_t;

static VoltMon_SmaFilter_t s_smaFilter[VOLTMON_CH_COUNT];

static uint16 prv_SmaFilter(VoltMon_SmaFilter_t *f, uint16 rawMv)
{
    uint32 sum;
    uint8  i;

    f->buf[f->idx] = rawMv;
    f->idx = (f->idx + 1u) % VOLTMON_SMA_TAPS;

    if (f->count < VOLTMON_SMA_TAPS)
    {
        f->count++;
        return rawMv;
    }

    sum = 0u;
    for (i = 0u; i < VOLTMON_SMA_TAPS; i++)
    {
        sum += f->buf[i];
    }

    return (uint16)(sum >> VOLTMON_SMA_SHIFT);
}
#endif

/* ---- ADC to mV conversion ----------------------------------------------- */

static uint16 prv_CountsToRailMv(uint16 counts, uint16 dividerScale)
{
    /* ADC voltage = counts * VREF / ADC_MAX
     * Rail voltage = ADC_voltage * dividerScale / 1000
     * Combined: rail_mV = counts * VREF_mV * dividerScale / (ADC_MAX * 1000) */
//  uint32 num;
//  num = (uint32)counts * VOLTMON_VREF_MV;

    uint64 num;
    num = (uint64)counts * VOLTMON_VREF_MV;
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

/* ---- Sample state reset -------------------------------------------------- */

/* Clears cached readings and SMA filter history so a stale/latched value
 * can't be reported as an active fault while scanning is gated off, and
 * so the filter doesn't blend post-restart samples with pre-outage ones. */
static void prv_ClearSamples(void)
{
    uint8 i;

    for (i = 0u; i < (uint8)VOLTMON_CH_COUNT; i++)
    {
        s_lastMv[i] = 0u;
    }
#if (VOLTMON_SMA_ENABLE == 1u)
    for (i = 0u; i < (uint8)VOLTMON_CH_COUNT; i++)
    {
        s_smaFilter[i].idx   = 0u;
        s_smaFilter[i].count = 0u;
    }
#endif
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
    }
    prv_ClearSamples();
    s_initialised = TRUE;
    Debug_Printf("[VMON] Init complete: %u channels across %u groups.\r\n",
                 (unsigned)VOLTMON_CH_COUNT, (unsigned)VOLTMON_NUM_GROUPS);
}

void VoltMon_RegisterFaultCb(VoltMon_FaultCb_t cb)
{
    s_faultCb = cb;
}

void VoltMon_Enable(void)
{
    s_voltMonEnabled = TRUE;
    Debug_Print("[VMON] Monitoring enabled\r\n");
}

void VoltMon_Disable(void)
{
    s_voltMonEnabled = FALSE;

    /* Block (bounded) until CPU2 confirms it has actually stopped
     * scanning before returning — so PowerManager doesn't start
     * disabling rail groups while CPU2 is mid-conversion. */
    if (!Ipc_RequestVoltMonStopWait(VOLTMON_STOP_ACK_TIMEOUT_MS))
    {
        Debug_Print("[VMON] WARN: CPU2 did not ack scan stop\r\n");
    }

    /* Safe now — CPU2 has acked, so no in-flight scan can clobber this. */
    prv_ClearSamples();

    Debug_Print("[VMON] Monitoring disabled\r\n");
}



void VoltMon_Scan(void)
{
    if (!s_initialised || !s_voltMonEnabled ||
    (g_ipcShared.pmc.pmState != (uint32)PM_STATE_ON))
    {
        Ipc_AckVoltMonStop();
        return;
    }

    uint8 i;
    uint8 g;
    Ifx_EVADC_G_RES convResult;

    if (!s_initialised || !s_voltMonEnabled)
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

        if (convResult.B.VF == 1u)
        {
            uint16 rawMv = prv_CountsToRailMv(
                (uint16)(convResult.B.RESULT),
                s_chTable[i].dividerScale);

#if (VOLTMON_SMA_ENABLE == 1u)
            s_lastMv[i] = prv_SmaFilter(&s_smaFilter[i], rawMv);
#else
            s_lastMv[i] = rawMv;
#endif
            prv_CheckThresholds(i, s_lastMv[i]);

            /*
            Debug_Printf("[VMON] %s = %u current:%umV (%umV ~ %umV)",
                         s_chTable[i].name,
                         convResult.B.RESULT,
                         s_lastMv[i],
                         s_chTable[i].uvFaultMv,
                         s_chTable[i].ovFaultMv
                         );
            if (s_lastMv[i] < s_chTable[i].uvFaultMv || s_lastMv[i] > s_chTable[i].ovFaultMv)
                Debug_Printf(" [*]");
            Debug_Printf("\r\n");
            */

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


#if defined(TARGET_EVAL_BOARD)
void VoltMon_PrintReport(void)
{
    uint8 i;
    Debug_Print("[VMON] --- ADC Report ---\r\n");
    for (i = 0u; i < (uint8)VOLTMON_CH_COUNT; i++)
    {
        Debug_Print("[VMON] ");
        Debug_Print(s_chTable[i].name);
        Debug_Print(" = ");
        /* Print mV as decimal manually to avoid Debug_Printf float issues */
        {
            char buf[16];
            uint16 mv = s_lastMv[i];
            int len = 0;
            if (mv == 0u)
            {
                buf[len++] = '0';
            }
            else
            {
                char tmp[8];
                int tl = 0;
                while (mv > 0u)
                {
                    tmp[tl++] = '0' + (mv % 10u);
                    mv /= 10u;
                }
                while (tl > 0)
                {
                    buf[len++] = tmp[--tl];
                }
            }
            buf[len] = '\0';
            Debug_Print(buf);
        }
        Debug_Print(" mV\r\n");
    }
    Debug_Print("[VMON] -----------------\r\n");
}
#endif

boolean VoltMon_AnyFaultActive(void)
{
    uint32 ch;
    for (ch = 0u; ch < (uint32)VOLTMON_CH_COUNT; ch++)
    {
        if (s_lastMv[ch] == 0u)
            continue;  /* Unconfigured channel */

        if (s_lastMv[ch] < s_chTable[ch].uvFaultMv ||
            s_lastMv[ch] > s_chTable[ch].ovFaultMv)
        {
            return TRUE;
        }
    }
    return FALSE;
}
