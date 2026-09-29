/**
 * @file    Clk_Cfg.h
 * @brief   SCU clock and PLL initialisation interface.
 */

#ifndef CLK_CFG_H
#define CLK_CFG_H

/**
 * @brief Configure the SCU PLL for 300 MHz CPU clock.
 *        Disables the software watchdog before reconfiguring clocks and
 *        re-enables safety watchdog (ENDINIT) afterwards.
 *        Must be the first call in Cpu0_Main().
 */
void Clk_Init(void);

#endif /* CLK_CFG_H */
