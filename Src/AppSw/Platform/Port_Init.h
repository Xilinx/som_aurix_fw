/**
 * @file    Port_Init.h
 * @brief   GPIO port direction and pull-resistor initialisation.
 */

#ifndef PORT_INIT_H
#define PORT_INIT_H

/**
 * @brief Configure all board GPIO pins defined in Platform_PinCfg.h.
 *        Must be called after Clk_Init() and before any module that
 *        reads or drives a GPIO.
 */
void Port_Init(void);

#endif /* PORT_INIT_H */
