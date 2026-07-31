/**
 * @file    Port_Init.c
 * @brief   GPIO direction and pull-resistor configuration for all board pins.
 *
 * Uses AppPin_t and AppPin_GetPort() — no IfxPort_Pxx identifiers,
 * no Tasking E272/E306/E333.
 */

#include "Port_Init.h"
#include "Platform_PinCfg.h"
#include "AppPin.h"
#include "IfxPort.h"

static void initOutput(const AppPin_t *pin, boolean initHigh)
{
    Ifx_P *port = AppPin_GetPort(pin->portIdx);
    IfxPort_setPinModeOutput(port, pin->pinIdx,
                             IfxPort_OutputMode_pushPull,
                             IfxPort_OutputIdx_general);
    if (initHigh)
        IfxPort_setPinHigh(port, pin->pinIdx);
    else
        IfxPort_setPinLow(port, pin->pinIdx);
}

/**
 * Open-drain output: driving HIGH releases the line (external pull-up holds it
 * high); driving LOW actively pulls the line to GND.  Both the TC387 and an
 * external device (e.g. the APU on APU_PROCHOT_L) can assert the line LOW
 * without bus contention.  IfxPort_getPinState() reflects the actual pad
 * voltage, so a LOW driven by either side is observable.
 */
static void initOutputOpenDrain(const AppPin_t *pin, boolean initHigh)
{
    Ifx_P *port = AppPin_GetPort(pin->portIdx);
    IfxPort_setPinModeOutput(port, pin->pinIdx,
                             IfxPort_OutputMode_openDrain,
                             IfxPort_OutputIdx_general);
    if (initHigh)
        IfxPort_setPinHigh(port, pin->pinIdx);
    else
        IfxPort_setPinLow(port, pin->pinIdx);
}

static void initInputPD(const AppPin_t *pin)
{
    IfxPort_setPinModeInput(AppPin_GetPort(pin->portIdx),
                            pin->pinIdx, IfxPort_InputMode_pullDown);
}

static void initInputPU(const AppPin_t *pin)
{
    IfxPort_setPinModeInput(AppPin_GetPort(pin->portIdx),
                            pin->pinIdx, IfxPort_InputMode_pullUp);
}

static void initInputNP(const AppPin_t *pin)
{
    IfxPort_setPinModeInput(AppPin_GetPort(pin->portIdx),
                            pin->pinIdx, IfxPort_InputMode_noPullDevice);
}

void Port_Init(void)
{
    /* ---- VRM Enable Outputs — LOW (disabled) at startup ----------------- */
    initOutput(&PIN_MAIN_12V_EFUSE_EN, FALSE);
    initOutput(&PIN_PWR_GROUP_B_EN,    FALSE);
    initOutput(&PIN_PWR_GROUP_C_EN,    FALSE);
    initOutput(&PIN_PWR_GROUP_D_EN,    FALSE);

    /* ---- VRM Power Good Inputs — pull-down (PG is active high) ---------- */
    initInputPD(&PIN_MAIN_12V_EFUSE_PG);
    initInputNP(&PIN_VR_APU_3V3_PG);
    initInputPD(&PIN_VDD_MISC_PG);
    initInputPD(&PIN_VDD_1V2_PG);
    initInputPD(&PIN_VDD_1V8_PG);
    initInputPD(&PIN_VDD_MEMQ_CHA_PG);
    initInputPD(&PIN_VDD_MEMQ_CHB_PG);
    initInputNP(&PIN_VDDIO_MEM_CHA_PG);
    initInputNP(&PIN_VDDIO_MEM_CHB_PG);
    initInputPD(&PIN_VDD_MEM_CHA_PG);
    initInputPD(&PIN_VDD_MEM_CHB_PG);
    initInputPD(&PIN_MP2825A_1_PG);
    initInputPD(&PIN_MP2825A_2_PG);
    initInputPD(&PIN_VDDCR_PG);

    /* ---- APU State Control — Outputs ------------------------------------ */
    initOutput(&PIN_APU_PWR_GOOD,   FALSE);
    initOutput(&PIN_COLD_RST,       FALSE);
    initOutput(&PIN_WARM_RST,       FALSE);
    initOutput(&PIN_MMC_RSMRST_L,   FALSE);
    initOutput(&PIN_APU_PWRBTN,     TRUE);
    initOutput(&PIN_PLTRST_L,       FALSE);
    /* APU_PROCHOT_L: open-drain so both TC387 and APU can assert LOW without
     * contention.  SysMonitor reads pad state to detect APU-side assertion. */
    initOutputOpenDrain(&PIN_APU_PROCHOT_L, TRUE);

    /* ---- APU State Control — Inputs ------------------------------------- */
    initInputPD(&PIN_SLP_S3);
    initInputPD(&PIN_SLP_S5);
    initInputPD(&PIN_APU_PWROK);
    initInputPU(&PIN_APU_PCC_L);
    initInputNP(&PIN_APU_RESET_IN_L);

    /* ---- COM-HPC Carrier Inputs ----------------------------------------- */
    initInputPU(&PIN_CB_PWRBTN_L);
    initInputPU(&PIN_CB_RSTBTN_L);
    initInputPD(&PIN_VIN_PWR_OK);
    initInputPD(&PIN_CB_AC_PRESENT);
    initInputPU(&PIN_CB_BATLOW_L);

    /* ---- COM-HPC General (Port 20) --------------------------------------- */
    initInputPU(&PIN_SLEEP_L);
    initInputPD(&PIN_RAPID_SD);
    initInputPU(&PIN_LID_L);
    initInputPU(&PIN_TAMPER_L);
    initInputNP(&PIN_WD_STROBE_L);
    initOutput(&PIN_RSMRST_OUT_L, FALSE);
    initOutput(&PIN_WD_OUT,       FALSE);

    /* ---- FuSa Outputs (Port 2) — safe defaults -------------------------- */
    initOutput(&PIN_FUSA_SPI_ALERT,     FALSE);
    initOutput(&PIN_FUSA_ALERT_L,       TRUE);
    initOutput(&PIN_FUSA_VOLTAGE_ERR_L, TRUE);
    initOutput(&PIN_FUSA_STATUS0,       FALSE);
    initOutput(&PIN_FUSA_STATUS1,       FALSE);
    initOutput(&PIN_PROCHOT_L,          TRUE);
    initOutput(&PIN_CATERR_L,           TRUE);

    /* ---- Thermal / ERU Inputs (Port 10) --------------------------------- */
    initInputPU(&PIN_THERMTRIP_L);
    initInputPU(&PIN_CARRIER_HOT);
    initInputPU(&PIN_USBC_PD_ALERT_L);
    initOutput(&PIN_USBC_PD_INT_TO_APU, FALSE);

    /* ---- Fan Control (Port 10) ------------------------------------------ */
    initInputNP(&PIN_FAN_TACHIN);
    initOutput(&PIN_FAN_PWM, FALSE);

    /* ---- Boot Select (Port 22) ------------------------------------------ */
    initInputPD(&PIN_BSEL_0);
    initInputPD(&PIN_BSEL_1);
    initInputPD(&PIN_BSEL_2);
    initOutput(&PIN_APU_ROM_SPI_SEL, FALSE);

    /* ---- DisplayPort HPD (Port 13) -------------------------------------- */
    initOutput(&PIN_DP2_HPD, FALSE);
    initOutput(&PIN_DP3_HPD, FALSE);

    /* ---- TLF35585 PMIC GPIO (Port 33) ----------------------------------- */
    initInputNP(&PIN_TLF_ERR);
    initInputNP(&PIN_TLF_SS);
    initInputNP(&PIN_TLF_WAKE);
    initOutput(&PIN_TLF_WDI, FALSE);

    /* ---- UART MUX (Port 14) --------------------------------------------- */
    /* HIGH = AURIX owns ASCLIN0 (P14.0/P14.1).
     * AURIX holds the UART from power-on until SYS_RESET_L is released,
     * at which point PowerManager switches to LOW (x86 SoC). */
    initOutput(&PIN_UART_MUX_SEL, TRUE);

    /* ---- APML Alert (Port 11) ------------------------------------------- */
    initInputPU(&PIN_APML_ALERT);

    /* ---- Blink LED (Port 34) -------------------------------------------- */
    initOutput(&PIN_BLINK, FALSE);
}
